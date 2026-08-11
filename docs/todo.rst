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

Deferred work
-------------

The ``PMIx_Init`` debugger-wait handler can outlive its return object
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``PMIX_EVENT_RETURN_OBJECT`` is stored as a bare ``cbobject`` pointer,
and here it points at a lock on ``PMIx_Init``'s own stack frame.  On the
success path the handler is ``PMIX_EVENT_ONESHOT`` and removes itself
when it fires; only the notification-failure return leaves it
registered.  The obvious fix does not work: ``PMIx_Deregister_event_handler``
gates on ``pmix_globals.initialized``, which ``PMIx_Init`` does not set
until forty lines later, so the public API is a no-op from there and
removing the handler means open-coding the thread-shift the entry point
performs.  Weigh that against the reachability — the process must ignore
a failed ``PMIx_Init``, keep running, and then be sent a
``PMIX_DEBUGGER_RELEASE`` it never asked for.

A cached notification caddy still leaks on the cache-aging path
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A cached notify caddy takes a reference that the event hotel owns.  The
"all targets notified" checkout path was fixed to release it; pure
cache-aging still leaks the working reference.  The fix needs
"cached" and "posted upstairs" to be told apart — both currently set
``holdcd = true`` — which is why it was deferred rather than attempted
alongside the other leak fixes.

Residual leaks on the group-leave path
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Valgrind runs against a server library embedded in a launcher show small
*indirect* libpmix leaks rooted in the departed client's peer and block
cleanup — ``_register_client``, ``harvest_envars``,
``ptl_base_connection_handler``, and the tracker's children.  They are
entangled with a leak in the host's own group code, which is what makes
attributing and fixing them delicate.  Left for separate, careful work.

Two shared-state exposures that need a structural answer
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* The ``spawn_iof_flags`` stand-in is a single process-wide slot,
  written from the caller's thread and read on the progress thread.  It
  is safe for the case it was built for, but two spawns in flight at
  once from one process means the second overwrites the first's flags.
  A tool that needs concurrent spawns formatted differently forces the
  stand-in to become per-request.
* ``PMIx_Spawn_nb`` in a server role walks
  ``pmix_server_globals.clients`` unlocked, on whatever thread the host
  called it on, while the progress thread adds and removes clients.  The
  cure is to thread-shift the whole entry point; the sibling call in
  ``pmix_monitor.c`` has the same exposure.

Smaller items carried forward
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIx_Value_get_number()`` is called without a ``NULL`` check on
  ``kv->value`` at three fetches in ``src/client``'s ``get_data()``.
  Nothing demonstrates a local datastore fetch producing such a kval, and
  if it is closed the screen belongs inside ``PMIx_Value_get_number``
  rather than at the call sites, since every caller in the tree is
  equally exposed.
* ``req->flags = cd->flags`` in ``pmix_server_process_iof()`` aliases the
  caddy's ``file``/``directory`` strings, which are freed when the caddy
  is released — so the IOF request is left holding two dangling
  pointers.  It is harmless only because nothing reads them today.
* ``pmix_globals.init_called`` is set with ``__atomic_test_and_set`` and
  cleared with a plain assignment.  All three roles do this, and the
  only interleaving that reaches it is a concurrent
  ``PMIx_Init``/``PMIx_Finalize``, which is already unsupported.
* ``refcb()`` in ``src/client`` overwrites the store status with the next
  unpack's, so a store failure while absorbing a cache refresh is
  silently dropped.
* ``resolve_peers()``'s pre-v3.2 compatibility branch is dead code — the
  rank it sets is unconditionally reset two lines later.  Not removed
  without a pre-v3.2 server to test against.
* The command-line parser drops a positional argument placed *before* an
  option, a consequence of ``getopt`` reordering.  Put options first.
* The relay in ``src/tool`` assumes the downstream tool and the upstream
  server negotiated the same ``bfrops`` module and buffer type.  A tool
  forces the same modules on itself and its server, so this holds today,
  but the code does not check it.
* The event-caching branch in the tool's ``_notify_complete`` appears to
  be unreachable, and ``src/client`` has no equivalent branch at all.
  Tracked as `openpmix#4101 <https://github.com/openpmix/openpmix/issues/4101>`_.

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
