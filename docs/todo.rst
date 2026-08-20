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

.. note:: **2026-08-18.**  The ``psensor`` progress-thread entry that
          stood under "Deferred work" is closed.  Both halves it called
          for were made together: ``pmix_psensor_base_open`` now starts
          the ``"PSENSOR"`` thread it creates, and
          ``pmix_psensor_base_close`` pauses that thread before anything
          is torn down and stops it only after the components -- which
          own the trackers, and therefore the timers armed on the base --
          have closed.  Both components' samplers were moved to
          ``EV_PERSIST`` timers they no longer re-arm, for the reason
          spelled out in ``src/mca/pstat/AGENTS.md``.
          ``test/unit/run_monitor.pl`` now runs its heartbeat *and* file
          scenario twice, once with
          ``psensor_base_use_separate_thread`` set, so the configuration
          that was silently dead is exercised by ``make check``.

          A second entry was opened, corrected and closed the same day,
          and the shape of the mistake is worth keeping.  The
          unexpected-message ``show_help`` that ``run_monitor.pl``
          printed on every run was first written down as a heartbeat
          arriving at a server with no posted recv -- plausible, since
          ``psensor/heartbeat`` really does post that recv lazily, and
          wrong.  Tracing the tag showed the opposite: the server *does*
          match the beat, on the wildcard recv the command switchyard
          serves, and answers it with the error the switchyard gets for
          trying to read a command out of a zero-byte buffer -- and it is
          the *client* that then cannot place the reply, because
          ``PMIx_Heartbeat`` is one-way and nobody posts for tag 1.  Both
          halves are now fixed: ``pmix_server_message_handler`` drops
          ``PMIX_PTL_TAG_HEARTBEAT`` instead of dispatching it, and an
          unmatched message is a framework trace rather than a
          ``show_help`` asking the user to report a bug.  The lesson is
          the one this page keeps relearning -- an entry written from a
          plausible reading of the code, rather than from watching it
          run, is a lead and not a finding.

Review coverage
---------------

Assessed on **2026-08-15** from the commit history, and refreshed on
**2026-08-19** when the ``src/mca/pnet``, ``src/mca/preg``,
``src/mca/pgpu`` and ``src/mca/pmdl`` reviews landed.  Move an entry out
of "Not yet reviewed" as its review lands, and refresh the churn figures
in "Reviewed, but changed materially since" when a re-review closes one.

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
``src/mca/pgpu``, ``src/mca/pmdl``, ``src/mca/pnet``, ``src/mca/preg``,
``src/mca/pstat``, ``src/mca/ptl``, and ``bindings/python``.
``src/client``, ``src/server``, ``src/hwloc``, ``src/util`` and
``src/mca/gds/shmem3`` were reviewed too, but have moved since — see
below.

Not yet reviewed
^^^^^^^^^^^^^^^^

Each of these has an orientation guide and nothing else: no findings were
ever recorded in it, and only drive-by fixes have landed.  Ordered by
size, which is a rough proxy for how much there is to find.

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

A fabric plane claimed asynchronously is not recorded
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pnet`` review (2026-08-19).
``pmix_pnet_base_register_fabric`` records a tracker for a plane only when
a module answers ``PMIX_OPERATION_SUCCEEDED`` — the "already done, inline"
status.  A module that claims the plane the *asynchronous* way, returning
plain ``PMIX_SUCCESS`` and calling back later, is never recorded, yet both
callers — ``src/server/pmix_server_fabric.c`` and
``PMIx_Fabric_register_nb`` — read ``PMIX_SUCCESS`` as "claimed, wait for
the callback".  A later update or deregister on that plane then answers
``PMIX_ERR_BAD_PARAM``, because the scan finds nothing.

Which half is wrong is the decision, and it cannot be settled by reading
the tree: **no shipped component implements** ``register_fabric`` **at
all** — grep the four component directories and the slots are unset — so
there is nothing to test a change against, and choosing the async
contract now pins down the interface a real fabric component would have
to meet.  Two loose ends belong to the same decision.  ``fabric->module``
is deliberately never set, so the branches in ``update_fabric`` and
``deregister_fabric`` that cast it back to a ``pmix_pnet_fabric_t *`` are
dead and every lookup goes through the index/name scan — a tracker
pointer parked in a caller's object would dangle the moment the fabric
was deregistered.  And nothing frees ``pmix_pnet_fabric_t.payload``:
``ftdes`` releases only ``name``, and the base never tells a component
its tracker died, so a component that parks an allocation there must free
it from its own ``deregister_fabric``.

See "The fabric path is scaffolding" in ``src/mca/pnet/AGENTS.md``.

Who owns an ``mca_base_*`` parameter
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pmdl`` review (2026-08-19).  ``pmdl`` re-prefixes
every value it reads out of an MCA param file so it reaches the library
that will look for it — ``PMIX_MCA_``, ``PRTE_MCA_`` or ``OMPI_MCA_`` —
and decides which by the parameter's first segment.  Two segments name
something in more than one library: ``mca`` (all three have an MCA base)
and ``pmix`` (OPAL carried a ``pmix`` framework of its own).

Half of that overlap is now settled, because one side of it was plainly
wrong: ``parse_file_envars`` was claiming those names out of the list
PMIx read from **its own** param files, so a value the user set for PMIx
— ``pmix_hwloc_topo_file`` is the concrete one — was renamed
``OMPI_MCA_*`` and reached neither library.  A parameter PMIx claims is
now left alone there.

The other half is left as it stands.  ``process_param_file`` reads
``openmpi-mca-params.conf``, tests for a PMIx parameter first, and so
forwards ``mca_base_component_path`` from **Open MPI's** file as
``PMIX_MCA_mca_base_component_path``.  Note which name that is: PMIx
registers the variable as project ``pmix``, framework ``mca``, component
``base``, and the *full* name a param file and an envar carry leaves the
project off — so ``mca_base_component_path`` is PMIx's own spelling of
it, and ``pmix_mca_base_component_path`` is the project-qualified long
name, which a param file also accepts (see ``var_set_from_file``, which
matches either).  Open MPI spells its equivalent the same way, which is
the whole difficulty: the unqualified name is genuinely ambiguous, and
nothing in the line says which library it was meant for.  Claiming it for
PMIx has been the behavior for releases and a site may be relying on it.
Deciding it means deciding whether the two ``mca_base`` namespaces are
one setting or two.

The ``ompi`` component's guide records the precedence as it stands.

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

An absent app-level value fails a child's launch
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pmdl`` review (2026-08-19), and narrowed rather
than closed.  ``pmdl/ompi``'s ``setup_fork`` treats a missing
``PMIX_PROCDIR``, ``PMIX_WDIR`` or ``PMIX_APP_ARGV`` as an error, and a
``setup_fork`` error fails ``PMIx_server_setup_fork`` and with it the
child's launch — so a host that registers a namespace without one of them
cannot start the job at all, however little the resulting envar matters
(``OMPI_MCA_initial_wdir`` is informational).

The review closed the case that was unambiguously wrong —
``PMIX_REINCARNATION``, which nothing in PMIx sets and no host is obliged
to provide, so its absence now means "zero restarts" rather than a failed
launch.  The three above are ordinary registration data that every host
in practice provides, so making them optional would be a behavior change
with nothing to test it against; it needs a decision about which of them
Open MPI genuinely requires.

Fabric inventory collection is a stub
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pnet`` review (2026-08-19).  No pnet component
collects inventory.  ``opa``'s ``collect_inventory`` carries a comment
about searching the topology for OPA NICs and then returns
``PMIX_SUCCESS`` having added nothing; ``deliver_inventory`` returns
``PMIX_SUCCESS``.  ``nvd``'s goes one step further — it confirms that a
matching NIC exists on the node — but still reports no contents.  The
plumbing is real, so a host that asks for fabric inventory gets a
successful, empty answer rather than an error; treat the capability as
unfinished rather than working.

Finishing it needs a decision the review was not entitled to make: what a
fabric inventory record contains, and how a component that has nothing to
report says so.  The base has **no decline convention** on this path —
unlike ``allocate`` and ``setup_fork``, it ``PMIX_ERROR_LOG``\ s any
non-``PMIX_SUCCESS`` return and abandons the fan-out to every component
behind it — so today "nothing here" and "collected everything" are the
same answer.

The deprecated regex API cannot carry a length
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/preg`` review (2026-08-19), and narrowed rather
than closed by it.  ``pmix_preg_base_legacy_decode`` bounds every read
against an ``avail`` argument, but only ``unpack`` can supply a real one:
it knows how many bytes are left in the buffer.  Every other caller holds
a bare ``char *`` and passes ``SIZE_MAX``, because
``pmix_preg.parse_nodes(regexp, &names)`` has nowhere to put a length
without changing a signature that predates the current API.  ``gds/hash``
has the length in ``val->data.bo.size`` and still cannot hand it over.

The review made the tag test exact, which removed the case that a host
could trigger with ordinary data — a plain node list whose first node
begins with ``blob`` is no longer taken for a blob and walked past its
end.  What is left needs a caller-owned string that really does carry the
``"blob:"`` tag and is truncated behind it, which is not something a peer
can produce (those arrive through the bounded ``unpack``).  Closing it
properly means plumbing a length through the deprecated signatures, and a
caller that can do that is better off moving to ``pmix_regex2_t``.

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
* **The compressed half of ``preg`` is invisible to a build with no
  compression library.**  ``preg/compress`` disables itself when
  ``pcompress`` has no module, so on such a build — a stock macOS
  developer tree among them — ``raw`` wins every encode, and neither the
  ``blob:`` framing in ``preg_base_legacy.c`` nor the ``compress``
  component is ever reached by a round trip.  ``test/unit/preg`` covers
  the framing with hand-built vectors, which is what a bounded decoder
  can be tested with, but the encode-decode pair only meet on a build
  that can compress: ``test_legacy_large`` says which encoding it got, so
  a thin run reports itself rather than passing quietly.  The real round
  trip was run under ``contrib/dockerswarm`` (Linux, all four
  compressors), where the 5000-node case does take the blob path.
* **Nothing exercises the pnet fabric calls.**  ``register_fabric``,
  ``update_fabric`` and ``deregister_fabric`` are wired all the way
  through ``src/mca/pnet/base``, but no shipped component implements any
  of them, so ``pmix_pnet_globals.fabrics`` is never populated in a stock
  build and every base fabric call ends in ``PMIX_ERR_NOT_SUPPORTED`` /
  ``PMIX_ERR_NOT_FOUND`` / ``PMIX_ERR_BAD_PARAM``.  Covering it means
  writing a component that claims a plane, which is the decision recorded
  above.  (``test/unit/server_fabric`` covers the server-side
  ``PMIx_Compute_distances`` handler, not this path.)
* **``pnet/simptest``'s end-to-end launch path is not in** ``make check``.
  ``test/unit/pnet_simptest_map`` drives ``allocate`` through
  ``PMIx_server_setup_application`` against a topology file it writes, but
  the other half — a client fetching ``PMIX_FABRIC_ENDPT`` by rank and
  ``PMIX_FABRIC_COORDINATES`` at the node level — still has to be run by
  hand, because ``test/simple/simptest`` generates its node map from the
  local host and so needs a topology file naming that host.  See
  "Running it" in ``src/mca/pnet/simptest/AGENTS.md``.

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
* **``pnet/opa`` and ``pnet/nvd`` ship no ``configure.m4``, on purpose.**
  A component with no ``configure.m4`` is configured by the MCA machinery
  itself and therefore builds unconditionally, which is what these two
  want: neither links anything nor needs an SDK — they read info
  attributes hwloc already recorded and set environment variables — and
  whether there is work to do is a property of the machine the *daemon*
  runs on, which only ``component_open`` is in a position to know.  The
  files they used to have asked that question at build time and got it
  wrong in both directions (``nvd``'s was hardwired off, ``opa``'s always
  succeeded).  The one visible consequence is that ``configure``'s summary
  no longer prints ``Transports / NVIDIA|OmniPath`` lines.  Do not restore
  them.
* **``transports_print`` in ``pnet/opa`` type-puns a ``uint64_t`` through
  ``unsigned int *``.**  That is undefined behavior in general and is not
  a live defect here: the whole tree is built ``-fno-strict-aliasing``
  (``config/pmix_setup_cc.m4`` adds it wherever the compiler takes it).
  The surrounding arithmetic is deliberately width-independent, and the
  byte-order dependence of the result does not matter because the key is
  generated once on the lead server and shipped as a string.  Do not copy
  the idiom into new code, and do not "fix" it as a bug.
* **A node in ``pnet/simptest``'s topology file that the job's node map
  does not name is silently skipped.**  ``allocate`` matches each config
  line against the ``PMIX_NODE_MAP`` by exact name, which is intrinsic to
  a file that describes the real nodes the RM placed the job on.  The
  converse is *not* silent: a node the job was placed on that the file
  fails to describe draws a ``node-not-found`` ``show_help`` naming it and
  the request is then declined — declined, rather than failed, because a
  hard error out of ``allocate`` aborts the base's fan-out for every other
  component too.
* **The bare ``atomic_bool`` fields in ``pmix_globals_t``** are correct,
  merely inconsistent with the typedefs used elsewhere, and the
  ``PMIX_C_HAVE_*`` defines in the installed ``pmix_config.h`` are now
  always ``1`` but are retained in case an out-of-tree consumer tests
  them.
