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

The page opens with a fourth thing that is not an issue at all —
**Review coverage**, which records where the deep review has and has not
yet been, so that the remaining work is visible rather than rediscovered
each time somebody asks.

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

          **Checked again on 2026-08-15**, which retired one more: the
          open decision about a tool caching an event nobody handled
          while a client discarded it.  The fan-out fix for
          `openpmix#4101 <https://github.com/openpmix/openpmix/issues/4101>`_
          answered it — a client now parks such an event, the tool's
          unreachable copy of the parking code is gone, and
          ``test/unit/event_forward`` covers both halves.  Two entries
          were corrected rather than retired, both because the code had
          moved: the tool's SIGCONT stdin handler now lives in
          ``src/common``, and the ``cd->nondefault`` blocks are down to
          one that still assigns inside its guard.  Everything else was
          re-verified against the tree and stands as written.

          A second entry went the same way on 2026-08-16: the open
          decision about two concurrent spawns not being formattable
          differently.  It listed three unattractive choices and missed
          a fourth — hold the output until the reply that can identify
          it arrives, rather than formatting it on a guess at all.  The
          entry is retired; see ``test/unit/iof_pending``.  Worth noting
          *why* it read as closed, since the shape recurs: it framed the
          problem as "which flags does this output get", which really
          has no answer at that moment, when the question that does have
          one is "does this output have to be formatted yet".

Review coverage
---------------

Assessed on **2026-08-15** from the commit history.  Move an entry out of
"Not yet reviewed" as its review lands, and refresh the churn figures in
"Reviewed, but changed materially since" when a re-review closes one.

.. note:: **An** ``AGENTS.md`` **is not evidence of a review.**  Every
          directory under ``src/`` has one; most were written in the July
          2026 orientation sweep, before any review started.  What marks
          a reviewed directory is a run of repair commits — "Repair…",
          "Screen…", "Close the … holes", "Sweep the leftovers in…" —
          and a commit that records the result in the guide.

Reviewed and current
^^^^^^^^^^^^^^^^^^^^

``src/class``, ``src/common``, ``src/event``, ``src/include``,
``src/runtime``, ``src/tool``, ``src/tools``, ``src/mca/base``,
``src/mca/bfrops``, ``src/mca/gds/base``, ``src/mca/gds/hash``,
``src/mca/ptl``, and ``bindings/python``.  ``src/client``, ``src/server``,
``src/hwloc``, ``src/util`` and ``src/mca/gds/shmem3`` were reviewed too,
but have moved since — see below.

Not yet reviewed
^^^^^^^^^^^^^^^^

Each of these has an orientation guide and nothing else: no findings were
ever recorded in it, and only drive-by fixes have landed.  Ordered by
size, which is a rough proxy for how much there is to find.

* ``src/mca/pstat`` (with ``plinux``, ``pmacos``, ``test``) — 4988
  lines.  Four bug fixes on 2026-07-04, never a sweep.
* ``src/mca/pnet`` (with ``base``, ``tcp``, ``opa``, ``nvd``,
  ``simptest``) — 4064 lines.  Revived on 2026-07-15 and not looked at
  since.
* ``src/mca/preg`` (with ``native``, ``raw``, ``compress``) — 2641
  lines.  It parses regular expressions arriving off the wire, which
  makes it the highest-risk member of this list.
* ``src/mca/pgpu`` (with ``amd``, ``intel``, ``nvd``, ``test``) — 2467
  lines.  The 2026-07-15 "repair the stale components" work was not a
  review.
* ``src/mca/pmdl`` (with ``ompi``) — 2422 lines.
* ``src/util/keyval`` — 2397 lines of lexer and parser, and the only
  directory in ``src/`` with **no** ``AGENTS.md`` at all.
* ``src/mca/pcompress`` (with ``zlib``, ``zlibng``, ``zstd``, ``lz4``) —
  2394 lines.  ``zstd`` (2026-08-08) and ``lz4`` (2026-08-15) are new
  code that has never been read by anyone but its author.
* ``src/mca/plog`` (with ``smtp``, ``stdfd``, ``syslog``) — 2176 lines.
  Three fixes on 2026-07-04 only.
* ``src/mca/psec`` (with ``native``, ``none``, ``munge``,
  ``dummy_handshake``) — 1901 lines.  This is the connection-handshake
  code; two small fixes on 2026-07-14/15 are all it has had.
* ``src/mca/psensor`` (with ``file``, ``heartbeat``) — 1398 lines.
* ``src/mca/pdl``, ``src/mca/pinstalldirs``, ``src/mca/pif`` — about
  3200 lines between them, and the lowest risk of the group.
* ``src/threads`` — 737 lines.  The 2026-07-17 cleanup fixed a TSD key
  leak and removed dead code, but was not a full pass.

Outside ``src/``, nothing has been reviewed: ``examples/`` (16678 lines,
leak-swept only), ``test/simple`` (11011), ``test/unit/util`` and
``test/unit/mca``, and the public headers in ``include/``.

Reviewed, but changed materially since
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Ordered by how much of the directory the review no longer covers.

#. **``src/util``** — reviewed 2026-07-17/18, the oldest review in the
   tree.  31 commits and +1440/-203 across 16 files since, none of it
   re-read: the CLI option-parsing rework, the ``dirpath`` conversion to
   descriptor-based operations, and the ``pmix_hash`` qualifier arrays.
#. **``src/hwloc``** — reviewed 2026-08-02.  The 2026-08-10 → 15 work
   added a device enumerator and reworked the distance computation,
   +753 lines in ``pmix_hwloc.c`` alone, against a five-line touch to the
   guide.  Effectively new, unreviewed code.
#. **``src/mca/gds/shmem3``** — reviewed 2026-08-03, then redesigned
   between 2026-08-04 and 15 (string key index, per-segment key indexes,
   tables sized by rank): 16 commits, +833/-916.  The guide was rewritten
   alongside it, but a redesign is not a review.
#. **``src/client``** — the per-file review is current through
   2026-08-13.  What sits outside it is the group-invite and context-id
   work of 2026-08-15: +355 lines in ``pmix_client_group.c`` and +245 in
   ``pmix_client_spawn.c``.
#. **``src/server``** — the 2026-08-09 → 15 commit run *is* the review,
   performed after the source was split into function-oriented files, so
   the body of the directory is covered.  Outside it is the same late
   feature work: group invite/endpoint exchange (2026-08-15) and the
   spawn output-forwarding inheritance (2026-08-12/13).

Lower priority, with current guides and modest churn: ``src/mca/base``
(+162/-79), ``src/common`` (+197/-12), ``src/event`` (+196/-11), and
``src/mca/ptl`` (mostly deletions).

Open decisions
--------------

A pruned deregistration is not propagated
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This entry used to say that
:ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`
could not take information back from a namespace that already held it —
the server's global cache is copied into a namespace's data store once,
when that namespace is first registered, and nothing re-reads it — so a
deregistration governed only the namespaces registered *after* it while a
running job kept its copy.  **That is closed.**  ``PMIx_Put`` gained
delete scopes, ``gds`` gained a ``del_key`` slot, a deregistration takes
the key back from every namespace that already holds it, and the server
tells its local clients so their cached copies go too.

Two things about how it closed are worth keeping, because both contradict
what this entry predicted.

``gds/shmem3`` did **not** need a new segment generation.  The obstacle
was always that a client reads the shared segment directly, so removing
data from one would mean putting a lock on a read path that is lock-free
by design — and the assumed answer was to publish a new generation
carrying a ``PMIX_UNDEF`` tombstone, which there is no way to advertise
outside a fence reply.  The record does not have to be *in* the segment.
Each process keeps its own list on ``job->tombstones``, built from the
notification its server sends, and every read consults it;
``pack_tombstones()`` adds the list to the cached job-info reply so a
client attaching later gets it too.

And the deletion had to be made to cross a node boundary, which is a
separate problem from the one this entry describes.  ``deregister_resources``
does not need it — the host calls that API on every daemon — but a
``PMIx_Put`` deletion happens in one process, and the modex is additive,
so a contribution that merely stops naming a key removes nothing at the
far end.  A removal is therefore *stated* in the next collecting fence, as
an entry whose value is ``PMIX_UNDEF``, and both datastores act on one
that arrives.  ``examples/delete_key.c`` is the case that proves it, and
it only proves anything with one rank per node.

What remains is the *qualified* form, and it is deliberate rather than
missing.  A deregistration that prunes elements out of an entry instead of
removing it is not propagated: the host asked for part of a value to go,
so the right answer is the pruned value rather than a deletion, and that
needs an update push which does not exist.  Building one means deciding
what a partial update looks like on the wire and in a shared segment
neither datastore can rewrite in place.

See :ref:`Delta Exchange and Data Deletion <modex-delta>` in
:doc:`how-things-work/modex`, tracked as `openpmix#4087
<https://github.com/openpmix/openpmix/issues/4087>`_.

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
* **Two fixes found in ``src/tool`` ship without regression tests**
  because the conditions cannot be arranged in ``make check``: the
  SIGCONT handler for forwarded stdin needs a tty, and the
  late-finalize-reply guard needs a server that answers the finalize
  handshake more than five seconds late.  Only the second is still in
  ``src/tool``; the stdin read event and its SIGCONT handler moved to
  ``src/common/pmix_iof.c`` when the tool was given the library's
  forwarding instead of its own, and are untested there for the same
  reason.
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

* **An event-caching block that assigns ``cd->nondefault`` inside its**
  ``0 < ninfo`` **guard is not dropping a flag.**  It reads as though a
  non-default event carrying no info would be parked as a default one
  and then delivered to default handlers that should not see it.  It
  cannot happen: every path that sets ``chain->nondefault`` reads it out
  of a ``PMIX_EVENT_NON_DEFAULT`` directive in the info array, so the
  flag is only ever true when there is info to have carried it.  Moving
  the assignment out of the guard is equivalent, not a fix — two of the
  three caching blocks now assign unconditionally only because the
  guarded form kept being re-reported.  The third, in
  ``pmix_notify_server_of_event``, still assigns inside the guard and is
  equally correct.
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
