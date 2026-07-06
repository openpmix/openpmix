Specification: Tracking Local Collective Participation
======================================================

.. note::

   This is a *design specification*. It states how the PMIx server is
   required to track local participation in collective operations, and
   in particular how it must account for participants whose connections
   are lost. It documents the intended behavior and the invariants that
   any implementation must preserve. The companion implementation plan
   lives alongside this document in the same directory.

Scope
-----

This specification covers the server-side tracking of *local*
participation in the collective operations that PMIx assembles on a node
before forwarding to the host environment:

* ``PMIx_Fence`` / ``PMIx_Fence_nb`` (``PMIX_FENCENB_CMD``)
* ``PMIx_Connect`` / ``PMIx_Connect_nb`` (``PMIX_CONNECTNB_CMD``)
* ``PMIx_Disconnect`` / ``PMIx_Disconnect_nb`` (``PMIX_DISCONNECTNB_CMD``)
* ``PMIx_Group_construct`` / ``_destruct`` (``PMIX_GROUP_CONSTRUCT_CMD``,
  ``PMIX_GROUP_DESTRUCT_CMD``)

It does **not** cover the global (inter-node) phase, which is the
responsibility of the host environment, nor the wire encoding of the
collected data, which belongs to ``bfrops``.

Background: how a collective is assembled
-----------------------------------------

A collective names a set of participating processes. Some of those
processes are *local* — clients (and their clones) connected to this
server — and some may be remote. The server's job in the local phase is
to wait until **every local participant has contributed** its call, then
pass the assembled operation up to the host for global completion.

The server represents each in-flight collective with a *tracker*. For
fence/connect/disconnect this is ``pmix_server_trkr_t`` (defined in
``src/include/pmix_globals.h``); group operations use the parallel
``grp_block_t`` / ``grp_trk_t`` structures in
``src/server/pmix_server_group.c``. Trackers live on the server-global
lists ``pmix_server_globals.collectives`` and
``pmix_server_globals.grp_collectives`` respectively.

A tracker records, among other things:

* the participant list (``pcs`` / ``npcs``) as originally supplied;
* the *expected* number of local participants (``nlocal``);
* a list of the contributions received so far (``local_cbs``, a list of
  ``pmix_server_caddy_t``, one entry per contributing peer — including a
  separate entry for each fork/exec'd clone of a participating rank);
* whether the tracker *definition* is complete (``def_complete``: every
  participating namespace has been registered so ``nlocal`` is known to
  be final);
* whether the operation has already been handed to the host
  (``host_called``).

When a local client calls the collective, the server locates or creates
the matching tracker and appends the caller's caddy to ``local_cbs``.

The counter-based completion test and its vulnerability
-------------------------------------------------------

Historically, local completion has been decided by comparing a count of
contributions against the expected local count. In the current code the
test appears (in several places) as:

.. code-block:: c

   if (trk->def_complete &&
       pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
       /* local phase complete -- forward to host */
   }

That is: *the number of contributions received equals the number of
local participants expected.* This is a **cardinality** test — it counts
contributions and expected participants, but it does not record *which*
specific participants have contributed.

Separately, when a client's connection is lost, the server must account
for its departure so that a collective it was expected to join does not
hang forever. The lost-connection handler
(``lost_connection`` in ``src/mca/ptl/base/ptl_base_sendrecv.c``)
currently, for each tracker the departing peer belongs to:

#. decrements the expected count (``--trk->nlocal``); and
#. removes any contribution the peer had already made from
   ``local_cbs`` (matched by process name).

Because participation is tracked only as a count, these two adjustments
race against the ordinary participation path. The failure has two
distinct symptoms.

**1. Early completion.** A cardinality test cannot tell the difference
between "this participant has not called yet" and "this participant
called and then died." If the expected count is reduced for a
participant whose contribution was already counted, the remaining live
participants can drive the count to the (now lowered) threshold before
they have all actually called in. The collective completes early and is
forwarded to the host with a subset of the intended local contributions.

   *Illustration.* A fence expects three local participants: ranks 0, 1,
   and 2. Rank 0 calls the fence; its contribution is counted. Rank 0
   then dies before ranks 1 and 2 call. In a pure counter model, rank
   0's contribution remains counted while the expected total is
   decremented to two; when ranks 1 and 2 call, the count reaches the
   lowered threshold of two and the fence completes — even though it was
   supposed to also include the (still-live-at-call-time) semantics of
   all three, and rank 2 may not yet have called at all.

**2. Loss of already-delivered data.** For a data-collecting fence, a
participant's contribution *is* its modex data. Discarding a departed
participant's contribution after it was delivered removes that data from
the assembled collective, so the surviving participants receive an
incomplete result — a silent correctness error, not merely a timing one.

The root cause in both cases is the same: **participation is tracked as a
count rather than by participant identity.** A count cannot answer the
one question the lost-connection handler must ask — *"has this specific
participant already contributed?"*

Required model: track participation by identity
-----------------------------------------------

The server must track local collective participation **by participant
identity**, not by a bare counter. Conceptually, each tracker maintains
three sets of local participants:

``expected``
    The local participants the collective is waiting on. Established as
    namespaces register and as the participant list is resolved against
    the set of local processes (including ``PMIX_RANK_WILDCARD``
    expansion). A participant enters ``expected`` when it is known to be
    a local member of the operation.

``contributed``
    The local participants that have delivered their call for this
    collective, together with whatever data or info that call carried.
    A participant enters ``contributed`` exactly once per required
    contribution.

``departed``
    Local participants that were lost (connection dropped, abnormal
    termination) **before** contributing. A participant enters
    ``departed`` only if it was expected and had not already
    contributed.

Completion rule
~~~~~~~~~~~~~~~

The local phase is complete when the tracker definition is complete
(``def_complete``) **and every expected participant has been accounted
for** — that is, each expected participant is either in ``contributed``
or in ``departed``:

.. code-block:: text

   local_complete  ==  def_complete
                       AND  (contributed ∪ departed)  ⊇  expected

Equivalently, the collective is still pending as long as at least one
expected participant has neither contributed nor departed.

This must be the **single** completion predicate used everywhere the
question "is this collective locally complete?" is asked. Today that
question is asked, with a hand-rolled cardinality test, in at least six
places (the fence, connect, and disconnect contribution paths; the two
late-registration re-checks in ``pmix_server.c``; and the lost-connection
handler). All of them must consult one shared predicate so the accounting
can never diverge between call sites.

Lost-connection accounting rules
---------------------------------

When a connection is lost, for every tracker (on **both** the
``collectives`` and ``grp_collectives`` lists) in which the departing
peer is an expected participant, the server must apply exactly one of the
following, based on whether that specific participant has already
contributed:

**Case A — the participant had already contributed.**
   *Ignore the loss for purposes of this collective.* The contribution
   stands: it remains in ``contributed`` and its delivered data remains
   part of the assembled collective. The expected count is **not**
   reduced and the contribution is **not** removed. The participant has
   already discharged its obligation to this collective; its subsequent
   death is irrelevant to local completion.

**Case B — the participant had not yet contributed.**
   The participant can never contribute now, so move it from
   ``expected`` accounting into ``departed`` (in the counter view: it is
   removed from the set of contributions still being waited on). This
   allows the collective to complete once the remaining live participants
   contribute, rather than hanging forever on a participant that will
   never call.

In both cases the completion predicate is then re-evaluated. If the
collective has become locally complete as a result, it is processed
exactly as it would be on a normal final contribution (see *Interaction
with the host handoff* below).

Note the asymmetry with the current implementation, which in every case
both decrements the expected count *and* removes any prior contribution.
Case A above forbids both of those adjustments; that is the specific
change that fixes the vulnerability.

Status reporting on loss
~~~~~~~~~~~~~~~~~~~~~~~~~

When a loss affects a collective, the server continues to report the
outcome to the surviving participants via the
``PMIX_LOCAL_COLLECTIVE_STATUS`` info field on the tracker:

* If at least one expected participant still remains after the loss is
  accounted for, the status is ``PMIX_ERR_PARTIAL_SUCCESS``.
* If the loss leaves no remaining live participants, the status is
  ``PMIX_ERR_LOST_CONNECTION``.

A loss falling under Case A (the participant already contributed) does
not by itself change the set of remaining participants and therefore does
not, on its own, downgrade the status.

Required architecture: a single tracking service
-------------------------------------------------

The behaviors above must be provided by **one collective-tracking service
with a single code path**, shared by all four operations, rather than by
per-operation copies of the accounting logic.

Today the logic is duplicated. Fence, connect, and disconnect use
``pmix_server_trkr_t`` on ``pmix_server_globals.collectives``; group
operations use the parallel ``grp_block_t`` / ``grp_trk_t`` pair on
``pmix_server_globals.grp_collectives``. The two carry essentially the
same state — participant list, expected count, definition-complete flag,
contribution list, host-handoff flag, lock, event — split across slightly
different layouts, and the completion test is hand-rolled at each of the
six-plus call sites that ask "is this collective locally complete?" This
duplication is precisely what let the group path drift out of sync: the
lost-connection handler was never taught about ``grp_collectives``, so
group collectives receive no loss accounting at all.

Consolidating removes that class of defect by construction. The service
must own:

* tracker lifecycle (locate-or-create, release);
* the identity accounting — the ``expected`` / ``contributed`` /
  ``departed`` sets;
* the single completion predicate defined above, evaluated in exactly one
  function;
* lost-connection accounting (Case A / Case B), applied uniformly to
  every tracker on a single tracker list;
* the host-handoff gate (``host_called``); and
* delivery of results to the surviving local participants.

Behavior that genuinely differs between operations must be confined to a
small set of per-operation hooks the service invokes — which host
callback to forward to, and how to assemble the operation's payload
(modex-data collection for fence; context-id assignment and group-info
aggregation for group construct). Everything else is shared. Sharing the
logic in exactly one place is the only tractable way to keep the
invariants below true: a predicate or a rule that exists once cannot
drift out of agreement with a copy of itself.

**What "single" means in practice.** The fence, connect, and disconnect
operations do share one tracker type (``pmix_server_trkr_t``), and for
them the requirement is literal: one struct, one predicate, one loss
routine. The group operations are the exception. Their tracker is a
two-level structure — a ``grp_block_t`` owning a list of ``grp_trk_t``
members, one per call signature — because a single group construct can
be assembled from several distinct signatures (leader, followers,
bootstrap), the contribution count is the number of member trackers
rather than the size of a single contribution list, and the block carries
group-only state (context-id assignment, group-info aggregation). Forcing
that onto ``pmix_server_trkr_t`` would be an invasive rewrite of delicate,
HPC-critical code that currently has no automated test coverage. The
consolidation requirement for the group family is therefore satisfied by
sharing the *semantics* — the same completion-predicate shape
(contributed + departed ≥ expected) and the same Case A / Case B
lost-connection rules — expressed in the group's own structure and
reached through a single exported entry point, rather than by collapsing
the two structs into one. Invariant 7 below is stated in these terms.

Additional required behaviors
-----------------------------

**Clones share a rank but must not corrupt the accounting.** A process
may fork/exec one or more clones that connect to the same server with the
same ``{namespace, rank}`` and independently join a collective. The
overriding requirement is that losing one peer must never discard a
clone's contribution that shares the same name. The current expected
count (``nlocal``) is maintained per *rank* — a clone is not counted as a
separate expected participant — so the contribution question is asked per
rank too: a rank is satisfied once any of its peers has contributed. The
clone data-loss hazard is closed not by keying the contribution *check*
on the peer pointer but by the stronger guarantee that **a loss never
removes a contribution at all** (Case A does nothing): whatever a rank's
peers delivered stays in the collective regardless of which peer's
connection drops. (A future change that made ``nlocal`` count clones
individually would move the contribution check to the peer granularity;
until then, per-rank name matching is the consistent choice.)

**Contribution is idempotent and terminal.** A given required
contribution is recorded once. Once recorded, it is never removed by
loss accounting (Case A). A duplicate call for the same required
contribution must not double-count toward completion.

**Late registration must not race the accounting.** Namespaces and
clients may register after a collective has already begun (the host may
spawn a client that calls in before the local ``register_client`` event
fires). The paths that raise ``nlocal`` / grow ``expected`` on late
registration and then re-test completion must use the same identity-based
predicate, so that a late registration and a concurrent loss cannot
combine to complete a collective early.

**Interaction with the host handoff.** Once a tracker has been forwarded
to the host (``host_called`` is set), local completion has already
occurred and the local accounting is frozen: a subsequent loss must not
attempt to re-complete or re-forward the collective. The server waits for
the host to return, then delivers the result to whichever local
participants are still connected. Losing a participant after handoff only
affects whether a reply can be delivered to that participant — never the
completion or contents of the collective.

**Group operations have the same requirement.** The group
construct/destruct trackers (``grp_block_t`` / ``grp_trk_t`` on
``grp_collectives``) use the same counter-based accounting and are
subject to the same vulnerability. They must adopt the identity-based
model and must be traversed by the lost-connection handler, which today
walks only the ``collectives`` list.

Invariants
----------

An implementation of this specification must preserve the following at
all times:

#. **A contribution, once recorded, is only ever removed by delivering
   the collective's result to that participant — never by loss
   accounting.**
#. **The expected participation set is reduced for a lost participant
   only if that participant had not yet contributed.**
#. **Local completion is decided by one shared predicate**, evaluated
   identically at every call site, expressed in terms of the
   ``expected``, ``contributed``, and ``departed`` sets.
#. **A collective completes locally if and only if its definition is
   complete and every expected participant has either contributed or
   departed-before-contributing.**
#. **After ``host_called`` is set, local loss accounting never re-drives
   completion** for that tracker.
#. **The rules apply uniformly** to fence, connect, disconnect, and group
   operations, and to both directly-connected clients and their clones.
#. **Participation logic is shared, not copied.** Fence, connect, and
   disconnect are tracked by one tracker representation with one
   completion predicate and one lost-connection routine. Group operations,
   whose two-level tracker differs materially, reuse the same completion-
   predicate shape and the same Case A / Case B loss semantics through a
   single exported entry point rather than a private copy. No operation
   carries its own divergent copy of the participation rules.

Relationship to the current implementation
-------------------------------------------

The behaviors above are partially and fragilely approximated today: the
fence/connect/disconnect path attempts to remove a departed peer's
contribution from ``local_cbs`` while also decrementing ``nlocal``, but
it does so by process name (so it cannot distinguish clones), it discards
already-delivered data (violating invariant 1), it duplicates the
completion test across many call sites, and it does not cover group
operations at all. The implementation plan describes how to collapse the
two tracker families into the single identity-based tracking service
required here — one tracker representation, one completion predicate, one
lost-connection routine — while preserving the established tracker
lifecycle, the thread-safety (progress-thread / caddy) rules, and full
ABI and wire-format compatibility (the tracker structures are internal to
``libpmix``; no public type, API signature, or on-wire message layout
changes).
