Client Init/Finalize Lifecycle and Server-Side State
====================================================

This document specifies how the PMIx server manages the per-client and
per-namespace state associated with a client across the ``PMIx_Init`` /
``PMIx_Finalize`` boundary. Its purpose is to define the intended
behavior precisely enough to implement and test it, and in particular to
guarantee one property:

.. admonition:: The cycling guarantee
   :class: note

   A client may cycle ``PMIx_Init`` → *(work: fences, gets, puts,
   events, …)* → ``PMIx_Finalize`` and then ``PMIx_Init`` again, any
   number of times, reusing the **same namespace and rank**, and each
   new ``PMIx_Init`` must present a **fresh interaction state** on the
   server. No state produced in one cycle may leak into the next. The
   host environment is **not** required to deregister and re-register the
   client between cycles for this to hold.

For a code-oriented view of the socket connection that underlies each
init, see :doc:`ptl`. This document is concerned with the *state
lifecycle*, not the transport.


The Problem
-----------

When a client calls ``PMIx_Init``, the server accumulates state on its
behalf: a peer object holding the connection, event-loop registrations,
tag counters, and a datastore (``gds``) module assignment; entries in
collective trackers as the client fences; event and I/O-forwarding
registrations; cached notifications; and committed key/value data. The
server also carries longer-lived, job-scoped state in the namespace
object: the roster of ranks, the count of local processes, the GDS job
data, and network/programming-model resources reserved for the job.

A single ``PMIx_Init``/``PMIx_Finalize`` pass, followed by the host
tearing the job down, is the common case and works. The difficulty is
the *cycling* case. ``PMIx_Finalize`` on the client side tears the client
library all the way down (including the class system), so a subsequent
``PMIx_Init`` is a brand-new library instance that opens a **new** socket
and reconnects. On the server, the namespace and the rank are still
registered — the host did not tear them down — so the second init must
reconnect onto existing job-scoped state while starting from a clean
per-client slate. If per-client state from the first cycle survives into
the second, the client sees stale data, duplicated accounting, or leaked
objects.

The design below draws a sharp line between **job-scoped** state, which
belongs to the namespace and persists until the host deregisters it, and
**client-scoped** state, which belongs to a single init/finalize cycle
and must not survive one.


The Actors
----------

``pmix_namespace_t`` (``nptr``)
    The job. Holds the ``ranks`` list, ``nlocalprocs`` (the number of
    local clients registered for this namespace), ``nfinalized`` (how
    many of those have finalized), the compatibility modules negotiated
    for the job, the namespace-level epilog, and the association to the
    GDS job data. Persists until the host calls
    ``PMIx_server_deregister_nspace``.

``pmix_rank_info_t`` (``info``)
    One entry per rank on ``nptr->ranks``. Records the rank's identity
    (``pname``), ``uid``/``gid``, the host's ``server_object``, the index
    (``peerid``) of the peer object serving this rank in the server's
    ``clients`` array, and ``proc_cnt`` — the number of live *clones* of
    this rank (see below). This is **registration** state: it is created
    by ``PMIx_server_register_client`` (or lazily on first connect) and
    lives until the client or the namespace is deregistered. It is
    **not** torn down by finalize.

``pmix_peer_t`` (``peer``)
    The per-connection object: socket, send/receive queues, event
    registrations, dynamic-tag counters, the ``finalized`` flag, the
    peer's assigned ``gds`` module, and the peer-level epilog. Held in
    ``pmix_server_globals.clients`` and pointed to by ``info->peerid``.
    This is the primary carrier of **client-scoped** state.

Clones
    A client may ``fork``/``exec`` a child that also calls ``PMIx_Init``
    using the *same* namespace and rank as its parent — in PMIx
    terminology, a *clone* of that client. Each clone opens its own
    socket and is tracked by ``info->proc_cnt``. A rank is only fully
    finalized when the original
    client **and all of its clones** have finalized — i.e. when
    ``proc_cnt`` returns to zero.


The Three Teardown Triggers
---------------------------

Server-side cleanup is driven by exactly three events. Each has a
distinct, non-overlapping responsibility. The remainder of this document
is normative: "must" denotes a required behavior.

1. Client finalize (all clones included)
    A client, together with all of its clones, has finalized — the rank's
    ``proc_cnt`` has dropped to zero. The peer object serving that rank
    is **recycled in place**: the server performs an object
    destruct/construct on it and repoints it at the namespace it belongs
    to. The object is **not** released and is **not** removed from the
    namespace's client list. See `Recycling the peer on finalize`_.

2. Host deregisters a client (``PMIx_server_deregister_client``)
    The server removes the peer object from the namespace and releases
    it. See `Deregistering a client`_.

3. Host deregisters the namespace (``PMIx_server_deregister_nspace``)
    The server removes all GDS-registered job data for the namespace,
    removes and releases all peer objects belonging to the namespace, and
    cleans up all remaining references to the namespace. See
    `Deregistering the namespace`_.

The essential distinction is that **finalize is reversible and
deregistration is terminal**. Finalize leaves the registration intact so
the client can init again; only the host, by deregistering, declares that
the client or the whole job is gone for good and its objects may be
freed.


Recycling the Peer on Finalize
------------------------------

When the last live process for a rank finalizes (``proc_cnt`` reaches
zero), the server **must** reset the peer object rather than release it:

* Run the peer's destructor (``PMIX_DESTRUCT``) so that every allocation
  and reference the object accumulated during the cycle is torn down:
  pending send/receive messages and queues, event registrations, the
  peer-level epilog, the reference to its namespace, its ``gds``
  assignment, dynamic-tag state, and the ``finalized`` flag.

* Immediately reconstruct it (``PMIX_CONSTRUCT``) so it returns to the
  pristine state a freshly allocated peer would have — in particular
  ``finalized`` is once again ``false`` and all lists and counters are
  empty.

* Repoint the reconstructed object at the namespace it serves
  (re-establish ``peer->nptr`` with a retained reference), so the peer
  remains associated with its job.

* Leave the object in ``pmix_server_globals.clients`` at its existing
  slot, and leave ``info->peerid`` pointing at it. The **registration**
  (the ``pmix_rank_info_t`` on ``nptr->ranks`` and the client-array slot)
  is deliberately preserved.

The result is a registered rank whose peer object is a clean, empty
container ready to be repopulated. When the client calls ``PMIx_Init``
again and reconnects, the server reuses this existing, reset peer for the
rank instead of allocating a new one — so no peer object is orphaned and
the second cycle starts from a guaranteed-fresh per-client state.

Why destruct/construct rather than release? Releasing the peer would
either strand ``info->peerid`` (a dangling index) or force the host to
re-register the client before it could init again — defeating the cycling
guarantee. Recycling in place keeps the registration valid and the
client-array slot stable while still discarding every byte of
client-scoped state from the previous cycle.

Job-scoped accounting must be reconciled at the same time so that it does
not drift across cycles. In particular, the namespace's finalized
accounting (``nfinalized`` relative to ``nlocalprocs``) and the rank's
``proc_cnt`` must reflect the true, current number of connected processes
after a recycle, so that a client which finalizes and re-inits is not
double-counted and the "all local processes finalized" condition remains
accurate for the job as a whole.

What finalize does **not** do
    Finalize does not remove the rank from ``nptr->ranks``, does not
    release the peer, does not drop the namespace's GDS job data, and
    does not touch network or programming-model resources reserved for
    the job. Those are job-scoped and survive until the host deregisters.
    Transient, client-scoped registrations that are keyed to the peer —
    event registrations, I/O-forwarding requests, in-flight direct-modex
    requests, and cached notifications targeting the client — are pruned
    as part of the teardown so they do not linger on the reset object.


Deregistering a Client
----------------------

``PMIx_server_deregister_client`` is the host's declaration that a
specific rank is gone and will not init again. Here the peer is **removed
and released**, not recycled:

* Remove the peer object from the namespace: release the reference the
  namespace/registration holds, null out the client-array slot
  identified by ``info->peerid``, and remove the ``pmix_rank_info_t``
  from ``nptr->ranks``.

* Release the peer object.

* Restore any job resources that were charged to the rank (network
  allocations, monitoring/sensors) and honor the peer's epilog, exactly
  as when a client terminates.

After a client is deregistered, the rank no longer exists on the server;
a subsequent connection for that rank would be rejected unless the host
re-registers it.


Deregistering the Namespace
---------------------------

``PMIx_server_deregister_nspace`` is the host's declaration that the
entire job is gone. This is the terminal, whole-namespace teardown and
**must** reclaim everything the namespace owned:

* Remove all GDS-registered job data for the namespace (``gds`` del of
  the namespace), so no key/value or job-info data survives.

* Remove and release **all** peer objects belonging to the namespace —
  both those still recycled/idle from a finalize and any that never
  finalized — nulling their client-array slots and freeing every
  ``pmix_rank_info_t`` on ``nptr->ranks``.

* Release the network and programming-model resources reserved for the
  job, run the namespace epilog, and prune every remaining reference to
  the namespace: event and I/O-forwarding registrations, cached
  notifications targeting any of its procs, and finally the namespace
  object itself.

Nothing that names the namespace may remain after this call returns.


State Ownership Summary
-----------------------

The following table classifies the state and states which trigger frees
it. "Recycled" means reset in place by finalize (not freed); "Released"
means freed.

.. list-table::
   :header-rows: 1
   :widths: 34 22 22 22

   * - State
     - Client finalize (all clones)
     - Deregister client
     - Deregister nspace
   * - Peer object (``pmix_peer_t``)
     - Recycled (destruct/construct, repointed, kept)
     - Released, slot nulled
     - Released, slot nulled
   * - Event / IOF / direct-modex regs, cached notifications for the peer
     - Pruned
     - Pruned
     - Pruned
   * - Peer epilog
     - Executed
     - Executed
     - Executed (peer) + namespace epilog
   * - Rank info (``pmix_rank_info_t`` on ``nptr->ranks``)
     - Kept (registration preserved)
     - Removed, released
     - Removed, released
   * - GDS job data for the namespace
     - Kept
     - Kept
     - Removed
   * - Network / programming-model job resources
     - Kept
     - Rank's share restored
     - Released (whole job)
   * - Namespace object (``pmix_namespace_t``)
     - Kept
     - Kept
     - Released; all references purged


Invariants
----------

An implementation of the above must uphold these invariants:

* **No cross-cycle carryover.** After a recycle, the peer object must be
  indistinguishable from a freshly allocated one except that it is
  already associated with its namespace. ``finalized`` is ``false``; all
  queues, event registrations, tag counters, and datastore assignments
  are empty/default.

* **No orphaned peers.** Cycling init/finalize must not leave unreferenced
  peer objects in the ``clients`` array. Recycling in place, rather than
  allocating a new peer on reconnect, is what guarantees this.

* **No counter drift.** Per-namespace finalized accounting and per-rank
  ``proc_cnt`` must return to values consistent with the actual number of
  connected processes after each cycle, so that the job-level "all local
  processes finalized" condition and any resource-release keyed on it
  remain correct no matter how many times clients cycle.

* **Registration outlives finalize; only deregistration is terminal.**
  The ``pmix_rank_info_t`` and the client-array slot persist across
  finalize and are removed only by an explicit host deregister. GDS job
  data and job resources persist until ``PMIx_server_deregister_nspace``.
