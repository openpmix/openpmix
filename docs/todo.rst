Known gaps and deferred work
============================

This page inventories issues that were identified during the deep
reviews of the PMIx source tree but were **deliberately not fixed at
the time they were found**.  Each entry records what the problem is,
why it was left alone, and what closing it would take.

Three kinds of entry appear here:

* **Open decisions** — a real defect whose fix requires a judgement
  that the review was not entitled to make on its own (a change to a
  published attribute, a released API's behavior, or the PMIx
  Standard).
* **Deferred work** — a real defect whose fix is larger, riskier, or
  more entangled than the change it was found alongside.
* **Coverage gaps** — code that is believed correct but that no test
  reaches, usually because reaching it needs fault injection or
  hardware the test environments do not have.

Entries that are *by design* are also listed, under their own heading,
so that they are not repeatedly "rediscovered" and re-fixed.

.. note:: Each directory's ``AGENTS.md`` carries the full reasoning for
          the items found in it.  Those files are orientation maps, not
          work logs, so this page is the place that records an item as
          still **open**.  Re-verify an entry before acting on it; the
          code moves.

.. note:: **Every entry below was checked against the tree on
          2026-08-11.**  That pass retired three of them — one whose fix
          had landed a month before this page first recorded it as open,
          one that had been fixed since, and one that a fresh valgrind
          run against a launcher could no longer reproduce — and
          corrected a claim that was never true.  Treat that as the
          standing expectation rather than a one-off: an entry here is a
          lead, not a finding.  The two "never validated" coverage gaps
          are the only remaining entries that cannot be checked by
          reading the tree, so they are where the next stale one will be.

          Three of the "smaller items carried forward" were closed on
          2026-08-13 — the ``PMIx_Value_get_number`` screen (now in the
          function itself, where every caller in the tree gets it), the
          ``init_called`` clear (now ``pmix_atomic_clear``, the required
          partner of the ``__atomic_test_and_set`` that sets it), and
          ``refcb()``'s dropped store status.

Open decisions
--------------

A deregistration cannot retract what a namespace already holds
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`
removes entries from the server's global cache, and the qualified form of
it now removes exactly what the request named.  What it cannot do is take
the information back from a namespace that already has it: the cache is
copied into a namespace's data store once, when that namespace is first
registered — ``hash_cache_job_info``, guarded by the per-namespace
``gdata_added`` — and nothing re-reads it afterwards.  So a deregistration
governs the namespaces registered *after* it, while a running job keeps
its copy.  The rationale the call is documented with — that a host may
decide client processes should no longer have access to the information —
is therefore only half served.

Closing it needs a delete-a-key entry point that the ``gds`` module struct
does not have.  Adding one is not symmetric across the components.  For
``hash``, the server could remove the key from each namespace's tables and
message its local clients to do the same in theirs.  For ``shmem3`` that
does not help: a client reads the shared segment directly, and removing
data from it means putting a lock on a read path that is lock-free by
design and is the reason that component exists.  Any real fix has to
answer the ``shmem3`` case first; the messaging half is not worth building
on its own.

Two concurrent spawns cannot be formatted differently
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_globals.spawn_iof_flags`` is a single process-wide slot holding
the output-formatting directives of the spawn a tool has in flight.  It
exists because forwarded output can arrive *before* the spawn reply names
the namespace it belongs to, and until it existed that output was
formatted with the process-wide defaults instead — so ``prun --output
tag`` lost the tag on whichever rank happened to be quickest.  Two spawns
in flight at once from one process means the second overwrites the
first's flags.

This cannot simply "become per-request", which is how it was recorded
before.  The reader is ``spawn_or_global_flags()`` in
``src/common/pmix_iof.c``, reached from ``pmix_iof_write_output()``
precisely when the arriving output names a namespace we have no record
of; the only things in scope there are that unknown namespace and the
stream.  Nothing identifies *which* in-flight spawn the output came
from, and nothing can until the reply arrives — which is the very thing
the stand-in exists to cover for.  So the choices are: keep one slot and
accept that concurrent spawns share it; let the reply establish the
flags, which re-opens the window the stand-in was built to close; or
carry something in the IOF message that names the spawn, which is a
wire-format and Standard question rather than a bug fix.

A tool caches an event nobody handled; a client discards it
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When a local event chain in a tool ends with nothing having handled the
event, ``_notify_complete`` in ``src/tool/pmix_tool.c`` parks a copy in
the notification hotel so that a handler registered *later* can still be
given it.  A client in the same position simply releases the chain — the
equivalent branch does not exist in ``src/client/pmix_client.c``.

Only one of those can be right, and choosing is a statement about
event-delivery behavior in a released library rather than a bug fix.
Either the tool's branch is unreachable and should go, or it is correct
and the client is losing events a late registrant was entitled to see.

Nothing has been found that reaches it.  A server forwards an event to a
tool only if that tool registered for the code, and it applies the
tool's affected-proc filter before forwarding, so a tool does not
normally receive an event it has no handler for.  The one shape that
might — an event already on the wire when the handler is deregistered —
**has not been confirmed**, and should not be treated as evidence until
somebody checks whether the server stops forwarding before or after the
deregistration round-trips.

What is *not* at issue is the branch's correctness if it is entered:
its two unchecked allocations and its caddy-leaking failure arms are
fixed, as are the same two in the event layer's copy of the block
(``_notify_client_event``).  Tracked as
`openpmix#4101 <https://github.com/openpmix/openpmix/issues/4101>`_.

Deferred work
-------------

Smaller items carried forward
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The items that stood under this heading were closed on 2026-08-13; what
is left of them is recorded here, because in two cases the fix does not
cover the whole of what the entry described.

* **The pre-v3.2 branch of** ``resolve_peers()`` **no longer carries a
  dead store**, but what it *meant* to do is still not done.  The branch
  assigned ``PMIX_RANK_WILDCARD`` and was then overwritten with
  ``PMIX_RANK_UNDEF`` before anything read it; the assignment is gone and
  the branch now varies only the ``key`` and ``ninfo``, which is what it
  always really did.  The legacy path resolves because ``try_fetch()``
  retries an ``UNDEF`` rank as ``WILDCARD``.  **Fetching at ``WILDCARD``
  directly, as the branch intended, is still untried** — that is a
  behavior change on a path only a pre-v3.2 server exercises, and there
  is none to test against.
* **A malformed** ``PMIX_QUALIFIED_VALUE`` **or a NULL key arriving in a
  cache refresh now fails the enclosing** ``PMIx_Get``.  See the
  ``refcb()`` entry in ``src/client/AGENTS.md`` for why all-or-nothing is
  the only answer an application can act on.  Recorded here because it is
  the one behavior change in that group of fixes.

Coverage gaps
-------------

* **The out-of-memory and finalize-race arms of the server switchyard's
  host callbacks.**  ``op_cbfunc``, ``op_cbfunc2`` and ``resop_cbfunc``
  in ``src/server/pmix_server_switchyard.c`` each have two arms that do
  not thread-shift.  Both were fixed to release the caddy they own, but
  neither is reachable from a unit test without fault injection — a
  failed allocation, or the progress thread stopping while a host
  completion is in flight.
* **``test/simple/simptest`` cannot host a spawn.**  Its ``spawn_fn``
  calls ``PMIx_server_setup_application()``, a thread-shifting API, and
  then waits on the result — but ``spawn_fn`` is the host callback and
  runs *on* the server's progress thread, so the fake launch deadlocks.
  Spawn coverage therefore lives in the multi-node suite rather than in
  ``make check``.  Do not add a ``run_*.pl`` that spawns until that path
  is made asynchronous.
* **The reply handling of the ``PMIx_Compute_distances`` pair is not
  covered anywhere,** and cannot be: two of the three paths are
  allocation failures and the third needs a server reply whose declared
  count exceeds its payload.  Reaching the receive path at all requires
  a client whose *local* hwloc computation failed.
* **Two fixes in ``src/tool`` ship without regression tests** because
  the conditions cannot be arranged in ``make check``: the SIGCONT
  handler for forwarded stdin needs a tty, and the late-finalize-reply
  guard needs a server that answers the finalize handshake more than
  five seconds late.
* **The debugger-wait teardown in ``PMIx_Init`` is not exercised.**
  Reaching it needs the debugger-stop key set on the namespace *and* the
  ``PMIX_READY_FOR_DEBUG`` notification to then fail, which no test
  environment can arrange without fault injection.
* **The process-set and resolve examples have never been leak-validated.**
  Both hang in the test environments used so far, so valgrind is killed
  before ``PMIx_Finalize`` runs and the report is inconclusive.
* **``pps`` has never been validated against a live process-table
  server.**  Its no-connect paths are covered by the tools smoke test;
  the proc-table rendering is not.

Not defects — by design
-----------------------

These look like bugs and are not.  They are recorded so that they are
not "fixed" by a later reader.

* **The fence modex bucket reported as a libpmix leak belongs to the
  host.**  ``pmix_server_fence`` unloads the assembled bucket with
  ``PMIX_UNLOAD_BUFFER`` and hands the bare pointer to
  ``pmix_host_server.fence_nb``; ownership transfers on the call, and the
  request direction carries no ``(release_fn, release_cbdata)`` pair the
  way the reply direction does.  We cannot free it on return — the host
  parks it and reads it from another thread — so a host that does not
  free it leaks about 128 bytes per collecting fence.  In a valgrind run
  against a launcher this looks exactly like a libpmix leak and nothing
  else does: the allocation is ``pmix_server_collect_data``, the stack
  bottoms out in ``progress_engine``, and there is no ``prte_`` frame
  anywhere in it.  Both in-tree hosts (PRRTE and ``test/simple/simptest``)
  currently leak it.  See ``src/server/AGENTS.md``.

* **The event-caching blocks assign ``cd->nondefault`` inside their**
  ``0 < chain->ninfo`` **guard**, which reads as though a non-default
  event carrying no info would be parked as a default one and then
  delivered to default handlers that should not see it.  It cannot
  happen: every path that sets ``chain->nondefault`` reads it out of a
  ``PMIX_EVENT_NON_DEFAULT`` directive in the info array, so the flag is
  only ever true when there is info to have carried it.  Hoisting the
  assignment out of the guard is equivalent, not a fix.  Both copies —
  ``_notify_complete`` in ``src/tool`` and ``_notify_client_event`` in
  ``src/event`` — are left as they are.
* **``pmix_srand()`` copies the seeded state into a file-static buffer**
  (``src/util/pmix_alfg.c``).  It looks like a footgun, but it is the
  only way to seed the global that ``pmix_random()`` reads, and the unit
  test deliberately holds down that behavior.  ``pmix_random`` is unused
  in-tree and the real ``pmix_srand`` callers use their own buffers, so
  the "last writer wins" hazard is not reachable.  Do not drop the copy
  without giving ``pmix_random`` another way to be seeded.
* **``wait_signal_callback()`` reads the child-list size without a
  lock** (``src/common/pmix_pfexec.c``).  This is a formal data race,
  kept on purpose: the child is appended to the list before the ``fork()``
  that can generate its ``SIGCHLD``, so the count cannot be stale for a
  child that matters, and removing the guard would make pfexec call
  ``waitpid(-1)`` on every ``SIGCHLD`` in a process with no children of
  its own.  If it is restructured, replace the read with a counter
  maintained across all six add/remove sites.
* **A failed ``pmix_rte_init`` returns without unwinding.**  It emits a
  diagnostic and returns, deliberately leaving the frameworks, globals
  and event base it had already brought up, because a failed
  ``PMIx_Init`` aborts the process.  New failure points should follow the
  same pattern.
* **``get_job_data`` returning success with an empty buffer is safe.**
  The requesting client initializes its reported status to
  ``PMIX_ERR_NOT_FOUND`` and overwrites it only if a value turns up, so
  an empty success degrades to not-found at the requester.
* **``pmix_server_job_ctrl`` creating a namespace for an unknown target
  is intentional.**  A job-control request may name a job this server has
  not been told about yet, and the epilog directives need somewhere to
  hang; the entry is reused when registration arrives.
* **The bare ``atomic_bool`` fields in ``pmix_globals_t``** are correct,
  merely inconsistent with the typedefs used elsewhere, and the
  ``PMIX_C_HAVE_*`` defines in the installed ``pmix_config.h`` are now
  always ``1`` but are retained in case an out-of-tree consumer tests
  them.
