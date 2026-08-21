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

.. note:: **2026-08-20.**  The ``src/mca/psensor`` review found one
          shape repeated five times, and it is worth naming because it
          passes every test anyone would think to write: a directive
          parsed out of the request, stored in the tracker, and then
          never read again.  ``PMIX_MONITOR_HEARTBEAT_DROPS`` set
          ``ndrops`` and nothing consulted it, so a request asking to
          tolerate missed beats alerted on the very first empty window.
          ``PMIX_MONITOR_ID`` was never parsed at all, so the ``id``
          argument to ``stop()`` — the whole cancel handle — could never
          match anything.  ``PMIX_MONITOR_CANCEL`` was recognized by
          neither component, so it fell through to the resource-usage
          path, which answers ``PMIX_SUCCESS`` for an id it has never
          held: a client's cancel reported success and the monitor kept
          firing forever.  The ``error`` argument, which
          ``PMIx_Process_monitor(3)`` documents as "the code the monitor
          is to use", was discarded in favour of a hardcoded alert.  And
          the ``file`` sampler checked only the first of the three
          attributes a request named.

          Each of those looked like working code, and the framework's
          own end-to-end test passed throughout — because a test that
          waits for an alert cannot tell an honored directive from an
          ignored one.  ``test/unit/run_monitor.pl`` now arms a capped
          monitor and a cancelled one under a status code of their own
          and fails if *either* fires, which is the only shape that
          distinguishes them.  Each of the three fixes was verified by
          re-breaking it and watching the new check catch it.

          Two lifetime defects came out of the same pass.
          ``PMIx_Notify_event`` returns ``PMIX_ERR_NOT_AVAILABLE``
          without reaching its callback once the progress thread has
          stopped — which ``PMIx_server_finalize`` does well before it
          closes this framework — so both samplers stranded a tracker
          and the peer it retained.  And ``psensor/heartbeat`` never
          un-posted the PTL recv it posts lazily, nor cleared the flag
          saying it had: ``ptl`` closes after ``psensor``, so a DSO build
          left the ptl list naming a function in an unloaded plugin, and
          a second ``PMIx_server_init`` in one process would have found
          the flag set, the recv gone, and declared every monitored
          client dead on its first window.

Review coverage
---------------

Assessed on **2026-08-15** from the commit history, and refreshed on
**2026-08-20** when the ``src/mca/pnet``, ``src/mca/preg``,
``src/mca/pgpu``, ``src/mca/pmdl``, ``src/mca/pcompress``,
``src/mca/plog``, ``src/mca/psensor``, ``src/mca/psec`` and
``src/mca/pif`` reviews landed.
Move an entry out
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
``src/mca/pcompress``, ``src/mca/pgpu``, ``src/mca/plog``,
``src/mca/pif``, ``src/mca/pmdl``, ``src/mca/pnet``, ``src/mca/preg``,
``src/mca/psec``, ``src/mca/psensor``, ``src/mca/pstat``,
``src/mca/ptl``, ``src/threads``, and ``bindings/python``.
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
* ``src/mca/pdl``, ``src/mca/pinstalldirs`` — about 2200 lines between
  them, and the lowest risk of the group.

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

``pmix_net_samenetwork()`` only understands a /64 IPv6 prefix
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pif`` review (2026-08-20).  Its ``AF_INET6`` arm
compares the first eight bytes of the two addresses **only when the
prefix length it is handed is 64**, and returns false for every other
value.  That single fact is why the two IPv6 discovery components
disagree about what to store in ``if_mask``: ``bsdx_ipv6`` hard-codes 64
(so ``pmix_ifaddrtokindex()`` works on macOS/BSD), while ``linux_ipv6``
stores the true prefix from ``/proc`` — which for ``::1`` is 128, so on
Linux that entry can never match anything.

Both halves are individually defensible and neither can be changed on
its own: teaching ``bsdx_ipv6`` the real prefix would start failing every
interface that is not on a /64, and the honest fix is to give
``pmix_net_samenetwork()`` a general prefix comparison first and only
then make the two components agree.  Left alone because the only
consumer, ``pmix_ifaddrtokindex()``, has no in-tree callers — it is
exported for companion projects — so there is nothing here to measure a
behavior change against.

A kernel interface index does not fit ``if_kernel_index``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/pif`` review (2026-08-20).  ``pmix_pif_t``
declares ``if_kernel_index`` as a ``uint16_t`` and
``pmix_ifnametokindex()`` returns an ``int16_t``, but Linux hands out
32-bit interface indices that only increase — a long-lived host that
churns veth or container interfaces will eventually issue one that
truncates, and the ``int16_t`` return turns anything above 32767 into a
negative number that every caller reads as "not found".

Not fixed because both types are in the installed
``src/util/pmix_if.h``: the struct layout is ABI, and the return type is
a published signature.  Widening them is a deprecation exercise
(``pmix_ifnametokindex2()`` and a new field at the end of the struct),
not a repair.

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

* **``pmix_debug_threads`` cannot be turned on.**  The four lock macros
  emit distinct debug strings under ``if (pmix_debug_threads)``, but
  nothing in the tree ever assigns that variable — there is no MCA
  parameter for it and no other write outside its definition in
  ``src/threads/thread.c``.  The facility is reachable only by setting
  the variable from a debugger.  Registering an MCA parameter for it
  would cost a few lines; it was left alone because ``src/threads`` is
  semantics-frozen and this is a feature rather than a defect.  Until
  then, do not read a silent run as evidence the handshake was not
  exercised.

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
* **The ``psensor/file`` drop-count and baseline semantics have no
  automated check.**  The fix stops a monitor from alerting before it has
  ever recorded a miss — a request with no ``PMIX_MONITOR_FILE_DROPS``
  used to trip on the first sample of a perfectly healthy file.
  Distinguishing the fixed behavior from the broken one takes a monitor
  with a *zero* drop allowance watching a file that is being kept fresh,
  and zero tolerance means any scheduling hiccup or coarse filesystem
  timestamp granularity fails it.  The heartbeat drop allowance is
  covered instead (``run_monitor.pl``'s capped monitor), because a
  request can be given an allowance no run can spend; there is no
  equivalent for "must not alert on the first look".
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
* **``psec/munge``'s failed-encode path is still not executed.**  The
  rest of the component now is: ``test/unit/psec_credentials.c`` drives
  every *active* credential module rather than a fixed list, so on a host
  with libmunge and a live ``munged`` it examines ``munge`` on the same
  terms as ``native``.  The PRRTE tree's ``contrib/slurmswarm`` image is
  such a host — it installs ``libmunge-dev`` before it builds PMIx, so
  the component is compiled, and its entrypoint starts ``munged`` — and
  the suite passes 62/62 there, valgrind-clean, with the ``info[n]``
  defect confirmed to segfault it when reinstated.  What no test reaches
  is the branch where ``munge_encode`` *fails* midway through a refresh,
  which is where the dangling ``mycred`` lived; provoking it means making
  ``munged`` fail on demand between two ``PMIx_Get_credential`` calls.
* **The plog ``smtp`` component has never been run.**  It builds only
  where libesmtp is present, and exercising it needs a reachable SMTP
  server on top of that, so the fixes from the ``src/mca/plog`` review
  (2026-08-20) — an uninitialized ``message_status_t``, a ``crnl()``
  that emitted LF where it meant CR, a message callback that ended the
  message before the body when no prefix was configured — were verified
  by reading and compile-checked with ``--enable-test-build`` against
  the shim header, not by sending mail.  The shim makes every libesmtp
  call a stub, so a test-build tree cannot exercise them either.  The
  first real user of this component should expect to find more.
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

Not defects — by design
-----------------------

These look like bugs and are not.  They are recorded so that they are
not "fixed" by a later reader.

* **A ``pmix_lock_t`` whose ``status`` reads ``PMIX_ERR_INIT`` is
  reporting a missing assignment, not an init failure.**  Both
  ``PMIX_CONSTRUCT_LOCK`` and ``PMIX_LOCK_STATIC_INIT`` seed ``status``
  with ``PMIX_ERR_INIT``.  A waiter reads it only after a handler has
  woken it, so every handler on a path whose waiter reads ``status`` must
  assign it — and nothing enforces that, which makes the default the
  thing that decides how a violation presents.  ``PMIX_SUCCESS`` would
  turn a forgotten assignment into a confident wrong answer;
  ``PMIX_ERR_INIT`` makes it loud.  ``PMIX_CONSTRUCT_LOCK`` previously
  assigned nothing at all, and since the lock usually lives in a
  ``PMIX_NEW``'d caddy — ``malloc``, not ``calloc`` — the alternative was
  heap garbage.  If one of these turns up in a bug report, look at the
  handler that woke the lock and check its error paths before believing
  the status.

  The sweep behind this is worth not redoing.  Poisoning ``status`` and
  warning on it in ``PMIX_WAIT_THREAD`` fires ~86,000 times across
  ``make check``, which looks damning and is not: those callers read
  ``cb->status``, the *caddy's* own field, and never touch the lock's.
  Grepping ``lock.status`` and ``lock->status`` specifically narrows it
  to about nine sites in the library, every one woken through an
  ``opcbfunc`` that assigns ``status`` first.  The two spellings are a
  character apart and only instrumentation separates them.

* **``PMIX_ACQUIRE_THREAD`` / ``PMIX_RELEASE_THREAD`` having no callers
  in** ``libpmix`` **does not make them dead code.**  An in-tree grep
  finds only ``test/unit/threads_primitives.c``, and the live handshake
  everywhere in the library is ``WAIT``/``WAKEUP`` at about 120 sites
  each — which reads as a retirement candidate and is not one.  These
  headers are installed under ``$(pmixincludedir)/src/threads``, and
  PRRTE uses the pair: ``src/runtime/prte_locks.h`` includes
  ``src/threads/pmix_threads.h`` to declare ``prte_init_lock`` as a
  ``pmix_lock_t``, and ``prte_init()``, ``prte_finalize()`` and
  ``src/prted/pmix/pmix_server_notify.c`` guard ``prte_initialized`` with
  it across eleven call sites (checked against openpmix/prrte master,
  2026-08-20).  Removing them breaks PRRTE's build, and changing their
  semantics changes PRRTE's behavior.

  This is worth keeping precisely because the in-tree evidence points the
  wrong way, and because unused-looking API is where documentation rots:
  ``src/threads/AGENTS.md`` had described ``ACQUIRE_THREAD`` as *not*
  implying that it returns holding the mutex, when that pair is exactly a
  held-mutex critical section, so a reader who believed the guide and
  mixed the pairs would unlock a mutex they do not hold.  That is
  corrected, and the unit test now covers the pair.

* **The function-pointer cast in ``pmix_thread_start`` is deliberate.**
  It hands a ``void *(*)(pmix_object_t *)`` to ``pthread_create``, which
  wants a ``void *(*)(void *)`` — formally a call through an incompatible
  function pointer type.  GCC's ``-Wcast-function-type``, which is on
  through ``-Wextra -Werror``, treats any pointer parameter as matching
  any other and does not diagnose it, and every ABI PMIx supports passes
  a ``void *`` and a struct pointer identically.  Only clang's opt-in
  ``-Wcast-function-type-strict`` and UBSan's ``-fsanitize=function``
  object, and neither is used by the build or by CI — the sanitizer job
  in ``.github/workflows/builds.yaml`` is ASan only.  If one of those is
  ever turned on, the fix is a small trampoline that takes ``void *`` and
  calls ``t_run``, not a change to ``pmix_thread_fn_t``.

* **A statically initialized mutex is not ``ERRORCHECK`` even in a debug
  build.**  ``pmix_mutex_construct`` sets ``PTHREAD_MUTEX_ERRORCHECK``
  under ``PMIX_ENABLE_DEBUG`` so self-deadlocks and double-unlocks abort
  loudly, but ``PMIX_MUTEX_STATIC_INIT`` expands to
  ``PTHREAD_MUTEX_INITIALIZER`` — an ordinary mutex — because there is no
  static spelling of a mutex attribute.  This is a property of pthreads,
  not an oversight, and it cannot be fixed without giving up static
  initialization.  The consequence to remember is diagnostic: a clean
  debug run over a file-scope lock says nothing about whether it can
  deadlock.

* **``pmix_mutex_construct`` ignoring ``pthread_mutex_init``'s return is
  not an unchecked error.**  The constructor returns ``void``, so there
  is nothing it could do but abort, and on both glibc and macOS
  ``pthread_mutex_init`` with a valid attribute allocates nothing and
  cannot fail.  POSIX permits ``EAGAIN``/``ENOMEM``, so if a platform
  that can really fail it ever appears, add a debug-only ``perror`` and
  ``abort`` matching the ``EDEADLK``/``EPERM`` checks in the same file
  rather than trying to report it upward.

* **A** ``pcompress`` ``compress_string`` **that does not screen its
  argument for NULL is not a missing guard.**  All five implementations
  hand the pointer straight to ``strlen``, which reads as an asymmetry
  now that both *decompress* entry points screen for NULL — but the two
  directions take different data.  A decompressor is handed a length and
  a buffer a peer declared; a compressor is handed a string the caller
  just built and owns.  No caller in the tree can produce a NULL there,
  and a component answering ``false`` for one would report "I declined
  to compress" for what is really a caller bug, hiding it.  Add the
  screen only alongside a caller that can actually pass NULL.

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
* **The plog router builds its per-request channel list out of the
  global module wrappers.**  ``pmix_plog_base_log`` appends the
  ``pmix_plog_base_active_module_t`` objects that live in
  ``pmix_plog_globals.actives`` onto a local ``pmix_list_t``, so the
  list links and the ``added`` flag are written into shared state.  It
  reads as a re-entrancy bug waiting to happen, and it would be one — a
  module whose ``log`` reached ``pmix_plog_base_log`` synchronously
  would relink the objects out from under the loop walking them.  No
  module does: ``stdfd`` hands off to ``PMIx_server_IOF_deliver``, which
  posts an event and returns, and ``pmix_show_help`` thread-shifts
  rather than calling down inline.  Copying the wrappers per request
  would cost an allocation on every log call to defend against a caller
  that does not exist; the invariant is documented in
  ``src/mca/plog/AGENTS.md`` instead.  Enforce it there, not here.
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
* **``psec/dummy_handshake`` sends its length and status words as raw
  host-format ``size_t`` / ``pmix_status_t``.**  That means it only
  interoperates between peers of identical width and endianness, which
  would be a wire-format defect in a real mechanism.  It is not one here:
  the component exists solely to exercise the ``ptl``/``psec`` handshake
  plumbing, is built only under ``--enable-dummy-handshake``, and is
  documented as not a pattern to copy.  Do not "fix" it by inventing a
  wire encoding for a test harness.
* **``pmix_psec_base_select`` sets ``pmix_psec_globals.selected`` before
  it can fail.**  A select that ends with an empty actives list returns
  ``PMIX_ERR_SILENT`` with the flag already true, so a second call would
  return ``PMIX_SUCCESS`` over an unusable framework.  There is no second
  call: ``pmix_init.c`` invokes it once and treats the failure as fatal
  to library init.  Left as-is rather than adding a rollback for a path
  that cannot be re-entered.
* **The bare ``atomic_bool`` fields in ``pmix_globals_t``** are correct,
  merely inconsistent with the typedefs used elsewhere, and the
  ``PMIX_C_HAVE_*`` defines in the installed ``pmix_config.h`` are now
  always ``1`` but are retained in case an out-of-tree consumer tests
  them.
