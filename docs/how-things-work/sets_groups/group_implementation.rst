.. _group-implementation-label:

Group Operations: Theory and Implementation
===========================================

.. note::

   This is a *developer-level* specification. It describes how the
   ``PMIx_Group_*`` family is realized inside ``libpmix`` — the client-side
   and server-side machinery, the two-phase collective model, and the
   fault-tolerance accounting that keeps a group operation from hanging when
   a participant is lost. The user-facing semantics are documented in
   :ref:`Group Construction, Destruction, and Fault Tolerance
   <group-construction-label>`; the general server-side collective-tracking
   model that the group family reuses is specified in
   :doc:`../collectives/tracking_spec`.

Audience: contributors modifying the group code, the collective-tracking
service, or a host environment's ``pmix_server_module_t.group`` callback.


Two realizations of "group construction"
----------------------------------------

The group family is implemented as **two distinct mechanisms**, chosen per
API rather than per group:

* **Round-trip collectives.** ``PMIx_Group_construct`` and
  ``PMIx_Group_destruct`` are true collectives. Each participant's client
  library packs a command and sends it to its local server with
  ``PMIX_PTL_SEND_RECV``; the server assembles the local participants,
  forwards the aggregated operation to the host for global completion, and
  replies to each local participant. Commands: ``PMIX_GROUP_CONSTRUCT_CMD``
  and ``PMIX_GROUP_DESTRUCT_CMD``.

* **Event-driven handshakes.** ``PMIx_Group_invite``, ``PMIx_Group_join``,
  and ``PMIx_Group_leave`` send **no** server command. They are realized
  entirely through the event-notification subsystem (``PMIx_Notify_event``),
  with the server's only role being to relay events between its clients and
  the host. This is why the invite method works without any host collective
  support, and why ``PMIx_Group_leave`` was re-implemented as an event
  (its former ``PMIX_GROUP_LEAVE_CMD`` round-trip was never handled by any
  server switchyard and has been removed).

The code lives in two files:

* ``src/client/pmix_client_group.c`` — all six client API pairs, the client
  group tracker, and the client-side fault handling (invite timeout/decline,
  construct abort, leader-failure watch).
* ``src/server/pmix_server_group.c`` — the server block/tracker structures,
  the local-phase assembly and completion predicate, host handoff, and the
  server-side fault accounting.

Shared membership helpers used by the collectives live in
``src/client/pmix_client_convert.c``.


Client side
-----------

The group tracker
^^^^^^^^^^^^^^^^^

A single caddy type, ``pmix_group_tracker_t`` (in
``pmix_client_group.c``), carries state across the progress-thread boundary
for every API in the family. It is deliberately overloaded — its meaning
shifts with the operation — but always carries the standard caddy machinery
(``ev``, ``lock``, ``status``, ``cbdata``, cached ``cbfunc``/``opcbfunc``)
plus the group identity (``grpid``, ``members``/``nmembers``). For the
event-driven paths it additionally holds the per-member bookkeeping used by
the invite/join handshake (``responded[]``, ``answered[]``, ``nanswered``,
``optional``), the timeout state (``timer_active``), a one-shot resolution
guard (``completed``), and the event-handler registration id (``ref``, seeded
to ``SIZE_MAX`` so teardown can tell whether a handler was ever registered).

Construct and destruct (round-trip)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``PMIx_Group_construct_nb`` does the real work; the blocking form is a thin
wrapper that supplies an internal callback and waits on the caddy lock. The
non-blocking form:

#. **Expands and validates membership.** When the participants are named
   explicitly (not the add-members or bootstrap methods), the ``procs`` array
   is run through ``pmix_client_convert_group_procs()`` so any embedded PMIx
   *group* identifier is replaced by its member processes, and the caller is
   checked for inclusion via ``pmix_client_proc_is_included()``. A caller
   that is not among the expanded participants gets ``PMIX_ERR_NOT_A_MEMBER``
   immediately. Both steps are skipped when ``PMIX_GROUP_ADD_MEMBERS`` or
   ``PMIX_GROUP_BOOTSTRAP`` is present, since those intentionally omit
   participants.

#. **Packs the message** (``construct_msg()``): the command, group ID,
   participant array, and info array, folding in the caller's own endpoint
   data (fetched at ``PMIX_REMOTE`` scope and packed as ``PMIX_PROC_DATA``)
   and prepending the caller's proc ID to any ``PMIX_GROUP_INFO`` array.

#. **Dispatches** with ``PMIX_PTL_SEND_RECV(..., construct_cbfunc, cb)`` and
   returns.

``construct_cbfunc`` unpacks the returned status, final membership, optional
context ID, and group info, registers the completed group in
``pmix_client_globals.groups`` (sorted for a consistent cross-participant
view), and delivers the results. ``PMIx_Group_destruct_nb`` mirrors this with
``PMIX_GROUP_DESTRUCT_CMD``, sending the membership it holds locally (the
server does not store it) and removing the group from the local list on
completion.

``pmix_client_proc_is_included()`` (in ``pmix_client_convert.c``) is the
shared "is the caller covered by this participant array?" predicate — it
matches the caller's namespace and accepts ``PMIX_RANK_WILDCARD``,
``PMIX_RANK_LOCAL_NODE``, ``PMIX_RANK_LOCAL_PEERS``, or an exact rank. It was
hoisted out of duplicate copies in the fence and connect/disconnect paths so
all three collectives share one membership check.

Invite / join / leave (event-driven)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The invite method tracks invitees **by identity**. ``invite_setup()``
expands wildcard ranks into concrete members, builds parallel
``responded[]`` (accepters) and ``answered[]`` (accepters *and* decliners
*and* terminated) flag arrays, seeds the leader's own slot, and registers a
prepended handler for ``PMIX_GROUP_INVITE_ACCEPTED``,
``PMIX_GROUP_INVITE_DECLINED``, and ``PMIX_PROC_TERMINATED``. It then notifies
the invitees with ``PMIX_GROUP_INVITED`` over a custom range, and, if a
``PMIX_TIMEOUT`` was supplied, arms a libevent timer.

The critical thread-safety rule: the response handler (``invite_handler``)
and the timeout handler run on the **progress thread** and must never raise
an event themselves (that would deadlock). They only *wake* the operation
(``invite_wake``, guarded by the ``completed`` one-shot). All event
generation — ``PMIX_GROUP_INVITE_FAILED`` for non-accepters, the
``PMIX_GROUP_CONSTRUCT_COMPLETE`` broadcast, or a ``PMIX_GROUP_CONSTRUCT_ABORT``
— happens afterward on the caller's thread in ``invite_announce()``. A
termination event names the departed proc in ``PMIX_EVENT_AFFECTED_PROC``
(not the event source), so ``invite_handler`` attributes it to the right
member via that key.

``PMIx_Group_join_nb`` maps the ``pmix_group_opt_t`` to an event code
(``PMIX_GROUP_INVITE_ACCEPTED`` / ``PMIX_GROUP_INVITE_DECLINED``) and notifies
the leader. On an *accept* with a known leader it also arms the leader-failure
watch (below).

``PMIx_Group_leave_nb`` finds the group locally, drops it from the local list
immediately, and generates a ``PMIX_GROUP_LEFT`` event ranged to the
membership excluding self (naming self in ``PMIX_EVENT_AFFECTED_PROC``). Per
its contract it returns once the event is *locally generated*. Remaining
members update their local membership when they receive ``PMIX_GROUP_LEFT``,
handled in ``pmix_invoke_local_event_hdlr`` (``src/event/pmix_event_notification.c``)
alongside the existing group-construct-complete membership handling.

The leader-failure watch
^^^^^^^^^^^^^^^^^^^^^^^^^

When a process accepts an invitation, ``setup_leader_watch()`` registers a
private, non-blocking, prepended handler keyed on leader termination
(``PMIX_PROC_TERMINATED``, ``PMIX_ERR_PROC_ABORTED``,
``PMIX_ERR_PROC_TERM_WO_SYNC``) and on construct resolution
(``PMIX_GROUP_CONSTRUCT_COMPLETE`` / ``PMIX_GROUP_CONSTRUCT_ABORT``). If the
watched leader (held in ``members[0]``) is lost before the construct
resolves, the handler emits ``PMIX_GROUP_LEADER_FAILED`` to this process's own
handlers via **non-blocking** ``PMIx_Notify_event`` (the blocking form would
deadlock on the progress thread) and tears itself down. It always returns
``PMIX_EVENT_NO_ACTION_TAKEN`` so it never swallows the events the application
registered to see. Reselection is entirely application-driven: a process
nominates itself by returning ``PMIX_GROUP_LEADER`` in its handler's results,
and the outcome is announced with ``PMIX_GROUP_LEADER_SELECTED``.


Server side
-----------

Block and tracker structures
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Group collectives use a **two-level** tracker (unlike the flat
``pmix_server_trkr_t`` shared by fence/connect/disconnect), because a single
group construct can be assembled from several distinct call signatures
(leader, followers, bootstrap) and carries group-only state:

* ``grp_block_t`` — one per group-ID operation, on the server-global list
  ``pmix_server_globals.grp_collectives`` (declared in
  ``src/server/pmix_server_ops.h``). It owns: the operation kind ``grpop``
  (a ``pmix_group_operation_t``, initialized to ``PMIX_GROUP_NONE``); the
  aggregated membership (``pcs``/``npcs``) and directives (``info``/``ninfo``)
  built by ``aggregate_info``; the list ``mbrs`` of ``grp_trk_t``, **one per
  participant call** (its size is the number of local contributions); the
  list ``departed`` of processes lost before contributing; ``nlocal`` (the
  expected local count) and ``def_complete`` (all participating namespaces
  registered, so ``nlocal`` is final); ``host_called`` (the local phase is
  forwarded and frozen); ``need_cxtid``; and the local-phase timeout state
  (``ev``, ``event_active``).

* ``grp_trk_t`` — one per participant call, hung off a block's ``mbrs``. It
  holds the membership and directives *as passed by that participant*, a
  back-pointer to its block, and ``local_cbs`` — the list of
  ``pmix_server_caddy_t`` that both identifies **who actually contributed**
  (by peer name) and holds the reply destination for each local participant.

* ``grp_shifter_t`` — the thread-shift caddy carrying the host's completion
  result back onto the progress thread.

Local-phase assembly and completion
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_server_group()`` receives a construct/destruct command on the progress
thread. It requires a host ``group`` callback (else ``PMIX_ERR_NOT_SUPPORTED``),
unpacks the group ID / procs / directives, seeds a default
``PMIX_LOCAL_COLLECTIVE_STATUS = PMIX_SUCCESS`` slot as the last element of
the local info array (the slot the fault machinery later overwrites), and
locates or creates the block via ``get_tracker()``. A call with ``nprocs ==
0`` is a *follower*; a ``PMIX_GROUP_BOOTSTRAP`` call is a *bootstrap* leader —
both keep independent trackers (the leader is unknown) and are forwarded to
the host immediately, one call per participant, without local aggregation.

For the collective (fully-specified) method:

* ``check_definition_complete()`` walks all trackers' member procs, verifies
  every participating namespace is known and fully registered, and — once so
  — sets ``def_complete`` and the final ``nlocal`` (wildcards contribute the
  namespace's local process count). Until every namespace is registered the
  block stays pending, so a client that calls in before its local
  ``register_client`` event cannot complete the collective early.

* ``grp_blk_locally_complete()`` is the **single completion predicate**::

      def_complete  AND  (size(mbrs) + size(departed)) >= nlocal

  This is the group analog of ``pmix_server_trk_complete()`` used by the
  fence family. The ``>=`` (rather than ``==``) tolerates fork/exec clones
  that share a rank and over-count. The local phase is complete once every
  expected local participant has either **contributed** (a tracker on
  ``mbrs``) or **departed**.

* When the predicate holds and the block has not already been forwarded,
  ``aggregate_info()`` merges every tracker's procs/info into the block
  (deduplicating and special-casing ``PMIX_GROUP_ADD_MEMBERS``,
  ``PMIX_GROUP_BOOTSTRAP``, ``PMIX_PROC_DATA``, and ``PMIX_GROUP_INFO``), the
  fault decision below is applied, the local-phase timer is deleted,
  ``host_called`` is set, and the operation is forwarded to
  ``pmix_host_server.group()``.

The host's completion callback ``grpcbfunc`` thread-shifts into
``_grpcbfunc``, which extracts the context ID, final membership, and group
info from the host result, stores group info via
``pmix_server_process_grpinfo``, and — for every block matching the ID
(bootstrap can create several) — packs each tracker's ``local_cbs`` reply and
queues it, then removes and releases the block.


Fault tolerance: identity-based accounting
------------------------------------------

The group family reuses the identity-based collective-tracking model
specified in :doc:`../collectives/tracking_spec`. The essential idea is that
local participation must be tracked **by participant identity**, never by a
bare counter, so the lost-connection handler can answer the one question a
counter cannot: *"has this specific participant already contributed?"* The
completion predicate counts ``contributed ∪ departed`` against ``expected``;
the two lost-participant rules are:

* **Case A — the participant had already contributed.** Ignore the loss for
  this collective. The contribution stands, its data stays in the assembled
  result, and ``nlocal`` is not reduced. The dead peer's queued reply is
  harmless (its socket is closed). This is the rule that fixes the historical
  early-completion / data-loss bug.

* **Case B — the participant had not yet contributed.** It never will, so
  record it on ``departed`` (once, deduplicated). Completion is then
  re-evaluated; if the block is now locally complete it is forwarded exactly
  as on a normal final contribution.

Two entry points reach the same per-block routine ``account_departed()``:

* ``pmix_server_grp_peer_lost(peer)`` — called from ``lost_connection()`` in
  ``src/mca/ptl/base/ptl_base_sendrecv.c`` when a socket drops. A lost
  connection is not scoped to one group, so it applies ``account_departed``
  to **every** block on ``grp_collectives``. (Before this work the
  lost-connection handler walked only the fence-family ``collectives`` list,
  so a lost group member hung the construct forever.)

* ``pmix_server_grp_member_left(grpid, proc)`` — the deliberate cousin. A
  ``PMIx_Group_leave`` generates a ``PMIX_GROUP_LEFT`` event that the
  originating server intercepts in ``src/server/pmix_server_ops.c`` (before
  loop-detection, so only the originating server acts). It applies
  ``account_departed`` to the blocks for the **named** group only.

``account_departed()`` skips bootstrap/follower blocks (``nlocal == 0``) and
already-forwarded blocks (``host_called``); determines membership by matching
``proc`` against each tracker's ``pcs`` with ``PMIX_CHECK_NAMES`` and prior
contribution by matching against each tracker's ``local_cbs`` peer names
(Case A vs. Case B); and, if the loss completes the local phase, aggregates,
applies the fault decision, and forwards. Callers iterate with a
``FOREACH_SAFE`` macro because the block may be released on error.

The abort-vs-survive gate
^^^^^^^^^^^^^^^^^^^^^^^^^^

At each point where a *loss* (or leave) completes a block's local phase, the
server decides between aborting and surviving:

.. code-block:: c

   if (0 < pmix_list_get_size(&blk->departed)) {
       if (PMIX_GROUP_CONSTRUCT == op && !grp_ft_collective(blk)) {
           abort_construct(blk, PMIX_GROUP_CONSTRUCT_ABORT);
           return;
       }
       pmix_server_set_collective_status(blk->info, blk->ninfo,
                                         PMIX_ERR_LOST_CONNECTION);
   }

* ``grp_ft_collective(blk)`` scans the aggregated directives for
  ``PMIX_GROUP_FT_COLLECTIVE`` (``"pmix.grp.ftcoll"``, bool, default false).
* A **construct without the flag** aborts. A construct **with the flag**, or
  any **destruct**, survives on the survivors, recording
  ``PMIX_ERR_LOST_CONNECTION`` into the ``PMIX_LOCAL_COLLECTIVE_STATUS`` slot
  so the host is told the local phase was degraded (clients still receive
  their own status through ``grpcbfunc``).

``abort_construct(blk, status)`` deletes any armed timer; if a host is
present it issues ``pmix_host_server.group(PMIX_GROUP_CANCEL, blk->id, ...)``
with ``cancel_cbfunc``; sets ``host_called``; and completes the local
participants directly by driving ``grpcbfunc`` with the abort status. The
``PMIX_GROUP_CANCEL`` call is essential in the cross-server case: other
servers that already forwarded their local phase are waiting on the host
collective for a contribution this server will never send, so the host must
be told to unstick that collective. Because the block is removed locally, the
host's resulting abort release is a no-op back here — no double completion.
``abort_construct`` is only ever called pre-forward, so no host completion can
race it.

Synthesizing ``PMIX_GROUP_MEMBER_FAILED``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

On the *survive* path, ``notify_local_members_of_loss()`` (called from
``_grpcbfunc`` only when the construct succeeded, is a construct, and has a
non-empty ``departed`` list) emits, for each departed process, a
``PMIX_GROUP_MEMBER_FAILED`` event carrying ``PMIX_EVENT_AFFECTED_PROC`` (the
lost member) and ``PMIX_GROUP_ID``. It is delivered through the internal,
progress-thread-safe ``pmix_server_notify_client_of_event`` — never the
public ``PMIx_Notify_event``. Because each server does this accounting for
its own local survivors, a member lost on another node is reported by that
node's server performing the identical logic; the feature therefore needs no
cooperation from the host environment.

The local-phase timeout
^^^^^^^^^^^^^^^^^^^^^^^^^

A ``PMIX_TIMEOUT`` directive arms a libevent timer over the local phase via
``PMIX_THREADSHIFT_DELAY(blk, group_timeout, tmo)``, setting ``blk->ev`` and
``event_active``. ``group_timeout()`` fires ``abort_construct(blk,
PMIX_ERR_TIMEOUT)``. The timer is deleted before every host-forward (and in
the block destructor ``gbdes`` as an error-path safety net), so it can never
fire on an already-forwarded — and possibly freed — block. As with the fence
family, this timer covers only the *local* phase; the host owns the
cross-server timeout.

The collective-status slot
^^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_server_set_collective_status()`` (in ``src/server/pmix_server_ops.c``)
locates the ``PMIX_LOCAL_COLLECTIVE_STATUS`` entry **by key** (not by
position) and writes the status in place; an absent slot is a no-op. Finding
it by key matters for connect, which appends per-participant and job-level
info after the seeded slot — a positional write would corrupt that appended
data and leave the real status stale. The same helper is used by the
fence-family ``lost_connection`` path, both group host-forward points, and
the white-box unit test ``test/unit/collective_status.c``.


The host callback boundary
--------------------------

Group construct/destruct/cancel all cross the PMIx-to-host boundary through
one callback slot, ``pmix_server_module_t.group``, whose operation is a
``pmix_group_operation_t``:

.. code-block:: c

   typedef enum {
       PMIX_GROUP_CONSTRUCT,   /* 0 */
       PMIX_GROUP_DESTRUCT,    /* 1 */
       PMIX_GROUP_NONE,        /* 2 */
       PMIX_GROUP_CANCEL       /* 3 */
   } pmix_group_operation_t;

Because the enum's numeric values cross that boundary, new values **must** be
appended, never inserted — ``PMIX_GROUP_CANCEL`` was added at the end.
``PMIx_Group_operation_string()`` (in ``src/common/pmix_strings.c``)
stringifies each value. Two footguns to respect when editing this enum:

* Keep the body **free of comment lines**: the Python bindings generator
  ``bindings/python/construct.py`` treats every line inside a ``typedef
  enum`` as an enumerator, so an interleaved comment shifts every subsequent
  value. The explanatory comment therefore lives in a block above the
  ``typedef``.
* ``PMIX_GROUP_CANCEL`` is *not* carried on the client-to-server wire (it is
  purely a PMIx-to-host callback op), so no new ``bfrops`` support is needed.

Capability detection
^^^^^^^^^^^^^^^^^^^^^

The whole feature set above is advertised by the ``PMIX_CAP_GROUP_FT``
capability flag in ``include/pmix_version.h.in``. Its *presence* in the
installed ``pmix_version.h`` is the signal a consumer (e.g. PRRTE, via
``PRTE_CHECK_PMIX_CAP``) tests for; absence means the running PMIx predates
group fault tolerance. Edit the ``.in`` template, never the generated header.


Event and attribute reference
-----------------------------

Events (defined in ``include/pmix_common.h.in``):

============================================  ======  ================================================================
Event                                         Value   Role
============================================  ======  ================================================================
``PMIX_GROUP_INVITED``                        -159    Leader → invitees; carries ``PMIX_GROUP_ID``
``PMIX_GROUP_LEFT``                           -160    Departing proc → members; server intercepts to account the leave
``PMIX_GROUP_INVITE_ACCEPTED``                -161    Invitee → leader (``PMIx_Group_join`` accept)
``PMIX_GROUP_INVITE_DECLINED``                -162    Invitee → leader (``PMIx_Group_join`` decline)
``PMIX_GROUP_INVITE_FAILED``                  -163    Leader-local report of a non-accepter
``PMIX_GROUP_MEMBERSHIP_UPDATE``              -164    Membership change notification
``PMIX_GROUP_CONSTRUCT_ABORT``                -165    Construct aborted; all participants notified
``PMIX_GROUP_CONSTRUCT_COMPLETE``             -166    Group formed; carries final ``PMIX_GROUP_MEMBERSHIP``
``PMIX_GROUP_LEADER_SELECTED``                -167    App-driven reselection result
``PMIX_GROUP_LEADER_FAILED``                  -168    Leader lost; surfaced to a joining member's handlers
``PMIX_GROUP_CONTEXT_ID_ASSIGNED``            -169    Context ID assigned
``PMIX_GROUP_MEMBER_FAILED``                  -170    Server-synthesized on a lost member (survive path)
============================================  ======  ================================================================

Key attributes / status codes:

* ``PMIX_GROUP_FT_COLLECTIVE`` (``"pmix.grp.ftcoll"``, bool) — the
  survive-vs-abort gate for a lost member during construct.
* ``PMIX_GROUP_OPTIONAL`` (bool) — all-or-nothing (default) vs. reduced group
  for the invite method.
* ``PMIX_GROUP_NOTIFY_TERMINATION`` (bool) — report vs. error a member lost
  mid-destruct.
* ``PMIX_GROUP_LEADER`` (bool) — declares the construct leader; also returned
  from a leader-failed handler to nominate a replacement.
* ``PMIX_LOCAL_COLLECTIVE_STATUS`` (``pmix_status_t``) — the per-tracker
  health slot the fault machinery updates by key.
* ``PMIX_ERR_NOT_A_MEMBER`` — returned when the caller is not among the
  expanded construct participants.
* ``PMIX_ERR_TIMEOUT`` — local-phase timeout expiry.
* ``PMIX_ERR_LOST_CONNECTION`` / ``PMIX_ERR_PARTIAL_SUCCESS`` — degraded
  local-collective status recorded for the host.


Testing
-------

The behaviors above are exercised at several levels:

* ``test/unit/collective_status.c`` — white-box coverage of the by-key status
  helper (connect/fence layouts, absent slot, degenerate inputs), wired into
  ``make check``.
* ``test/simple`` drivers — ``simpgrpdie`` (survive on loss +
  ``PMIX_GROUP_MEMBER_FAILED``, with ``PMIX_GROUP_FT_COLLECTIVE``),
  ``simpgrpcabort`` (default abort with ``PMIX_GROUP_CONSTRUCT_ABORT``), and
  ``simpgrpctimeout`` (``PMIX_ERR_TIMEOUT`` on a hung live member).
* ``examples/`` exercisers driven by the dockerswarm harness
  (``contrib/dockerswarm/run-group-events.sh``) across a multi-node swarm, so
  the cross-server notification and cancel paths actually run:
  ``group_invite`` (happy path), ``group_invite_timeout`` /
  ``group_invite_decline`` / ``group_invite_abort`` (invite fault paths),
  ``group_destruct_die`` and ``group_die`` (loss mid-destruct/construct),
  ``group_construct_abort`` (lost member forces ``PMIX_GROUP_CANCEL`` to
  unstick remote servers), ``group_daemon_fail`` (whole-daemon loss), and
  ``group_leave``.
