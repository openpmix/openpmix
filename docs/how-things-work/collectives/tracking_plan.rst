Implementation Plan: A Single Collective-Tracking Service
=========================================================

.. note::

   This is the implementation plan for the design stated in
   :doc:`tracking_spec`. It is a working document describing *how* the
   server code will be reshaped to meet that specification; it will
   become stale once the work lands and should then be pruned or folded
   into the narrative docs. Read the specification first — this plan
   assumes its vocabulary (``expected`` / ``contributed`` / ``departed``,
   Case A / Case B, the single completion predicate).

Objectives
----------

#. **Fix the correctness bug.** A local participant that contributes and
   then dies must not cause early completion, and its already-delivered
   data must not be discarded (spec invariants 1 and 2).
#. **Consolidate.** Replace the two parallel tracker families with a
   single tracking service used by fence, connect, disconnect, and group
   construct/destruct (spec invariant 7).
#. **Preserve everything externally observable.** No change to any public
   type, API signature, ``pmix_server_module_t`` field order, or on-wire
   message layout. No change to the progress-thread / caddy discipline.

Non-goals
---------

* Changing the global (inter-node) collective phase — that remains the
  host environment's responsibility.
* Changing how modex data is packed or how context IDs are assigned;
  those become per-operation hooks, unchanged in behavior.
* Reworking clone semantics beyond what invariant correctness requires
  (see *Clones* below).

Current state (starting point)
------------------------------

The code paths this plan touches:

* **Fence/connect/disconnect tracker** — ``pmix_server_trkr_t`` in
  ``src/include/pmix_globals.h``.
* **Group tracker (two-level)** — ``grp_block_t`` / ``grp_trk_t`` in
  ``src/server/pmix_server_group.c``.
* **Locate/create tracker** — ``pmix_server_get_tracker`` /
  ``pmix_server_new_tracker`` in ``src/server/pmix_server_fence.c``.
* **Add contribution and completion test (fence)** —
  ``src/server/pmix_server_fence.c`` (append to ``local_cbs``, then the
  ``==`` test).
* **Completion test (connect/disconnect)** —
  ``src/server/pmix_server_connect.c``.
* **Completion test (group)** — ``src/server/pmix_server_group.c``.
* **Late-registration re-checks** — ``src/server/pmix_server.c``
  (``register_nspace`` and ``register_client`` paths).
* **Lost-connection accounting** — ``lost_connection`` in
  ``src/mca/ptl/base/ptl_base_sendrecv.c``.
* **Global tracker lists** — ``pmix_server_globals.collectives`` and
  ``pmix_server_globals.grp_collectives``.

The completion test ``def_complete && pmix_list_get_size(&local_cbs) ==
nlocal`` is copied at each site above. ``lost_connection`` walks only
``collectives`` and, per tracker, unconditionally does ``--nlocal`` and
removes the departed peer's contribution by name.

Design
------

The service (proposed name ``pmix_coll`` — a set of ``pmix_coll_*``
functions in a new ``src/server/pmix_server_coll.c`` / ``.h`` pair, or an
extension of the existing ``pmix_server_ops`` surface) exposes one
tracker type and a small API.

Unified tracker
~~~~~~~~~~~~~~~

Extend ``pmix_server_trkr_t`` to be the single tracker for all four
operations and add identity accounting. The additive fields:

.. code-block:: c

   /* identity accounting (in addition to the existing
    * nlocal / def_complete / local_cbs / host_called fields) */
   pmix_list_t departed;   /* peers lost BEFORE contributing:
                            *   list of pmix_proclist_t (or a caddy that
                            *   retains the pmix_peer_t) */

* ``contributed`` is represented by the existing ``local_cbs`` list — it
  already stores one ``pmix_server_caddy_t`` per contributing peer, and
  each caddy retains its ``pmix_peer_t *peer``. "Has *this peer*
  contributed?" is answered by scanning ``local_cbs`` for a caddy whose
  ``peer`` pointer matches — **by peer, not by name**, which is what
  makes clones distinguishable.
* ``departed`` is the new list of expected peers lost before
  contributing. A peer appears in at most one of ``local_cbs`` /
  ``departed``.
* ``expected`` remains the count ``nlocal`` (established exactly as today,
  including ``PMIX_RANK_WILDCARD`` expansion and the late-registration
  increments). It is **no longer decremented** on loss.

The vestigial ``local_cnt`` field is removed; nothing should compute
completion from a raw counter after this change.

The single completion predicate
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One function, called from every site that currently hand-rolls the test:

.. code-block:: c

   bool pmix_coll_locally_complete(pmix_server_trkr_t *trk)
   {
       if (!trk->def_complete) {
           return false;
       }
       /* every expected participant has either contributed or departed */
       return (pmix_list_get_size(&trk->local_cbs) +
               pmix_list_get_size(&trk->departed)) >= trk->nlocal;
   }

Because a peer is counted in exactly one of the two lists and neither is
ever decremented behind the predicate's back, this cannot complete early:
a not-yet-heard-from live participant is in neither list, so the sum stays
below ``nlocal`` until it either contributes or departs. (``>=`` rather
than ``==`` is deliberate defensive hygiene against any clone
over-count; see *Clones*.)

Service API
~~~~~~~~~~~

.. code-block:: c

   /* locate or create the tracker for this operation */
   pmix_server_trkr_t *pmix_coll_get_tracker(char *id, pmix_proc_t *procs,
                                             size_t nprocs, pmix_cmd_t type);

   /* record a contribution from a connected peer (appends its caddy) */
   void pmix_coll_add_contribution(pmix_server_trkr_t *trk,
                                   pmix_server_caddy_t *cd);

   /* account for a lost peer across ALL trackers it participates in;
      applies Case A / Case B and drives completion where warranted */
   void pmix_coll_peer_lost(pmix_peer_t *peer);

   /* the shared completion predicate */
   bool pmix_coll_locally_complete(pmix_server_trkr_t *trk);

   /* forward a locally-complete tracker to the host via the right hook */
   pmix_status_t pmix_coll_forward(pmix_server_trkr_t *trk);

Per-operation hooks
~~~~~~~~~~~~~~~~~~~

The only operation-specific behavior is *what to do at handoff*. Model it
as a tiny dispatch keyed on ``trk->type`` inside ``pmix_coll_forward``
(fence → ``fence_nb`` after ``pmix_server_collect_data``; connect →
``connect``; disconnect → ``disconnect``; group → the group host call
with context-id / grpinfo aggregation). No operation retains its own
completion or loss logic.

Folding in groups
~~~~~~~~~~~~~~~~~

The group ``grp_block_t`` / ``grp_trk_t`` split maps onto the unified
tracker as follows:

* The block's participation bookkeeping (``nlocal``, ``def_complete``,
  and the counting that drives completion) is exactly what the unified
  tracker already holds — delete the duplicates.
* Group-specific state that has no analogue in the other operations
  (``need_cxtid`` / context-id assignment, ``grpinfo`` aggregation, and
  the ``block → mbrs`` fan-out where one construct spans multiple
  sub-collectives) is retained as a thin group-only side structure that
  *references* the unified tracker rather than re-implementing counting.

**Open decision:** whether one ``grp_block_t`` maps to exactly one
unified tracker or to several. If a block can legitimately carry more
than one concurrent sub-collective, the group wrapper keeps its ``mbrs``
list but each member is (or points at) a unified tracker. Resolve this by
auditing how ``mbrs`` is populated in ``pmix_server_group.c`` before
writing code.

Unify the lists
~~~~~~~~~~~~~~~

Move group trackers onto ``pmix_server_globals.collectives`` (or, if the
group wrapper is retained, ensure ``pmix_coll_peer_lost`` traverses both
lists). One list is preferred: it is the change that structurally
guarantees the lost-connection handler can never again miss an operation
class.

Rewriting ``lost_connection``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Replace the collective-adjustment block in
``src/mca/ptl/base/ptl_base_sendrecv.c`` with a single call to
``pmix_coll_peer_lost(peer)``. That routine, for each tracker the peer is
an expected participant in:

#. Determine contribution by **peer pointer** (scan ``local_cbs``).
#. **Case A** (found): do nothing to the accounting — leave the
   contribution and its data in place, leave ``nlocal`` untouched.
#. **Case B** (not found): append the peer to ``departed`` (once).
#. Set ``PMIX_LOCAL_COLLECTIVE_STATUS`` (``PMIX_ERR_PARTIAL_SUCCESS`` if
   live participants remain, else ``PMIX_ERR_LOST_CONNECTION``).
#. If ``host_called`` is already set, stop — the local phase is frozen
   (spec invariant 5).
#. Otherwise re-evaluate ``pmix_coll_locally_complete`` and, if newly
   complete, drive it through ``pmix_coll_forward`` exactly as a normal
   final contribution would.

All of this continues to run on the progress thread, as it does today;
no new thread-shifting is introduced.

Migration sequence
------------------

Land in small, independently-buildable, independently-reviewable commits
(the project favors one logical change per commit):

#. **Introduce the predicate.** Add ``pmix_coll_locally_complete`` and the
   ``departed`` list to ``pmix_server_trkr_t``; replace the six-plus
   hand-rolled completion tests with calls to it. No behavior change yet
   (``departed`` is always empty), so this is a pure refactor that must
   build clean and pass ``make check``.
#. **Fix loss accounting for fence/connect/disconnect.** Rewrite the
   ``collectives`` handling in ``lost_connection`` to Case A / Case B via
   ``departed`` instead of ``--nlocal`` + name-based removal. This is the
   commit that fixes the reported bug.
#. **Extract the service.** Move locate/create, add-contribution,
   forward, and loss accounting into the ``pmix_coll_*`` API; point
   fence/connect/disconnect at it. Still no group changes.
#. **Fold in groups.** Port group construct/destruct onto the unified
   tracker and service; resolve the ``block``/``mbrs`` open decision;
   unify the tracker lists (or extend the loss traversal). Group
   collectives now get loss accounting for the first time.
#. **Remove dead code.** Delete ``local_cnt`` and the now-unused
   ``grp_block_t`` / ``grp_trk_t`` counting fields.

Each step keeps the tree building warning-free under
``--enable-devel-check`` and green under ``make check``.

Testing
-------

Add unit tests under ``test/unit`` wired into ``make check`` so the
coverage sticks in CI. Target scenarios, per operation (fence, connect,
disconnect, group):

* **Baseline:** all local participants contribute → completes once, with
  all data, forwarded to host exactly once.
* **The reported bug (Case A):** participant contributes, then its
  connection drops before the others call → collective still waits for
  the remaining participants, completes with the departed participant's
  data included, and does **not** complete early.
* **Case B:** participant drops before contributing → collective
  completes once the remaining live participants contribute; status is
  ``PMIX_ERR_PARTIAL_SUCCESS`` (or ``PMIX_ERR_LOST_CONNECTION`` if none
  remain).
* **Loss after ``host_called``:** dropping a participant after handoff
  neither re-forwards nor alters completion.
* **Clones:** two peers sharing one ``{nspace, rank}`` each contribute;
  losing one after it contributed does not discard the other's
  contribution.
* **Late registration vs. concurrent loss:** a rank registering late
  while another participant is being lost does not complete the
  collective early.

Then the manual smoke path from the top-level guidance:
``make check`` in ``test``, and ``./simptest`` in ``test/simple``.

Risks and constraints
---------------------

* **ABI / wire format:** untouched. All affected structs are internal to
  ``libpmix``; the fix is behavioral. No ``bfrops`` component changes, no
  new status/event codes (``PMIX_ERR_PARTIAL_SUCCESS`` and
  ``PMIX_ERR_LOST_CONNECTION`` already exist).
* **Thread safety:** all accounting stays on the progress thread; the
  caddy discipline is unchanged. ``departed`` entries must retain the
  ``pmix_peer_t`` only if the peer object can outlive the tracker — audit
  peer refcounting when the peer is being torn down in ``lost_connection``
  (store the identity, or retain/release, as the lifetime analysis
  dictates).
* **Interoperability:** unchanged — this is server-local logic; a peer of
  any version sees identical behavior on the wire.
* **Clones** (see design): the peer-keyed contribution check is the
  correctness-critical part and is handled. Whether ``nlocal`` should
  count clones distinctly is a pre-existing question this plan does not
  regress; the ``>=`` predicate tolerates any clone over-count, and the
  audit in step 4 records whatever semantics ``pmix_server_new_tracker``
  already implements.

Open decisions to resolve before coding
----------------------------------------

#. ``grp_block_t`` → one unified tracker, or a retained thin wrapper over
   many? (Audit ``mbrs`` population.)
#. One global tracker list, or keep two and traverse both on loss?
   (Prefer one.)
#. ``departed`` element type and peer lifetime: store ``pmix_proc_t`` +
   peer pointer in a small caddy, vs. retain the ``pmix_peer_t``.
#. Where the service lives: a new ``pmix_server_coll.[ch]`` vs. growing
   ``pmix_server_ops``.
