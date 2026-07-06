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
  each caddy retains its ``pmix_peer_t *peer``. "Has this rank
  contributed?" is answered by scanning ``local_cbs`` for a caddy with
  the departing peer's name. Because ``nlocal`` counts *ranks* (one per
  local participant, clones not counted separately), the contribution
  question is correctly asked per rank: a rank is satisfied if any of its
  peers — the client or a fork/exec'd clone — has an entry. The clone
  data-loss hazard the spec warns about is avoided not by keying the
  *check* on the peer pointer but by **never removing a contribution on
  loss at all** (Case A does nothing), so a sibling clone's data can never
  be discarded.
* ``departed`` is the new list (of ``pmix_proclist_t``) of expected ranks
  lost before contributing, deduplicated by name. A rank appears in at
  most one of ``local_cbs`` / ``departed``.
* ``expected`` remains the count ``nlocal`` (established exactly as today,
  including ``PMIX_RANK_WILDCARD`` expansion and the late-registration
  increments). It is **no longer decremented** on loss.

The vestigial ``local_cnt`` field is removed; nothing should compute
completion from a raw counter after this change.

The single completion predicate
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

One function, called from every site that currently hand-rolls the test:

.. code-block:: c

   bool pmix_server_trk_complete(pmix_server_trkr_t *trk)
   {
       if (!trk->def_complete) {
           return false;
       }
       /* every expected participant has either contributed or departed */
       return (pmix_list_get_size(&trk->local_cbs) +
               pmix_list_get_size(&trk->departed)) >= trk->nlocal;
   }

Because a rank is counted in exactly one of the two lists and neither is
ever decremented behind the predicate's back, this cannot complete early:
a not-yet-heard-from live participant is in neither list, so the sum stays
below ``nlocal`` until it either contributes or departs. (``>=`` rather
than ``==`` is deliberate defensive hygiene against any clone
over-count; see *Clones*.)

As built
~~~~~~~~

No new ``pmix_coll_*`` namespace was introduced (see the *[FOLDED]* note
under the migration sequence). The pieces landed where their data already
lives:

* ``pmix_server_trk_complete`` — the predicate above — in
  ``pmix_server_ops.c``, declared in ``pmix_server_ops.h``; called from
  all six fence-family completion sites.
* Locate/create stays in ``pmix_server_get_tracker`` /
  ``pmix_server_new_tracker``; contributions are still appended to
  ``local_cbs`` at each operation's call site.
* The loss routine is the single Case A / Case B block in
  ``lost_connection`` (fence family) plus the pending exported
  ``pmix_server_grp_peer_lost`` (group family).
* Forwarding to the host stays at each operation's completion site
  (``fence_nb`` after ``pmix_server_collect_data``; ``connect`` /
  ``disconnect``; the group host call with context-id / ``grpinfo``
  aggregation). These handoffs were already per-operation and are left
  in place.

Groups: shared semantics, not a shared struct
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

An audit of ``pmix_server_group.c`` (done during implementation) settled
the open question about the ``grp_block_t`` / ``grp_trk_t`` split: it is
**not** folded into ``pmix_server_trkr_t``. Each participant call appends
a fresh ``grp_trk_t`` to the block's ``mbrs`` list, so the group's
contribution count is ``size(mbrs)`` (not the size of a single
contribution list); one block can carry several call signatures (leader /
follower / bootstrap); and the block owns group-only state (context-id,
``grpinfo`` aggregation). Collapsing that onto the fence tracker would be
an invasive rewrite of delicate, untested, HPC-critical code for no
correctness benefit. Instead the group family reuses the *semantics* —
the completion-predicate shape and the Case A / Case B rules — in its own
structure. See *Group loss-accounting design* under the migration
sequence for the concrete plan, and the spec's *What "single" means in
practice* for the rationale.

The two tracker lists therefore remain separate
(``pmix_server_globals.collectives`` and ``.grp_collectives``);
``lost_connection`` traverses both — the fence family inline and the
group family through the exported ``pmix_server_grp_peer_lost``.

Rewriting ``lost_connection`` (fence family — done)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The collective-adjustment block in
``src/mca/ptl/base/ptl_base_sendrecv.c`` now, for each tracker the peer
is an expected participant in:

#. Determine whether the peer's rank has already contributed (scan
   ``local_cbs`` by name — consistent with the per-rank ``nlocal`` count).
#. **Case A** (contributed): do nothing — leave the contribution and its
   data in place, leave ``nlocal`` untouched.
#. **Case B** (not contributed): append the rank to ``departed`` (once).
#. Set ``PMIX_LOCAL_COLLECTIVE_STATUS`` (``PMIX_ERR_PARTIAL_SUCCESS`` if
   any expected participant is still awaited, else
   ``PMIX_ERR_LOST_CONNECTION``).
#. If ``host_called`` is already set, stop — the local phase is frozen
   (spec invariant 5).
#. Otherwise re-evaluate ``pmix_server_trk_complete`` and, if newly
   complete, drive it through the existing completion block exactly as a
   normal final contribution would.

All of this continues to run on the progress thread, as it did before;
no new thread-shifting is introduced.

Migration sequence
------------------

Land in small, independently-buildable, independently-reviewable commits
(the project favors one logical change per commit). Status is recorded
against each step.

#. **[DONE] Introduce the predicate.** Added ``pmix_server_trk_complete``
   and the ``departed`` list to ``pmix_server_trkr_t``; replaced the six
   hand-rolled completion tests with calls to it. Behavior-neutral
   (``departed`` empty), builds clean, ``make check`` green.
#. **[DONE] Fix loss accounting for fence/connect/disconnect.** Rewrote
   the ``collectives`` handling in ``lost_connection`` to Case A / Case B
   via ``departed`` instead of ``--nlocal`` + name-based removal. This is
   the commit that fixes the reported bug. Verified with ``make check``,
   ``simptest``, and the ``simpdie`` death-injection scenarios.
#. **[FOLDED] Extract a ``pmix_coll_*`` service.** After steps 1 and 2
   there was little left to extract for the fence family: locate/create
   already live in ``pmix_server_{get,new}_tracker``, the predicate is
   ``pmix_server_trk_complete`` in ``pmix_server_ops.c``, and the loss
   routine is the single block in ``lost_connection``. A separate
   ``pmix_coll_*`` namespace would have been churn without payoff, so this
   step was folded into steps 1-2 rather than done as a rename. The group
   entry point (step 4) is exported from ``pmix_server_group.c`` for the
   same reason — keep the logic with the data it owns.
#. **[DONE] Remove dead code.** Deleted the vestigial ``local_cnt`` from
   both ``pmix_server_trkr_t`` and ``grp_block_t``.
#. **[DONE] Group loss accounting.** Group construct/destruct now get the
   same Case A / Case B semantics; previously ``lost_connection`` did not
   touch ``grp_collectives`` at all, so a group whose participant died
   before contributing hung forever. Implemented as described below and
   validated with a new death-injection exercise
   (``examples/group_die.c``): a member leaves before contributing and the
   survivors' construct completes rather than hangs. ``make check`` and
   the normal group path (``doubleget``) are unaffected.

Each landed step keeps the tree building warning-free under
``--enable-devel-check`` and green under ``make check``.

Group loss-accounting design (as built)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Implemented in ``pmix_server_group.c`` (keeping the two-level structure —
see the spec's *What "single" means in practice*), reached from
``lost_connection`` through one exported entry point:

#. Added ``pmix_list_t departed`` and a real ``bool host_called`` to
   ``grp_block_t`` (the existing ``host_called`` on ``grp_trk_t`` is
   declared but never set — groups had no host-called gate).
#. Added ``grp_blk_locally_complete(blk)`` == ``def_complete &&
   (size(mbrs) + size(departed)) >= nlocal`` (a pure predicate, matching
   the fence-family shape). Both completion sites gate on it together with
   ``!blk->host_called``; the normal completion site sets ``host_called``
   just before calling the host.
#. Exported ``pmix_server_grp_peer_lost(peer)``, called from
   ``lost_connection`` alongside the fence-family loop. For each block the
   peer is an expected member of, that has ``nlocal > 0`` (skipping
   bootstrap/follower blocks, which pass up per call and never wait) and
   is not yet ``host_called``: if any member's ``local_cbs`` holds a caddy
   with the peer's name it already contributed (Case A — do nothing);
   otherwise record the rank on ``departed`` once (Case B). If that makes
   the block locally complete, forward it to the host then — no further
   participant call will arrive to do it.
#. The loss path repeats the ``aggregate_info`` + ``pmix_host_server
   .group`` completion (using the first member's ``pcs``, since all
   non-bootstrap members share the membership) rather than sharing a
   helper with the normal site: the normal site's error handling is
   entangled with the triggering caddy's ownership, and duplicating a
   dozen lines kept that delicate path untouched. This is the one place
   the group family does not literally share code with itself; the
   *semantics* (predicate shape, Case A / Case B) are identical.

Keeping this logic inside ``pmix_server_group.c`` confines the delicate
group lifecycle (context-id, ``grpinfo`` aggregation, ``grpcbfunc``
reply/removal) to the file that owns it, and gives ``lost_connection`` a
clean, single call — mirroring how the fence family is handled.

A multi-node harness (``contrib/dockerswarm``) drives the group examples,
including ``group_die``, across separate PMIx servers under PRRTE.

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
