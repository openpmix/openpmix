Known gaps and deferred work
============================

**Everything on this page is open.**  It inventories the issues found
during the deep reviews of the PMIx source tree that were **deliberately
not fixed at the time they were found**, the code that is believed
correct but that no test reaches, and the directories no review has read.
Each entry records what the problem is, why it was left alone, and what
closing it would take.

Nothing here is closed, and no entry is a retrospective.  Closed entries,
the dated log of what each review pass found and retired, the list of
things that look like bugs but are *by design*, and the things that are
real and **will not be done** all live in :doc:`review-notes`.  They are kept, because they stop the same ground
being re-covered; they are kept **there**, so that this page reads as a
work list.

Four kinds of entry appear here:

* **Open decisions** — a real defect whose fix requires a judgement
  that the review was not entitled to make on its own (a change to a
  published attribute, a released API's behavior, or the PMIx
  Standard).
* **Deferred work** — a real defect whose fix is larger, riskier, or
  more entangled than the change it was found alongside.
* **Coverage gaps** — code that is believed correct but that no test
  reaches, usually because reaching it needs fault injection or
  hardware the test environments do not have.
* **Review coverage** — the directories the deep review has not yet
  read, and those whose code has moved far enough since their review
  that the coverage no longer holds.

.. note:: Each directory's ``AGENTS.md`` carries the full reasoning for
          the items found in it.  Those files are orientation maps, not
          work logs, so this page is the place that records an item as
          still **open**.

          **Re-verify an entry before acting on it: an entry here is a
          lead, not a finding.**  Entries have been retired by a fresh
          read of the tree more than once — the code moves, and an entry
          written from a plausible reading of it rather than from
          watching it run is exactly the kind that goes stale.  Every
          entry below was last checked against the tree on
          **2026-08-28**.

At a glance
-----------

**Open decisions — 1**

* :ref:`todo-fabric-async`

**Deferred work — 2**

* :ref:`todo-get-pointer-values`
* :ref:`todo-fabric-inventory`

**Coverage gaps — 21.**  No CI race detector; the switchyard's
out-of-memory and finalize-race arms; a multi-namespace
``PMIx_Disconnect``; a spawn hosted by ``simptest``; the
``PMIx_Compute_distances`` reply path; two ``src/tool`` fixes (SIGCONT
stdin, late finalize reply); the ``PMIx_Init`` debugger-wait teardown;
leak validation of the process-set and resolve examples; ``pps`` against
a live process table; the compressed half of ``preg``; ``psensor/file``
drop counts; a ``pcompress`` module that fails to start;
``psec/munge``'s failed encode; ``plog/smtp`` (never
run at all); the ``pnet`` fabric calls; ``pnet/simptest``'s end-to-end
launch; the TSD finalize ordering; ``gds/shmem3`` on macOS; the
client-side tombstone generation; and three ``src/hwloc`` findings.
Each is listed in full under `Coverage gaps`_.

**Connections with earlier releases — 9.**  Found in the review of
connection behavior that preceded the removal of tools on other hosts.
Each is listed in full under `Connections with earlier releases`_.

**Review coverage — 3 directories unread**, plus ``src/common`` and four
lower-priority directories whose code has moved since their review.
Nothing outside ``src/`` has been reviewed at all.

Connections with earlier releases
---------------------------------

Found in a review of how servers, clients and tools of different releases
connect (2026-10-24), and checked against the tree on 2026-10-25.  Every
entry is a behavior a site running a mix of releases can meet.  Each says
what happens, when, and what would close it.

.. _todo-conn-client-owner:

A client refuses an older server run by another user
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work.*  Over TCP, a client checks who owns its server's end of
the connection (``pmix_ptl_base_check_server`` in
``src/mca/ptl/base/ptl_base_fns.c``).  A server of this release gives the
client its uid, and the kernel's answer must match it.  An older server
gives nothing, so the owner must be the client's own user, root, or a
user listed in ``ptl_base_server_access``.

That refuses a client whose host runs as a service account - a resource
manager daemon with its own user - and one in a rootless container, where
the kernel names the server's owner as the overflow uid (65534).  Both
worked with every earlier release.

To close: a client is named for its server by that server's own
environment, as ``check_unnamed`` already accepts where there is no
table.  Skip the owner check for a client of an older server, and keep it
for tools.

.. _todo-conn-unaccepted-root:

A connection not yet accepted is reported as root's
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work.*  On Linux kernels through 6.x, a TCP connection still in
the server's accept queue has no socket of its own yet, and the kernel
gives its owner as uid 0.  ``sock_diag`` reports that with an inode of 0;
``/proc/net/tcp`` does the same in its inode column.  A tool that looks
up its server's owner before the server has called ``accept()`` therefore
sees root.  Against a server of this release, root does not match the uid
the server gave, and the tool is refused.  Kernel 7.0 reports the real
owner.

To close: treat an inode of 0 as "not yet known"
(``PMIX_ERR_NOT_FOUND``) in ``src/util/pmix_getid.c``, so the existing
retry asks again.

.. _todo-conn-retry-window:

A busy server can outlast the tool's retry
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work.*  When the kernel does not list the connection yet, the
tool retries the lookup for about one second and then gives up.  A server
with a full accept queue - many tools starting at once - can take longer
than that, and the tool fails although nothing is wrong.

To close: tie the retry to the handshake's own deadline, or make it an
MCA parameter.

.. _todo-conn-bare-uri-wire:

A bare TCP URI speaks a wire format older servers lack
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work.*  A tool given only a TCP URI, with no file, knows
nothing of its server's release.  ``pmix_ptl_base_select_compat`` then
picks no wire format, and the tool uses its newest, ``v70``.  A server of
v6.1 or earlier has no ``v70`` and refuses the connection.

To close: with no version to go by, use the newest wire format every
supported server has, so that a server of a later release still accepts
it.

.. _todo-conn-table-unreadable:

A table that cannot be read counts as "not listed"
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work.*  On Linux, ``sock_diag`` answers ``ENOENT`` both for a
connection it does not have and when the kernel has no TCP handler
loaded.  ``pmix_util_getid_tcp`` then reads ``/proc/net``.  If that
cannot be read either - ``hidepid``, or a restricted ``/proc`` in a
container - the answer stays ``PMIX_ERR_NOT_FOUND``.  A server refuses
such a peer as unowned, and a tool retries and fails, where the right
answer is ``PMIX_ERR_NOT_SUPPORTED``: no table to ask.  With legacy peers
on, that would take the peer as the platforms with no table do.

To close: return ``PMIX_ERR_NOT_SUPPORTED`` when ``sock_diag`` said
``ENOENT`` and ``/proc/net`` could not be read.

.. _todo-conn-launcher-rndz:

``PMIX_LAUNCHER_RNDZ_URI`` vouches for nothing
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work.*  On a platform with no table of TCP connections, a tool
connects to a TCP server only when a file its caller named vouches for
it.  A launcher's rendezvous URI is given in ``PMIX_LAUNCHER_RNDZ_URI``
instead, so a tool given one is refused there.

To close: have the launcher's rendezvous name a file the tool can check,
or treat that variable as the caller naming the server.

.. _todo-conn-docs-old-clients:

The connections page misdescribes v2.1 and v3.2 clients
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Deferred work (docs).*  ``docs/how-things-work/connections.rst`` says a
client of an earlier release, given the local socket's URI, initializes
as a singleton and returns ``PMIX_ERR_UNREACH``.  A v2.1 or v3.2 client
does not come up as a singleton: its ``PMIx_Init`` fails.

To close: give each release's behavior separately.

.. _todo-conn-ompi5-singletons:

An Open MPI 5 job runs as singletons under a server without legacy peers
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Open decision.*  When the server does not take legacy peers, an Open MPI
5 process (a v5.0 client) is given only the local socket's URI, which it
cannot use.  It comes up as a singleton, so an N-process job runs as N
separate one-process jobs, and the only sign is an error line from each.

The decision is what the server should do.  It could leave the client's
variables unset so that the client fails outright, or set something every
earlier release reads as "no server".  Either changes what an earlier
release sees, so it needs to be checked against each release from v2.1.

.. _todo-conn-empty-nspace:

A v5.0 scheduler with an empty namespace is untested
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Coverage gap.*  A tool that connects with an empty namespace is refused
("names no process", ``src/mca/ptl/base/ptl_base_connection_hdlr.c``).
Read from the v5.0 source, a v5.0 scheduler may connect that way.  No test
connects one, so whether it is refused is unconfirmed.

To close: add a case to the cross-version tests with a v5.0 scheduler
connecting to a server of this release.

Open decisions
--------------

.. _todo-fabric-async:

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

Deferred work
-------------

.. _todo-get-pointer-values:

``PMIX_GET_POINTER_VALUES`` is honored by three shortcuts and nothing else
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found reviewing ``src/client/pmix_client_get.c`` (2026-08-22).

The attribute asks that "any pointers in the returned value point
directly to values in the key-value store".  ``process_request()`` obeys
it for the two requests it answers out of ``pmix_globals`` —
``PMIX_PROCID`` returns ``&pmix_globals.myidval`` and ``PMIX_RANK``
returns ``&pmix_globals.myrankval`` — and, inconsistently, not for
``PMIX_VERSION_NUMERIC``, which allocates.  Every other path, local hit
or server round trip, hands back an allocated copy the caller must
release.

Closing it is not a fix but a change to the ownership contract
``PMIx_Get(3)`` states, and one that has to answer a question the
shortcuts do not raise: a pointer into the datastore is only safe while
the datastore holds it, and ``gds/hash`` rewrites its tables on the
progress thread.  The two shortcuts are safe because they point at
process-lifetime globals.  Left as recorded behavior; the smaller,
separable piece is making ``PMIX_VERSION_NUMERIC`` agree with its two
neighbours.

.. _todo-fabric-inventory:

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

Coverage gaps
-------------

* **Nothing in CI can detect a data race.**  The sanitizer job builds
  with ``-fsanitize=address``; ASan finds no races, and there is no
  ThreadSanitizer build anywhere.  This became visible closing the
  ``PMIx_server_init`` ordering question (2026-08-23): the library's
  central threading invariant — that ``pmix_globals``, the ``gds``
  tracker lists and ``pmix_server_globals`` are touched only from the
  progress thread — is held up by reading code, and every claim that a
  particular path honors it is argued rather than measured.  ``gds/hash``
  takes no lock at all, so a violation is heap corruption rather than a
  stale read, and the interleavings that would expose one are measured in
  microseconds and need two processes.  A TSan configuration exercised by
  the tool and server tests is the missing instrument; until there is
  one, treat "this runs on the progress thread" as an assertion to be
  re-checked by hand whenever the code around it moves.

* **The out-of-memory and finalize-race arms of the server switchyard's
  host callbacks.**  ``op_cbfunc``, ``op_cbfunc2`` and ``resop_cbfunc``
  in ``src/server/pmix_server_switchyard.c`` each have two arms that do
  not thread-shift.  Both were fixed to release the caddy they own, but
  neither is reachable from a unit test without fault injection — a
  failed allocation, or the progress thread stopping while a host
  completion is in flight.  The same gap now covers the 32 dispatch arms
  that answer ``PMIX_ERR_NOMEM`` when ``PMIX_GDS_CADDY`` cannot allocate,
  and the ``PMIX_ERR_NOMEM`` arm of ``PMIX_SERVER_QUEUE_REPLY``
  (2026-08-24): both replaced a NULL dereference that killed the server,
  and both need an allocator hook to reach.  An allocation-failure
  injection facility — a debug-build counter that makes the *n*\ th
  ``pmix_malloc`` fail — would close a good part of this directory's
  untestable set at once; it is the single highest-value piece of test
  infrastructure the review has wanted.
* **A multi-namespace** ``PMIx_Disconnect`` **is not reached by** ``make
  check``.  ``test/test_cd.c`` — what the ``--test-connect`` runs drive
  — disconnects a process from its *own* namespace, so the loop in
  ``disconnect_cbfunc()`` that drops the other participants' cached data
  never executes.  Reaching it needs live processes in two namespaces,
  and therefore a real launcher, for the reason in the next entry.  The
  path is verified today by running ``examples/dynamic.c`` under a PRRTE
  DVM with ``PMIX_MCA_gds_base_verbose=1`` forwarded to the clients, so
  the ``GDS DEL NSPACE`` lines can be seen coming from a client rather
  than only from the server's deregistration.  Closing this means a test
  that stands up a server and two client namespaces in-process, in the
  white-box style of ``test/unit/server_fence.c``.
* ``test/simple/simptest`` **cannot host a spawn.**  Its ``spawn_fn``
  calls ``PMIx_server_setup_application()``, a thread-shifting API, and
  then waits on the result — but ``spawn_fn`` is the host callback and
  runs *on* the server's progress thread, so the fake launch deadlocks.
  Spawn coverage therefore lives in the multi-node suite rather than in
  ``make check``.  Do not add a ``run_*.pl`` that spawns until that path
  is made asynchronous.
* **The reply handling of the** ``PMIx_Compute_distances`` **pair is not
  covered anywhere,** and cannot be: two of the three paths are
  allocation failures and the third needs a server reply whose declared
  count exceeds its payload.  Reaching the receive path at all requires
  a client whose *local* hwloc computation failed.
* **Two fixes found in** ``src/tool`` **ship without regression tests**
  because the conditions cannot be arranged in ``make check``: the
  SIGCONT handler for forwarded stdin needs a tty, and the
  late-finalize-reply guard needs a server that answers the finalize
  handshake more than five seconds late.  Only the second is still in
  ``src/tool``; the stdin read event and its SIGCONT handler moved to
  ``src/common/pmix_iof.c`` when the tool was given the library's
  forwarding instead of its own, and are untested there for the same
  reason.
* **The debugger-wait teardown in** ``PMIx_Init`` **is not exercised.**
  Reaching it needs the debugger-stop key set on the namespace *and* the
  ``PMIX_READY_FOR_DEBUG`` notification to then fail, which no test
  environment can arrange without fault injection.
* **The process-set and resolve examples have never been leak-validated.**
  Both hang in the test environments used so far, so valgrind is killed
  before ``PMIx_Finalize`` runs and the report is inconclusive.
* ``pps`` **has never been validated against a live process-table
  server.**  Its no-connect paths are covered by the tools smoke test;
  the proc-table rendering is not.
* **A** ``pcompress`` **module that fails to start has nothing to fail
  it with.**  ``pmix_compress_base_select()`` now degrades to the base's
  no-op stubs when the winning module's ``init()`` returns an error,
  rather than failing ``PMIx_Init``.  No component implements ``init``
  — all five leave the slot ``NULL`` — so reaching that branch needs a
  component written to fail on purpose.  The behavior it replaced was
  equally unreachable; the branch is written for the first module that
  does implement ``init``.
* **The compressed half of** ``preg`` **is invisible to a build with no
  compression library.**  ``preg/compress`` disables itself when
  ``pcompress`` has no module, so on such a build — a stock macOS
  developer tree among them — ``raw`` wins every encode, and neither the
  ``blob:`` framing in ``preg_base_legacy.c`` nor the ``compress``
  component is ever reached by a round trip.  ``test/unit/preg`` covers
  the framing with hand-built vectors, which is what a bounded decoder
  can be tested with, but the encode-decode pair only meet on a build
  that can compress: ``test_legacy_large`` says which encoding it got,
  so a thin run reports itself rather than passing quietly.  The real
  round trip was run under ``contrib/dockerswarm`` (Linux, all four
  compressors), where the 5000-node case does take the blob path.
* **The** ``psensor/file`` **drop-count and baseline semantics have no
  automated check.**  The fix stops a monitor from alerting before it
  has ever recorded a miss — a request with no
  ``PMIX_MONITOR_FILE_DROPS`` used to trip on the first sample of a
  perfectly healthy file. Distinguishing the fixed behavior from the
  broken one takes a monitor with a *zero* drop allowance watching a
  file that is being kept fresh, and zero tolerance means any scheduling
  hiccup or coarse filesystem timestamp granularity fails it.  The
  heartbeat drop allowance is covered instead (``run_monitor.pl``'s
  capped monitor), because a request can be given an allowance no run
  can spend; there is no equivalent for "must not alert on the first
  look".
* ``psec/munge``'s **failed-encode path is still not executed.**  The
  rest of the component now is: ``test/unit/psec_credentials.c`` drives
  every *active* credential module rather than a fixed list, so on a
  host with libmunge and a live ``munged`` it examines ``munge`` on the
  same terms as ``native``.  The PRRTE tree's ``contrib/slurmswarm``
  image is such a host — it installs ``libmunge-dev`` before it builds
  PMIx, so the component is compiled, and its entrypoint starts
  ``munged`` — and the suite passes 62/62 there, valgrind-clean, with
  the ``info[n]`` defect confirmed to segfault it when reinstated.  What
  no test reaches is the branch where ``munge_encode`` *fails* midway
  through a refresh, which is where the dangling ``mycred`` lived;
  provoking it means making ``munged`` fail on demand between two
  ``PMIx_Get_credential`` calls.
* **The plog** ``smtp`` **component has never been run.**  It builds
  only where libesmtp is present, and exercising it needs a reachable
  SMTP server on top of that, so the fixes from the ``src/mca/plog``
  review (2026-08-20) — an uninitialized ``message_status_t``, a
  ``crnl()`` that emitted LF where it meant CR, a message callback that
  ended the message before the body when no prefix was configured — were
  verified by reading and compile-checked with ``--enable-test-build``
  against the shim header, not by sending mail.  The shim makes every
  libesmtp call a stub, so a test-build tree cannot exercise them
  either.  The first real user of this component should expect to find
  more.
* **Nothing exercises the pnet fabric calls.**  ``register_fabric``,
  ``update_fabric`` and ``deregister_fabric`` are wired all the way
  through ``src/mca/pnet/base``, but no shipped component implements any
  of them, so ``pmix_pnet_globals.fabrics`` is never populated in a stock
  build and every base fabric call ends in ``PMIX_ERR_NOT_SUPPORTED`` /
  ``PMIX_ERR_NOT_FOUND`` / ``PMIX_ERR_BAD_PARAM``.  Covering it means
  writing a component that claims a plane, which is the decision recorded
  above.  (``test/unit/server_fabric`` covers the server-side
  ``PMIx_Compute_distances`` handler, not this path.)
* ``pnet/simptest``'s **end-to-end launch path is not in** ``make
  check``. ``test/unit/pnet_simptest_map`` drives ``allocate`` through
  ``PMIx_server_setup_application`` against a topology file it writes,
  but the other half — a client fetching ``PMIX_FABRIC_ENDPT`` by rank
  and ``PMIX_FABRIC_COORDINATES`` at the node level — still has to be
  run by hand, because ``test/simple/simptest`` generates its node map
  from the local host and so needs a topology file naming that host.
  See "Running it" in ``src/mca/pnet/simptest/AGENTS.md``.

* **The TSD finalize-ordering fix has no automated test.**
  ``pmix_tsd_keys_destruct`` deletes the process's pthread keys, so it
  may only run once every other thread is joined; it used to run six
  lines above the ``pmix_progress_thread_stop`` in ``pmix_rte_finalize``
  that joins the progress thread, and has been moved below it.  Neither
  half of what that fixed is testable from a unit program: reaching a
  deleted key needs the progress thread to print a process name inside a
  window a few calls wide, and the key-slot leak needs the same window
  and then shows up only as a ``PTHREAD_KEYS_MAX`` exhaustion many
  init/finalize cycles later.  ``test/unit/threads_primitives.c`` covers
  the registry's own contract — including that a destructor is *not* run
  for a key the finalizing thread never set a value for — but the
  ordering itself is held only by the comment at the call site and by
  ``src/threads/AGENTS.md``.

* ``gds/shmem3`` **gets no coverage at all on macOS, of any kind.**  Its
  ``configure.m4`` gates on a 64-bit non-Apple host with no ``|| test
  "$pmix_testbuild" = "1"`` escape, so a Mac does not even *compile* it
  — ``--enable-test-build`` does not help, and a change made there has
  not been compile-checked until it has been built on Linux.
  ``test/unit/gds_datastore``'s ``test_shmem3_job_segment()`` asks
  ``pmix_gds_base_assign_module()`` for the component by name and prints
  ``SKIP`` where it is absent, so the case is honest rather than
  missing, and on Linux it drives the segment build end to end in one
  process. Everything beyond one process — a fence, a second modex
  generation, a client attaching at a fixed address, and every
  cross-node fetch — is reachable only through
  ``contrib/dockerswarm/run-gds-tests.sh``.
* **The client-side tombstone generation fix is not what**
  ``examples/delete_key.c`` **discriminates on.**  The example now
  re-publishes a deleted key and requires every rank to read the new
  value back, which pins the documented behavior — a deletion is not
  permanent — but it passes both before and after the fix: on a client
  the keyed get misses locally and is answered by the server, which never
  had the bug.  Catching the client-side half needs a NULL-key read that
  actually reaches the modex, which takes the local table to miss for the
  rank or an explicit ``PMIX_REMOTE`` scope, and nothing arranges that
  today.

* **Three findings from the** ``src/hwloc`` **review have no test.**
  Their reasoning is in ``src/hwloc/AGENTS.md`` items 21, 22 and 29; what
  is open is the coverage, not the fix.

  * A process that adopted its topology from shared memory and was told
    to share it must publish the XML, since
    ``hwloc_shmem_topology_write()`` takes SIGBUS on an adopted topology.
    Reaching that needs a second-level server between a daemon and its
    clients, so it belongs in ``contrib/dockerswarm/run-topology.sh``,
    which has no such shape today.
  * A ``NULL`` under the deprecated ``PMIX_TOPOLOGY`` key needs the info
    array ``PMIx_server_init`` was called with, and topology acquisition
    runs once per process — so it needs a test binary of its own, the way
    ``test/unit/hwloc_setup_fail.c`` did.  That is the cheapest of the
    three to close.
  * The shmem address and size were ``uint64_t`` where one becomes a
    ``void *``.  Only a build where a pointer is narrower than 64 bits
    shows it, and none of the test environments is one.

Review coverage
---------------

What the deep review has *not* reached.  Move an entry out of "Not yet
reviewed" as its review lands, and refresh the churn figures in
"Reviewed, but changed materially since" when a re-review closes one.
The directories that are reviewed and current are listed in
:doc:`review-notes`.

.. note:: **An** ``AGENTS.md`` **is not evidence of a review.**  Every
          directory under ``src/`` has one; most were written in the July
          2026 orientation sweep, before any review started.  What marks
          a reviewed directory is a run of repair commits — "Repair…",
          "Screen…", "Close the … holes", "Sweep the leftovers in…" —
          and a commit that records the result in the guide.

Not yet reviewed
^^^^^^^^^^^^^^^^

Each of these has an orientation guide and nothing else: no findings were
ever recorded in it, and only drive-by fixes have landed.  Ordered by
size, which is a rough proxy for how much there is to find.

* ``src/mca/pdl``, ``src/mca/pinstalldirs`` — about 2200 lines between
  them, and the lowest risk of the group.

Outside ``src/``, nothing has been reviewed: ``examples/`` (16678 lines,
leak-swept only), ``test/simple`` (11011), ``test/unit/util`` and
``test/unit/mca``, and the public headers in ``include/``.

Reviewed, but changed materially since
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Ordered by how much of the directory the review no longer covers.
Figures are against the commit that recorded each review, measured
2026-08-24.  ``src/util`` stood at the head of this list until
2026-08-26 and ``src/hwloc`` until 2026-08-27; the per-file re-reviews
closed both.

#. ``src/common`` — reviewed 2026-08-02.  The two files that had moved
   furthest since, ``pmix_iof.c`` (1215 changed lines: the tool being
   given the library's stdin forwarding instead of its own, the hold on a
   spawned job's early output, and the SIGCONT handler that moved here
   out of ``src/tool``) and ``pmix_pfexec.c`` (335, most recently the
   removal of the library's signal traps and the per-holder reference on
   a pfexec child), were **re-reviewed on 2026-08-27**.  The other
   fifteen files are close to what the 2026-08-02 review read.

Lower priority, with current guides and churn that is mostly the reviews'
own work coming back through: ``src/event`` (+414/-29 since 2026-08-01,
nearly all of it the notification fan-out and cached-event replay that
the ``src/server`` and event-forwarding reviews drove), ``src/mca/ptl``
(+617/-365 since 2026-08-07, the connection handler plus deletions
elsewhere), ``src/mca/gds/hash`` (+495/-152 since 2026-08-03, chiefly the
single job-level store the ``PMIx_Get`` review forced), and
``src/mca/base`` (+17/-2 since 2026-08-16).
