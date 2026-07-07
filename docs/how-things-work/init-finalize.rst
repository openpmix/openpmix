Client Init/Finalize Lifecycle and State Management
===================================================

This document specifies how PMIx manages the state associated with a
client across the ``PMIx_Init`` / ``PMIx_Finalize`` boundary. It has two
halves: the **server-side** management of the per-client and
per-namespace state the server holds on a client's behalf, and the
**client-side** teardown of the client library itself. Its purpose is to
define the intended behavior precisely enough to implement and test it,
and in particular to guarantee one property:

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
    ``proc_cnt`` has dropped to zero. The peer object serving that rank is
    left in place as an inert **tombstone** at its existing client-array
    slot (it is not freed, its slot is not nulled, and ``info->peerid`` is
    not moved), while the rank's **registration** (the ``pmix_rank_info_t``
    on ``nptr->ranks``) is preserved so the client can init again. The
    tombstone is reclaimed later, at the next reconnect for the rank or at
    namespace deregistration. See `Tombstoning the peer on finalize`_.

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


Tombstoning the Peer on Finalize
--------------------------------

When the last live process for a rank finalizes (``proc_cnt`` reaches
zero), the server **must not** free the peer object, null its client-array
slot, or move ``info->peerid`` at socket-close time. Instead it leaves the
peer in place as an inert **tombstone**:

* Leave the object at its existing slot in ``pmix_server_globals.clients``
  with ``finalized == true``. By the time ``pmix_server_peer_finalized``
  runs, ``lost_connection`` has already stopped the peer's send/receive
  events and closed its socket, and the ``finalized`` flag short-circuits
  every send path (the ``PMIX_PTL_SEND_*`` macros and
  ``PMIX_SERVER_QUEUE_REPLY``). The tombstone is therefore harmless: it
  cannot arm a doomed send on its closed socket, yet it keeps
  ``info->peerid`` pointing at a real, resolvable peer object for the
  rank.

* Leave the rank's ``pmix_rank_info_t`` on ``nptr->ranks`` (the
  registration), leave the namespace's GDS job data in place, and leave
  ``info->peerid`` unchanged.

* Reduce only the rank's live-process count (``proc_cnt``). The finalized
  count (``nfinalized``) is **left as-is** — the tombstone is still a
  ``finalized`` peer and must keep being counted as one.

The tombstone is reclaimed at a later, quiescent point:

* **On reconnect.** When the client calls ``PMIx_Init`` again, the
  connection handler finds the stale tombstone at ``info->peerid``, frees
  it (nulls the slot, drops the ``nfinalized`` count it was holding, and
  ``PMIX_RELEASE``\ s it), then allocates a **fresh** peer for the rank.
  The prior cycle has fully finalized by the time a new init arrives, so
  no collective from it can still be in flight — this is a safe point to
  mutate the array and the count.

* **On deregistration.** If the rank never reconnects (the common case for
  a spawned child that runs once and exits), the tombstone is freed when
  the host deregisters the namespace.

Why leave a tombstone rather than free the peer at socket-close? Two
earlier designs freed (or destruct/reconstructed) the object the moment
the socket dropped, from inside ``lost_connection``. Both mutated shared
per-namespace/per-rank state — nulling the ``clients`` slot, and in
particular clearing ``info->peerid`` to ``-1`` — while a *different*
process's spawn/connect/disconnect collective or direct-modex get was
still walking the ``clients`` array and resolving ranks through
``info->peerid``. A rank whose ``peerid`` had just been cleared could no
longer be resolved to its GDS module, so the in-flight collective or get
stalled: an architecture- and timing-sensitive hang of multi-local-process
``MPI_Comm_spawn``. Leaving the finalized peer in place keeps every
``peerid`` valid and every array slot populated, so nothing a concurrent
collective touches changes underneath it; the object is reclaimed only
once no such operation can be in flight.

A tombstone whose slot has been taken over by a newer peer for the same
rank — a fork/exec'd clone that is still running, or a fast re-init that
reconnected before the socket-close was processed — is no longer named by
``info->peerid``. Such a *stranded* object is freed immediately in
``pmix_server_peer_finalized`` (nulling only its own slot, never the live
referenced one, and never touching ``info->peerid``), so a departing
clone or a raced reconnect cannot leak a peer or its finalized count.

Job-scoped accounting must stay honest across cycles. The rank's
``proc_cnt`` tracks live processes and returns to zero when the last one
departs; ``nfinalized`` counts finalized peers and is decremented only
when a tombstone is actually reclaimed (on reconnect) or freed (stranded),
so that ``nlocalprocs == nfinalized`` remains an accurate "all local
processes finalized" test no matter how many times clients cycle.

What finalize does **not** do
    Finalize does not remove the rank from ``nptr->ranks``, does not drop
    the namespace's GDS job data, and does not touch network or
    programming-model resources reserved for the job. Those are job-scoped
    and survive until the host deregisters. Transient, client-scoped
    registrations that are keyed to the peer — event registrations,
    I/O-forwarding requests, in-flight direct-modex requests, and cached
    notifications targeting the client — are pruned as part of the
    teardown so they do not linger.

Tombstone mechanics
~~~~~~~~~~~~~~~~~~~~

The tombstone is driven from ``lost_connection`` in the transport layer
when a cleanly-finalized client's socket drops, and its correctness rests
on two per-namespace counters staying honest across every cycle.

**Peer states.** A client peer moves through three states:

.. list-table::
   :header-rows: 1
   :widths: 30 12 8 12

   * - State
     - ``finalized``
     - ``sd``
     - in ``proc_cnt``?
   * - Active
     - ``false``
     - ≥ 0
     - yes
   * - Finalizing (``FINALIZE_CMD`` seen, socket not yet dropped)
     - ``true``
     - ≥ 0
     - yes
   * - Tombstone (socket dropped, kept at ``info->peerid``)
     - ``true``
     - −1
     - no

A tombstone is deliberately left with ``finalized == true`` so it stays
inert to every ``finalized``-guarded send path while it waits at
``info->peerid`` to be reclaimed. It is removed from ``proc_cnt`` (its
process is gone) but remains counted in ``nfinalized`` (it is still a
finalized peer), and it stays resolvable through ``info->peerid`` so a
concurrent collective or get can still find the rank's GDS module.

**The two counters.**

* ``info->proc_cnt`` is the number of **live** processes (clones) for the
  rank. It is incremented in the connection handler when a peer connects
  and decremented on **every** peer departure — a clean finalize drop
  *and* an abnormal termination. Decrementing on abnormal death too
  (in ``lost_connection``) keeps a later clone's tombstone/reclaim
  accounting correct.

* ``nptr->nfinalized`` is the number of client peers currently in the
  ``finalized == true`` state, **including tombstones**. It is incremented
  when ``FINALIZE_CMD`` is received and decremented only when a tombstone
  is reclaimed — on reconnect, or when a stranded tombstone is freed.
  Keeping this balanced is what lets the ``nlocalprocs == nfinalized``
  test at namespace teardown fire the programming-model "all local
  processes finalized" callback.

**Finalize on socket drop.** When a cleanly-finalized peer's socket drops,
``pmix_server_peer_finalized`` decrements ``proc_cnt``. If the peer is
still the rank's referenced peer (``info->peerid == peer->index``) it is
left in place as a tombstone and nothing else is touched. Otherwise a
newer peer already owns the rank's slot and this object is stranded, so it
is freed at once: its own ``clients`` slot is nulled (never the live
referenced one), ``nfinalized`` is decremented, and it is
``PMIX_RELEASE``\ d. A reference still held by a pending non-blocking
collective (whose caddy sits on the tracker's ``local_cbs``) or an active
sensor is harmless in either case: a tombstone is simply retained a while
longer, and a released stranded object is not torn down until that last
holder drops its reference.

**Reclaim on reconnect.** The connection handler, before allocating a
fresh peer for a reconnecting rank, checks ``info->peerid``: if it names a
``finalized`` peer and the rank has no live process (``proc_cnt == 0``),
that tombstone is reclaimed — its slot nulled, ``nfinalized`` decremented,
and the object released — and only then is a new peer allocated. This is
the point at which ``info->peerid`` is repointed, by the normal
connection-handler assignment, to the freshly allocated peer.

**``peerid`` stays valid.** Because a finalize never clears
``info->peerid`` to ``-1``, a local ``PMIx_Get`` for the rank (which
resolves committed data only while ``info->peerid`` names a resolvable
peer) continues to work against the tombstone until the rank either
reconnects or is deregistered.


Deregistering a Client
----------------------

``PMIx_server_deregister_client`` is the host's declaration that a
specific rank is gone and will not init again. Unlike finalize — which
tombstones the peer object but keeps the rank's registration so it can
init again — deregistration also **removes the registration**:

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

* Remove and release **all** peer objects still belonging to the
  namespace (any rank whose processes have not all finalized), nulling
  their client-array slots and freeing every ``pmix_rank_info_t`` on
  ``nptr->ranks``.

* Release the network and programming-model resources reserved for the
  job, run the namespace epilog, and prune every remaining reference to
  the namespace: event and I/O-forwarding registrations, cached
  notifications targeting any of its procs, and finally the namespace
  object itself.

Nothing that names the namespace may remain after this call returns.


State Ownership Summary
-----------------------

The following table classifies the state and states which trigger frees
it. "Released" means the object is freed and its client-array slot nulled;
"Tombstoned" means it is left inert in place and reclaimed later; "Kept"
means the trigger leaves it in place.

.. list-table::
   :header-rows: 1
   :widths: 34 22 22 22

   * - State
     - Client finalize (all clones)
     - Deregister client
     - Deregister nspace
   * - Peer object (``pmix_peer_t``)
     - Tombstoned (kept at slot; reclaimed on reconnect/deregister)
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


Client-Side Teardown
--------------------

Everything above concerns the state a *server* holds for a client. The
cycling guarantee has a second half: the client's own library must tear
itself down on ``PMIx_Finalize`` so that a subsequent ``PMIx_Init`` in
the same process starts from a clean slate. Unlike the server side —
where finalize deliberately *preserves* registration state — the client
side has no such asymmetry to manage: a client ``PMIx_Finalize`` (once
the init reference count reaches zero) is a full teardown of the library,
and a later ``PMIx_Init`` is a brand-new library instance that opens a
fresh socket and reconnects.

The teardown runs ``PMIx_Finalize`` → ``pmix_rte_finalize`` → framework
closes plus ``pmix_finalize_util`` and the release of the progress
thread; it is the mirror image of ``PMIx_Init`` → ``pmix_rte_init`` →
``pmix_init_util`` plus the framework opens. The governing principle is
**symmetry**: finalize must undo exactly what init did, leaving no
residue that the next init would either trip over or skip.

The reference-count guard
    ``PMIx_Init`` is reference-counted: nested init/init/…/finalize
    sequences only tear down on the *last* finalize, when the counter
    returns to zero. The full teardown described here applies to that
    final finalize. A nested (non-final) finalize must leave the library
    running and untouched.

Three requirements make the symmetry real, and an implementation **must**
uphold all three:

1. **Every one-time-init latch must be reset.** Any ``static`` boolean or
   counter that guards a "do this once" step in init (module
   registration, key creation, subsystem setup) must be cleared by the
   corresponding finalize, so the step runs again on the next init.
   Leaving a latch set silently *skips* re-initialization on the second
   cycle — the failure is not a crash at finalize but wrong or missing
   state later. The multi-select framework ``initialized`` flags, the MCA
   parameter-registration latch, and the attribute/output/show-help
   subsystems all follow this rule and are the model to copy.

2. **Every freed global must be nulled, and every per-cycle flag reset.**
   A global pointer whose target is freed during finalize must be set to
   ``NULL`` in the same step. Two hazards follow from not doing so:

   * **Dangling reuse.** A non-``NULL`` pointer to freed memory defeats
     the ``if (NULL == p)`` guards that would otherwise make a
     between-cycles access safe; it reads as "still valid" and yields a
     use-after-free. The event base, the shared progress-thread tracker,
     and the cached server peer are pointers of this kind.

   * **Stale mode flags.** A flag that records *how* this cycle was set
     up — most importantly the singleton indicator, which records that
     the process came up with no server and therefore constructed the
     server-side I/O-forwarding lists — must be reset on every fresh
     init, not only when it happens to be toggled. A stale singleton flag
     carried from a no-server cycle into a with-server cycle drives
     finalize to tear down lists the with-server path never built.

3. **Init and finalize of the utility layer must stay in lockstep.**
   ``pmix_init_util`` opens a fixed set of low-level subsystems (output,
   install-dirs, show-help, the keyval parser, the MCA base and its
   variable system, the network and interface helpers). Each must be
   closed on finalize. Whether the close lives in ``pmix_finalize_util``
   or inline in ``pmix_rte_finalize`` is an implementation choice, but the
   two lists must be kept in correspondence: a subsystem added to init
   without a matching teardown persists across a cycle and breaks the
   clean-slate guarantee for the next init.

Platform caveat: destructor support
    On platforms that provide neither a compiler ``destructor``
    attribute nor linker ``-fini`` support, the library cannot guarantee
    cleanup at unload, and re-initialization after finalize is
    **explicitly unsupported** — a second ``PMIx_Init`` returns
    ``PMIX_ERR_NOT_SUPPORTED`` rather than risk an inconsistent restart.
    The cycling guarantee therefore applies only where destructor or
    ``-fini`` support is present (the mainstream case).


Tool-Side Teardown
------------------

The tool role has the same cycling guarantee as the client: a process
must be able to cycle ``PMIx_tool_init`` → work → ``PMIx_tool_finalize``
repeatedly, each init a clean slate. ``PMIx_tool_init`` /
``PMIx_tool_finalize`` are a **separate** entry-point pair from the client
``PMIx_Init`` / ``PMIx_Finalize`` — they do not share the client's
finalize code — so the three symmetry requirements above have to be
satisfied independently in the tool path. Two of them bite the tool
specifically:

The reference-count guard and the one-time latch
    ``PMIx_tool_init`` is reference-counted through its own counter, and
    it sets the shared one-time-init latch (``pmix_globals.init_called``).
    ``PMIx_tool_finalize`` must decrement that counter, tear down only on
    the last matching finalize, and **reset the latch**. Forgetting the
    reset is the most visible cycling failure: the second
    ``PMIx_tool_init`` sees the latch still set, concludes the library is
    already initialized, and returns ``PMIX_ERR_INIT`` — the tool never
    comes back up.

The tool constructs server state every init
    Because a tool may itself act as a server (a launcher connects *up* to
    its own server while *listening* for its children), ``PMIx_tool_init``
    unconditionally initializes the ``server_globals`` bookkeeping —
    constructing the client array and every server-side list — on **every**
    init, whether or not the tool will act as a server. Tool finalize must
    therefore destruct **all** of those lists, not just the subset a
    particular run happened to populate; any list left un-destructed leaks
    its contents each cycle when the next init reconstructs over it. For
    the same reason the tool must free (and ``NULL``) the ``tmpdir`` and
    ``system_tmpdir`` strings: the init-time code refreshes them only when
    they are ``NULL``, so a surviving pointer both leaks and silently
    pins the previous cycle's temporary-directory choice, ignoring a
    changed environment on the next init. The tool must likewise destruct
    the client-side ``peers`` array backing store and the static IOF
    sinks, exactly as the client finalize does.

    The reset of the *server module* latch
    (``pmix_server_globals.module_set``, set by
    ``PMIx_tool_set_server_module``) is **not** part of this pair: that
    latch is owned by a separate registration call a launcher makes
    outside ``PMIx_tool_init``, and the server reference finalizer does
    not clear it either. A launcher that re-registers its server module
    between cycles is therefore out of scope here and handled separately.


Invariants
----------

An implementation of the above must uphold these invariants:

* **No cross-cycle carryover.** The finalized peer serving a rank is
  reclaimed (freed) no later than the rank's next reconnect, at which
  point a fresh object is allocated, so a new cycle cannot inherit queues,
  event registrations, tag counters, or datastore assignments from the
  previous one.

* **No orphaned peers.** Cycling init/finalize must not accumulate
  unreferenced peer objects in the ``clients`` array. A finalized peer is
  left as a single tombstone at ``info->peerid`` and is reclaimed when the
  rank reconnects (or when the namespace is deregistered); a peer that a
  newer connection has already displaced is freed immediately. Either way
  at most one finalized object per rank is ever kept, and only while the
  rank's registration itself persists.

* **No shared-state mutation while a collective is in flight.** Finalize
  must not free a peer, null a live ``clients`` slot, or move a live
  ``info->peerid`` from ``lost_connection``: a concurrent
  spawn/connect/disconnect collective or direct-modex get may be walking
  that state for another process. All such mutation is deferred to a
  quiescent point (reconnect or deregistration), or confined to a stranded
  object no live ``peerid`` names.

* **No counter drift.** Per-namespace finalized accounting and per-rank
  ``proc_cnt`` must return to values consistent with the actual number of
  connected processes after each cycle, so that the job-level "all local
  processes finalized" condition and any resource-release keyed on it
  remain correct no matter how many times clients cycle.

* **Registration outlives finalize; only deregistration is terminal.**
  The ``pmix_rank_info_t`` and the client-array slot persist across
  finalize and are removed only by an explicit host deregister. GDS job
  data and job resources persist until ``PMIx_server_deregister_nspace``.

* **The client library reinitializes from a clean slate.** After the
  final ``PMIx_Finalize``, no ``static`` init-once latch remains set, no
  global pointer references freed memory, and no per-cycle mode flag
  retains a value from the previous cycle. The next ``PMIx_Init`` behaves
  exactly as the first.
