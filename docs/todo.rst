Known gaps and deferred work
============================

**Everything on this page is open.**  It inventories the issues found
during the deep reviews of the PMIx source tree that were **deliberately
not fixed at the time they were found**, the code that is believed
correct but that no test reaches, and the directories no review has read.
Each entry records what the problem is, why it was left alone, and what
closing it would take.

Nothing here is closed, and no entry is a retrospective.  Closed entries,
the dated log of what each review pass found and retired, and the list of
things that look like bugs but are *by design* all live in
:doc:`review-notes`.  They are kept, because they stop the same ground
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

**Open decisions — 3**

* :ref:`todo-fabric-async`
* :ref:`todo-mca-param-owner`
* :ref:`todo-iof-pull-handle`

**Deferred work — 13**

* :ref:`todo-resolve-peers-wildcard`
* :ref:`todo-get-pointer-values`
* :ref:`todo-mypeer-second-ref`
* :ref:`todo-pfexec-iof-directives`
* :ref:`todo-tool-delete-notice`
* :ref:`todo-global-syslog`
* :ref:`todo-compress-length-prefix`
* :ref:`todo-pcompress-init-fatal`
* :ref:`todo-pmdl-app-values`
* :ref:`todo-fabric-inventory`
* :ref:`todo-legacy-regex-length`
* :ref:`todo-debug-threads`
* :ref:`todo-server-genvars`

**Coverage gaps — 20.**  No CI race detector; the switchyard's
out-of-memory and finalize-race arms; a multi-namespace
``PMIx_Disconnect``; a spawn hosted by ``simptest``; the
``PMIx_Compute_distances`` reply path; two ``src/tool`` fixes (SIGCONT
stdin, late finalize reply); the ``PMIx_Init`` debugger-wait teardown;
leak validation of the process-set and resolve examples; ``pps``
against a live process table; the compressed half of ``preg``;
``psensor/file`` drop counts; a blocking ``psec`` handshake; ``psec/munge``'s
failed encode; ``plog/smtp`` (never run at all); the ``pnet`` fabric
calls; ``pnet/simptest``'s end-to-end launch; the TSD finalize ordering;
``gds/shmem3`` on macOS; the client-side tombstone generation; and three
``src/hwloc`` findings.  Each is listed in full under `Coverage gaps`_.

**Review coverage — 3 directories unread**, plus ``src/common`` and four
lower-priority directories whose code has moved since their review.
Nothing outside ``src/`` has been reviewed at all.

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

.. _todo-mca-param-owner:

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

.. _todo-iof-pull-handle:

A blocking ``PMIx_IOF_pull`` hands back no handle
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found re-reviewing ``src/common/pmix_iof.c`` (2026-08-27).

``PMIx_IOF_pull``'s registration id reaches the caller through
``regcbfunc``, and passing a NULL ``regcbfunc`` is what selects the
*blocking* form.  So a caller who registers synchronously is never told
the id, and ``PMIx_IOF_deregister``, whose first parameter is that id,
can never be called for it.  The registration lives in
``pmix_globals.iof_requests`` for the life of the process.

Closing it means either adding an ``OUT`` parameter to a released API —
which the backward-compatibility rules forbid outright — or defining an
attribute that carries the id back in the caller's ``directives`` array,
which is the mechanism the Standard prefers but which is a Standard
change, not a library one.  Recorded rather than repaired for that
reason.

Deferred work
-------------

.. _todo-resolve-peers-wildcard:

The legacy ``resolve_peers()`` branch never fetches at ``WILDCARD``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The pre-v3.2 branch of ``resolve_peers()`` no longer carries a dead
store — it assigned ``PMIX_RANK_WILDCARD`` and was then overwritten with
``PMIX_RANK_UNDEF`` before anything read it, and that assignment is
gone — but what it *meant* to do is still not done.  The branch now
varies only the ``key`` and ``ninfo``, which is what it always really
did, and the legacy path resolves because ``try_fetch()`` retries an
``UNDEF`` rank as ``WILDCARD``.  **Fetching at** ``WILDCARD``
**directly, as the branch intended, is still untried** — that is a
behavior change on a path only a pre-v3.2 server exercises, and there is
none to test against.

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

.. _todo-mypeer-second-ref:

The server and tool give back mypeer's second reference the long way
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found reviewing ``src/client/pmix_client.c`` (2026-08-22); what is left
belongs in ``src/server/pmix_server.c`` and ``src/tool/pmix_tool.c``.

``pmix_globals.mypeer`` carries a second reference on those two roles,
taken when ``pmix_client_globals.myserver`` is pointed at that same
object.  Both give it back by releasing ``pmix_globals.mypeer`` a second
time rather than by releasing ``myserver``, which is the pointer that
took it.  That works, but it costs the tool a ``myserver_is_mypeer``
flag captured before ``pmix_rte_finalize()`` purely to suppress a
release that would otherwise be a third one, and it left both roles with
``pmix_client_globals.myserver`` naming freed memory afterwards.  The
dangling pointers are fixed; the shape is not.

Releasing ``myserver`` *before* ``pmix_rte_finalize()`` — which is what
the client already does — would cover both cases with no flag and no
ordering subtlety: the object stays alive on the ``mypeer`` reference
throughout the runtime teardown, and ``rte_finalize`` then frees it.
Not done here because it reorders two finalize paths this review did not
cover, and confirming nothing in the ``rte_finalize`` chain reads
``pmix_client_globals.myserver`` wants more than a grep.

.. _todo-pfexec-iof-directives:

A locally fork/exec'd job gets none of its spawn's IOF directives
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found reviewing ``src/client/pmix_client_spawn.c`` (2026-08-22); the fix
belongs mostly in ``src/common/pmix_pfexec.c``.

``PMIx_Spawn_nb`` has three dispatch paths.  The server-role one and the
client/tool one both call ``pmix_server_spawn_parser()`` to read the
request's output directives into the caddy's ``channels``, ``flags`` and
``inherit_iof``; the server path then hands them to
``pmix_server_process_iof()`` and the client path copies ``flags`` onto
the namespace when the reply names it.  The fork/exec path a
disconnected launcher takes never calls the parser at all, and
``pfexec``
reads none of those fields.  The namespace object it creates keeps the
zeroed ``iof_flags`` its constructor gave it, and
``pmix_iof_write_output()`` formats from exactly that.

So ``PMIx_Spawn`` with ``PMIX_IOF_TAG_OUTPUT`` (or any other output
directive) produces tagged output from a connected launcher and
untagged output from a disconnected one.  ``PMIx_Spawn(3)`` says the
fork/exec fallback exists "allowing tools to maintain a single code
path for both the connected and disconnected cases", which is exactly
what this breaks.

Two pieces are needed and they are not the same size.  Parsing on that
path is one line, and belongs where the other two paths do it.  Acting
on the result is a ``pfexec`` question: it knows the namespace
immediately, so unlike the client path it could honor an
output-to-file directive rather than having to drop it — which makes
this a small feature rather than a transcription.  Left undone here
because adding the parse alone would produce a value nothing reads, and
because there is no in-tree way to exercise a disconnected launcher's
fork/exec output end to end.

.. _todo-tool-delete-notice:

A tool is sent key-deletion notices it has no receive posted for
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found reviewing ``src/client/pmix_client.c`` (2026-08-22); the fix
belongs in ``src/tool/pmix_tool.c``.

``pmix_server_notify_deleted()`` walks ``pmix_server_globals.clients``
and sends ``PMIX_PTL_TAG_DATA_DELETE`` to every peer there that is not
finalized and is not earlier than 7.0.0.  A tool that attached to the
server is in that array and reports its real version, so it is sent the
notice — but only ``PMIx_Init`` posts a receive for that tag.
``PMIx_tool_init`` does not, so the message reaches
``pmix_ptl_base_process_msg()`` with nothing waiting for it, is
discarded, and raises a ``PMIX_ERROR`` event on the way out.

Two things are wrong with that, and they want different fixes.  A tool
that has cached a key another process then deleted goes on answering
with the stale value, which is the whole reason the notice exists; that
argues for the tool posting the same receive and using the same handler.
Separately, a server should not be telling a peer something it cannot
have arranged to hear; the version test is the wrong screen for a role
that never posts the receive at all.

.. _todo-global-syslog:

Nothing forwards a global-syslog request to a gateway
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/plog`` review (2026-08-20).  Half of it was
fixed; the other half needs a design decision.

``PMIX_LOG_GLOBAL_SYSLOG`` means "record this in the system-wide
syslog", and only a gateway server is meant to emit it — the message is
supposed to travel to the gateway node and be written there.  The
``syslog`` module implements the gateway half: if it is a gateway it
writes locally, and if it is not it declines.  Nothing implements the
other half.  There is no transport that moves the request to a gateway,
so on a peer that is not one the entry has nowhere to go.

Before the review this was invisible, because the module returned
``PMIX_SUCCESS`` regardless and the caller was told the message had been
logged.  It now declines the entry, so the framework reports
``PMIX_ERR_NOT_AVAILABLE`` (or ``PMIX_ERR_PARTIAL_SUCCESS`` alongside
another channel that did work) and a *client* falls back to its own
modules — which will decline for the same reason.  The failure is
honest now, and still a failure.

Closing it means choosing between two designs and is not a bug fix:
either the request is relayed to the gateway over the existing
server-to-server path, which needs the routing to exist and raises the
question of what a client should be told while it is in flight, or
``PMIX_LOG_GLOBAL_SYSLOG`` is documented as gateway-only and the
attribute's description in ``include/pmix_common.h.in`` is corrected to
say so.  Note that ``pmix_log_host_only`` and the host's ``log2`` entry
point already give a resource manager a way to take the request and do
the forwarding itself, which may be the answer that needs no new PMIx
machinery at all.

.. _todo-compress-length-prefix:

A compressed blob's length prefix is taken on trust
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pcompress`` review (2026-08-20) and left alone
deliberately.

Every component allocates the uncompressed length the blob's 4-byte
prefix claims, *before* inflating anything.  The prefix generally came
off a peer's wire, so a five-byte blob whose prefix reads ``0xFFFFFFFE``
asks for a four-gigabyte allocation, which then fails or succeeds and is
thrown away when the payload turns out not to decode.  The review closed
the case that was a memory error — a blob too short to hold the prefix
at all — but not this one, which is a resource question rather than a
correctness one.

The obvious guard is a maximum expansion ratio, and that is exactly why
it was not written: DEFLATE tops out near 1032:1 while zstd's is far
higher, so any single cap either fails to constrain zstd or rejects
legitimate zlib output.  A per-component cap is possible; whether it is
worth the interoperability risk is a policy decision, not a bug fix.
Note also that the caller has already read the whole blob into memory by
the time it gets here, so the amplification is bounded by what the PTL
was willing to accept.

.. _todo-pcompress-init-fatal:

A pcompress module's ``init()`` failure is fatal to library init
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Also from the ``src/mca/pcompress`` review, and dead code today.

The framework's documented stance is that having no compressor is not an
error: ``pmix_compress_base_select()`` returns ``PMIX_SUCCESS`` when it
selects nothing, and the base default no-op stubs stay in place.  But if
the winning module's ``init()`` fails, that error is returned, and
``pmix_init.c`` treats a non-``SUCCESS`` return from the select as fatal
to ``PMIx_Init``.  So a compression library that loads but cannot start
would take the whole library down, where an absent one is shrugged off.

Unreachable: no module in the framework implements ``init``, and every
one of the five leaves the slot ``NULL``.  It is recorded because the
first module that does implement it will inherit the inconsistency, and
the fix — degrade to the base default rather than fail — should be made
then, with a module to test it against.

.. _todo-pmdl-app-values:

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

.. _todo-legacy-regex-length:

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

.. _todo-debug-threads:

``pmix_debug_threads`` cannot be turned on
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The four lock macros emit distinct debug strings under
``if (pmix_debug_threads)``, but nothing in the tree ever assigns that
variable — there is no MCA parameter for it and no other write outside
its definition in ``src/threads/thread.c``.  The facility is reachable
only by setting the variable from a debugger.  Registering an MCA
parameter for it would cost a few lines; it was left alone because
``src/threads`` is semantics-frozen and this is a feature rather than a
defect.  Until then, do not read a silent run as evidence the handshake
was not exercised.

.. _todo-server-genvars:

A server-wide envar hook that nothing fills
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found reviewing ``src/server/pmix_server.c`` (2026-08-23).
``pmix_server_globals.genvars`` is declared as "argv array of envars given
to me for passing to all clients", is statically initialized to NULL, and
is read in exactly one place — ``setup_fork_body()`` replays it into every
child's environment.  **Nothing in the tree ever writes it.**  There is no
directive, no API argument and no environment variable that reaches it, so
a host has no way to say "add these to every client I fork" and the replay
loop is dead code.

This is an unbuilt feature rather than a defect, which is why it is here
rather than fixed: closing it means choosing the interface.  The obvious
candidate is ``PMIx_server_register_resources`` — it already carries
host-supplied job-wide information into ``pmix_server_globals.gdata`` —
with the ``PMIX_SET_ENVAR`` / ``PMIX_ADD_ENVAR`` / ``PMIX_PREPEND_ENVAR``
family that ``src/common/pmix_pfexec.c`` already parses for a spawn.  That
also decides the harder half: whether a later registration replaces or
appends, and whether ``PMIx_server_deregister_resources`` has to take an
envar back out of children that have already been forked (it cannot).
The comment above ``setup_fork_body`` used to assert that the registration
path sets it; that has been corrected, and ``src/server/AGENTS.md`` says
the read site is not evidence of a writer.

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
* **A handshake-model psec module blocks the progress thread for as long
  as its peer takes to answer.**  ``PMIX_PSEC_SERVER_HANDSHAKE_IFNEED``
  runs inside the ``ptl`` connection handler, on the progress thread,
  with the socket deliberately still in blocking mode; a peer that
  connects and then stops writing pins that thread until the socket
  errors out.  This is intrinsic to the way ``ptl`` sequences the
  connection handshake rather than anything ``psec`` chooses, and today
  the only handshake-model module is ``dummy_handshake``, which is
  test-only.  It becomes a real availability question the moment a
  genuine one is written, and the fix belongs in ``ptl`` — a timeout on
  the handshake exchange, the way ``handshake_wait_time`` already bounds
  the connect-ack.  Recorded here so a new mechanism does not inherit it
  silently.
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
* ``src/util/keyval`` — 138 lines of flex source (``keyval_lex.l``);
  the 2327-line ``keyval_lex.c`` beside it is a generated build product
  and is not review material.  It has no ``AGENTS.md`` of its own, but
  the ``src/util`` re-review gave it a section in that directory's
  guide and covered its only driver, ``pmix_keyval_parse.c``, so what
  is left unread is the ``.l`` itself.

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
