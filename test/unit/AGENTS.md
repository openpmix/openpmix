<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PMIx unit test suite

This directory is the top level of PMIx's `make check` unit suite. The
top-level [`AGENTS.md`](../../AGENTS.md) rules (copyright header,
`pmix_config.h` first, constant-on-the-left, brace everything,
warning-free under `--enable-devel-check`) apply here too, and its
"Never bend a test to accommodate a bug" rule applies with particular
force: nearly every program here exists because a specific defect got
past review, and weakening one hides exactly the regression it was
written to catch.

Three subdirectories carry their own suites:

- [`class/`](class/) — the `src/class` object model and containers. See
  [`class/AGENTS.md`](class/AGENTS.md); it documents a build-configuration
  trap that let several real defects survive having a test suite.
- [`util/`](util/) — the `src/util` helpers.
- [`mca/`](mca/) — [`src/mca/base`](../../src/mca/base): the MCA variable
  system, the enumerators, the component comparison/selection helpers,
  the `show_load_errors` parser and the framework register/open/close
  lifecycle. Every case there corresponds to a defect
  from the August 2026 review of that directory, listed in
  [`src/mca/base/AGENTS.md`](../../src/mca/base/AGENTS.md).

  Two things about that suite are load-bearing. Every program that
  brings the variable system up does so through `pmix_init_util()` — the
  lightest init that establishes the install dirs and the MCA — and
  settles what the parameter files will say **before** that call,
  because the variable system reads them exactly once during init and
  there is no second chance. `mca_base_var` sets
  `PMIX_MCA_mca_base_param_files=none` so its results do not depend on
  the developer's `~/.pmix`; `mca_base_paramfile` instead writes real
  files into a private temporary directory and points
  `mca_base_param_files` and `mca_base_override_param_file` at them,
  which is the only way to reach the precedence rules at all.
  `mca_base_var_enum` feeds the boolean enumerator's own `dump()` output
  back into its `value_from_string()`, on the principle that a value
  `pmix_info` advertises and the library then rejects is a bug rather
  than a cosmetic mismatch.

  What that suite cannot cover is the `dlopen` half of `src/mca/base`:
  the component repository is `#if PMIX_HAVE_PDL_SUPPORT` and does
  nothing at all in a statically-linked build, which is what a developer
  on macOS has. That half belongs to
  `contrib/dockerswarm/run-mca-tests.sh`, which builds an
  `--enable-mca-dso` tree.

## What lives here

Each program is a standalone `main()` linked against
`$(top_builddir)/src/libpmix.la`. There is no test framework, and there
should not be one: each prints its own pass/fail lines and exits non-zero
if anything failed. That keeps a failing test readable in a CI log
without cross-referencing a harness.

They fall into three groups.

**Self-contained library tests** — no server, no launcher, no network.
These run anywhere and are the bulk of the suite: `compress`, `preg`,
`bfrops_regex2`, `bfrops_alloc_inherit`, `bfrops_darray`,
`bfrops_malformed`, `bfrops_get_number`, `bfrops_null_object`,
`bfrops_helpers`, `info_support`, `iof_pattern`,
`hwloc_datatype`, `tracker_match`, `trk_complete`, `collective_status`,
`collect_job_info`, `progress_threads`, `runtime_init`, `pmix_log`.

**Singleton client tests** — call the real public API in a process that
comes up with no server. `client_cycle` (init/finalize cycling),
`tool_cycle`, `singleton_register`, `rndz_stale`, `event_chain`,
`gds_fallback`, `client_api`.

**Perl-driven server tests** — `run_*.pl`, generated from `.pl.in` by
`configure`, which start a `test/simple` server and drive real clients
against it. The `run_grp*.pl` family covers group construct/invite/
destruct including the failure and timeout cases.

### `client_api` — the `src/client` regression test

[`client_api.c`](client_api.c) is the singleton-side regression suite for
[`src/client`](../../src/client). Every case in it corresponds to a
defect found in the July 2026 review of that directory (see
[`src/client/AGENTS.md`](../../src/client/AGENTS.md) for the full list),
and several of them segfaulted before being fixed:

- `PMIx_Get_nb` for a request the library answers itself — `PMIX_PROCID`,
  `PMIX_VERSION_NUMERIC`, `PMIX_RANK` — which built a caddy with no "get
  logic" object and then released that field unconditionally.
- `PMIx_Get_nb` under `PMIX_GET_STATIC_VALUES`, which read an
  uninitialized pointer.
- `PMIx_Get` under `PMIX_GET_REFRESH_CACHE` with the NULL `proc` the API
  explicitly permits.
- Parameter validation on `PMIx_Get`, `PMIx_Get_nb`,
  `PMIx_Compute_distances_nb`, and the fabric APIs.
- `PMIx_Fabric_deregister` twice in a row, which used to free the info
  array a second time.

The August 2026 second sweep of that directory added:

- `PMIx_Put` with a NULL value or a NULL key — both were dereferenced by
  a `pmix_output_verbose()` call that sat above the validation.
- The OUT parameters of `PMIx_Compute_distances`, `PMIx_Resolve_peers`
  and `PMIx_Resolve_nodes`, each of which wrote a default through an
  unchecked pointer.
- `PMIx_Group_construct` / `PMIx_Group_invite` out-parameters, both
  poisoned before the call and passed as NULL, matching the
  `PMIx_Group_join` cases already here.
- `PMIx_Spawn[_nb]` with no apps array.
- A malformed `PMIX_HOSTNAME` qualifier on `PMIx_Get`, which the request
  parser used to hand straight to `strdup()`.
- `PMIx_Fabric_construct(NULL)` and `PMIx_Fabric_deregister[_nb]`
  **before `PMIx_Init`** — that one runs at the top of `main()`, ahead of
  init, because the defect was a missing `initialized` gate in front of a
  `pmix_globals.mypeer` dereference. Keep it there.

The third sweep added `test_topology_bad_params()`. The topology entry
points are the one part of `src/client` that answers entirely out of
hwloc, so they gate on `initialized` alone and a singleton reaches their
arguments — which makes them the easiest place here to cover the
"screen what the caller handed you" rule, and is why two unscreened
pointers had survived in them:

- `PMIx_Load_topology(NULL)`, which segfaulted on `topo->source`.
- `PMIx_Get_relative_locality(l1, l2, NULL)`, which stored the answer
  through the OUT parameter. **The inputs in that case have to be
  well-formed** (`"hwloc:NM0"`), or the call stops at the
  locality-payload check and never reaches the store — a version of
  this case with junk locality strings passes against the unfixed
  library and proves nothing. Both forms are in the test so the
  distinction stays visible.

### `bfrops_darray` and `bfrops_malformed` — the `src/mca/bfrops` regression tests

Both come from the August 2026 review of
[`src/mca/bfrops`](../../src/mca/bfrops/base/AGENTS.md), which lists the
defects individually. What is worth knowing here is why they are two
programs rather than one, because they assert different kinds of thing.

[`bfrops_darray.c`](bfrops_darray.c) is a **consistency sweep**, not a
list of cases. For a `pmix_data_array_t` the tree has to answer six
questions per element type — how to pack one, unpack one, copy one, how
wide an element is, how to copy an array of them, how to release an
array of them — in six different places, and nothing forces the last
three to be written. A type with pack, unpack and copy but no arm in
`pmix_bfrops_base_tma_data_array_construct()` looks finished: it works
as a scalar and packs fine inside an array, and only fails on the way
back. Eighteen registered types were in that state. So the test walks
`all_types[]` and holds construct, pack/unpack and copy against each
other for every one of them. **When you add a data type, add it to
`all_types[]`** — that is the only thing keeping the six answers in
step.

Two entries in that list are deliberate exceptions and should stay
documented rather than quietly dropped:

- `PMIX_REGEX` is absent. The packer strides such an array as `char *`
  and the destructor strides it as `pmix_byte_object_t`; those differ in
  width, so arrays of it are intentionally not constructible until
  somebody settles which stride is right. See the note in
  `data_array_construct()`.
- `PMIX_PROC_CPUSET` and `PMIX_TOPO` are skipped by the *copy* sweep
  because an all-zero hwloc object is not a valid one, so copying it
  legitimately declines. They get their own case
  (`test_uncopyable_cpuset_array_declines_cleanly`) asserting that it
  declines **without freeing the element block twice**, which is what it
  used to do — an abort, not a failure.

[`bfrops_malformed.c`](bfrops_malformed.c) asserts one contract:
**no input may make the unpacker read outside the buffer it was given.**
Returning an error is fine and returning a wrong value is tolerable;
walking off the end is not. That framing matters when you extend it —
several cases pass whether the library refuses or answers, and they are
written that way on purpose.

The two crashers it pins down are both three-byte messages. A count that
claims a hundred values in front of one byte of them used to keep
decoding past the allocation; and `flex_unpack_integer()` bounded its
loop with `flex_size - 1`, which is `SIZE_MAX` when nothing is left, so
it read until it happened to find a byte without a continuation flag.
Against the unfixed library the first of those segfaults when the
payload ends on a page boundary. `test_every_truncation_of_a_real_message`
is the broad net over the same class: it packs a well-formed message and
cuts it at every offset, which reaches decoder states nobody would think
to write down.

`test_random_bytes_through_every_unpacker` is the wider net still, and
it earned its place: it found two defects that reading did not. A data
array's element count sized the receiver's allocation with nothing
relating it to the size of the message, so a twenty-byte payload asking
for 2^40 elements got what it asked for and the process was OOM-killed;
and a query's qualifier count allocated, failed, and then unpacked into
the NULL. Its seeds are fixed and its generator is a plain LCG so any
failure is reproducible — if you find one, print the seed and the
payload rather than adding a one-off case.

Note what a failure of that stage looks like: the process dies with the
buffered stdout unflushed, so you get **no output at all** and exit 137
(OOM), 139 (SIGSEGV) or 134 (glibc heap assertion). That is the signal,
not a missing test.

**The fuzz stage runs its inputs in this one process on purpose.** Heap
corruption planted by one input is normally noticed by the allocator
several inputs later, so a harness that forks per input throws the
evidence away with the child. That is not hypothetical: the scratch
version of this fuzzer forked, ran clean for hours against a remote heap
overflow, and only surfaced it once folded in here. Do not "isolate" the
cases.

Two cases assemble a malformed message by hand, and they do it through a
byte accumulator rather than through `PMIx_Data_embed()`. **`PMIx_Data_embed()`
replaces a buffer's payload; it does not append to it.** A message built
by embedding one piece after another is only ever the last piece — a
one-byte buffer, which every unpacker refuses, so the test passes
against any library at all and proves nothing. Both cases were written
that way first.

[`bfrops_get_number.c`](bfrops_get_number.c) is the third, and it is a
**property test rather than a case list** for the same reason: the
function under test is two thousand lines of one near-copy of an arm per
(source type, destination type) pair, so enumerating cases reproduces
exactly the blind spot that put five defects in it at once. It states
two things and checks them over every pair at every width boundary — a
success must return the value it was given, and a refusal must be of a
value that genuinely did not fit.

The second property is the one that earns its keep. Thirteen
`PMIX_PROC_RANK` arms stored the correct answer and then fell through
without a `return`, so every conversion to a rank was right and reported
`PMIX_ERR_BAD_PARAM` — invisible to any test that only inspects
successful conversions. Likewise `PMIX_PID` had no source handler at all.

One exclusion in it is policy, not laziness: a PMIx structured value
(`PMIX_PID`, `PMIX_STATUS`, `PMIX_PROC_RANK`) is deliberately not
converted into another structured type, as `check_rank()` in the source
states. The test encodes that; do not delete the exclusion to make a
"missing" conversion appear.

[`bfrops_null_object.c`](bfrops_null_object.c) is the fourth, and it
asserts survival rather than any return code. A value tagged with a
pointer-backed type and carrying no object is malformed, but it takes
nothing more than setting `.type` before the data — and eighty-five
(type, operation) pairs segfaulted on one. What each operation *returns*
for such a value is deliberately not checked: "there is no object here"
is a state all of them can express, and which spelling a given one picks
is not the subject.

The one exception, which the test does pin down, is
`PMIX_DATA_ARRAY`: a value that names a data array and carries none must
copy to an **empty array**, not to another NULL, because callers
dereference the result. Do not relax that to "either is fine".

The test is noisy on purpose — a few of the types it walks have no
member in the value union at all, and the library prints a diagnostic
for them. Those lines are the library declining rather than faulting,
which is what is being asserted; do not quiet them by dropping the
types.

[`bfrops_helpers.c`](bfrops_helpers.c) is the fifth, and it covers the
public API surface that `bfrops/base` happens to own — the
`PMIx_Argv_*` family, the `PMIx_Info_list_*` builders, and the
construct/create/load/free helpers. Ten of those faulted on a NULL
argument. Like `bfrops_null_object` it asserts survival rather than a
return code, because refusing and quietly doing nothing are both
defensible for most of them.

It also runs each family on *ordinary* input right after the degenerate
cases. A screen added to a hot helper is exactly the kind of change that
can quietly break the path it was meant to protect, and a suite made
only of NULL cases would not notice.

Neither program needs a server, and neither can reach the two things a
*peer* decides — which bfrops module encodes a message, and whether the
buffer is described. A single-process round trip is self-consistent
under either, so it cannot fail on them. That half is
[`examples/datatypes.c`](../../examples/datatypes.c), driven across
separate nodes by
[`contrib/dockerswarm/run-bfrops-tests.sh`](../../contrib/dockerswarm/AGENTS.md).

### `runtime_init` — the `src/runtime` bring-up regression test

[`runtime_init.c`](runtime_init.c) covers what `pmix_rte_init` makes of
the directives a host passes it, and what `pmix_rte_finalize` gives back.
It comes up as a server because that is the role that actually passes
host directives through `pmix_rte_init`, and it runs **two full
init/finalize cycles**, re-checking everything on the second: this layer's
standing requirement is that a second `PMIx_Init` starts from a clean
slate, and per-cycle state rebuilt wrongly — or not rebuilt at all — shows
up nowhere else in the suite.

Its cases come from the August 2026 review of that directory (see
[`src/runtime/AGENTS.md`](../../src/runtime/AGENTS.md)): the alias list
that was never built for a host-supplied `PMIX_HOSTNAME`, the verbosity
channel and the IOF file/directory strings that were never released, and
the scalar directive state (`external_progress`, `external_topology`,
`nodeid`) that leaked from one cycle into the next.

One case is a different kind of thing and is worth keeping in mind for
any directory: `test_help_topics()` asks `pmix_show_help_string()` for
**every `show_help` topic `src/runtime` names**, with the same arguments
the real call site passes. That function returns `NULL` for a topic that
is not in the file, so one line per topic keeps code and help text in
step — two topics in `pmix_params.c` had never existed, and a user who
mistyped the color parameter got "I couldn't find that help reference"
instead of the diagnostic. The test also asks for a deliberately absent
topic, so the assertion is known to have teeth.

### `common_api` — the `src/common` regression test

[`common_api.c`](common_api.c) is the same idea one directory over: the
singleton-side regression suite for [`src/common`](../../src/common), the
role-shared public API layer. Every case corresponds to a defect from the
August 2026 review of that directory (see
[`src/common/AGENTS.md`](../../src/common/AGENTS.md)), and three of them
**segfault** rather than merely failing against an unfixed library — the
malformed `PMIX_LOG_SOURCE`, `PMIx_IOF_pull` with a source count and no
array, and printing the attributes of a function registered under a long
name. Those run early on purpose: a crash there means the rest never runs,
which is exactly the signal wanted.

- Malformed directive values, which is the recurring defect in that
  directory: `PMIX_LOG_SOURCE` read as a proc, the
  `PMIX_PROCID`/`PMIX_NSPACE`/`PMIX_RANK` query qualifiers read the same
  way, `PMIX_IOF_OUTPUT_TO_FILE`/`_TO_DIRECTORY` read as strings.
- `PMIx_Query_info` with no keys at all, and the locally-resolved queries
  repeated 200 times — including a run with a NULL callback, because the
  query caddy is released through several different paths depending on
  how the request was answered and that one used to release it through
  none.
- `PMIx_Data_copy_payload` with a NULL destination, and
  `PMIx_Info_directives_string` for `PMIX_INFO_PERSISTENT`.
- The IOF XML escaper, fed two high-bit bytes: it asserts they come back
  as `&#128;`/`&#255;` **and** that no `&#-` appears, so the test states
  what was actually wrong with the old signed-char output rather than
  just what the new output happens to be.
- `PMIx_Register_attributes` with a NULL name, with an over-long name
  (the stack-overflow case), and the two public attribute lookups with
  NULL.

The heartbeat hang from that same review is **not** here, because it
cannot be: a singleton is turned away at the "am I connected?" check long
before `PMIx_Process_monitor` reaches its heartbeat branch. It lives in
[`../simple/simpmonitor.c`](../simple/simpmonitor.c) on the observer rank
instead, driven by `run_monitor.pl`, where a regression is an unbounded
hang the driver's time limit catches.

**What it cannot cover, and why.** A singleton has no server, so every
path that round-trips — which is most of `src/client` — short-circuits
before it starts. `PMIx_Fence` and `PMIx_Commit` return success without
sending; `PMIx_Get` never leaves the local datastore;
publish/lookup/spawn/connect all stop at the "am I connected?" check.
That half of the directory is covered by
`contrib/dockerswarm/run-client-tests.sh`, which puts the ranks behind
different PMIx servers — and, for `src/common`, by
`contrib/dockerswarm/run-common-tests.sh`. Do not try to grow
`client_api.c` or `common_api.c` into them: a test that needs a server
belongs in the swarm suite or in a `run_*.pl` against `test/simple`.

The same short-circuit is why the *parameter* coverage here is uneven,
and the unevenness is not an oversight. Most entry points run
`initialized` → `connected` → `progress_thread_stopped` before they look
at their arguments, so in a singleton the state checks answer first and
the argument checks are dead code. The APIs above are exactly those that
validate before (or without) gating on `connected`. Do not reorder a
check in `src/client` to make it reachable from here — put the case in
a `run_*.pl` or the swarm suite instead.

### `run_grpinvitenb.pl` — the non-blocking ends of invite/join

[`run_grpinvitenb.pl.in`](run_grpinvitenb.pl.in) drives
[`examples/group_invite_nb.c`](../../examples/group_invite_nb.c): the
leader invites with `PMIx_Group_invite_nb`, and each invitee accepts with
a real `PMIx_Group_join_nb` callback. `run_grpinvite.pl` reaches neither
— it drives the blocking invite and passes no join callback — and both
were broken until the [openpmix#4059][i4059] follow-ons.

It asserts that the non-blocking invite completes *and* a group actually
forms (that form used to announce nothing at all, so no group formed),
and that every invitee's join callback fires with the group id and the
full membership (join used to complete when its acceptance went out,
carrying no group data, so `results` came back empty). Both halves are
checked in the client, which exits non-zero on failure; the driver also
greps for the pass lines so a client that skipped its checks cannot pass
the test vacuously.

### `run_grpinvitesuppress.pl` — an application that ends the event chain

[`run_grpinvitesuppress.pl.in`](run_grpinvitesuppress.pl.in) drives
[`examples/group_invite_suppress.c`](../../examples/group_invite_suppress.c),
which is `group_invite.c` plus one handler: the leader registers an
ordinary single-code handler for `PMIX_GROUP_INVITE_ACCEPTED`, logs the
acceptance, and returns `PMIX_EVENT_ACTION_COMPLETE`. That is the
reproducer from [openpmix#4059][i4059] — it used to hang the leader
forever, because the library counted invitation answers in an ordinary
multi-code handler that the application's single-code handler pre-empted.

The driver asserts in both directions: the group must still form (the
library saw the acceptances through its internal observer), *and* the
leader must have logged all N-1 acceptances (the library's own handler no
longer swallows the application's by completing the chain). The example
deliberately supplies no `PMIX_TIMEOUT`, so a regression is an unbounded
hang caught by the driver's time limit rather than an abort — don't
"fix" that by bounding the invite, since a bounded call turns the
regression into a different, weaker test.

The mechanism itself is covered separately by `test_observer` in
[`event_chain.c`](event_chain.c).

[i4059]: https://github.com/openpmix/openpmix/issues/4059

### `run_grpbadinfo.pl` — malformed directives must not crash the library

[`run_grpbadinfo.pl.in`](run_grpbadinfo.pl.in) drives
[`examples/group_badinfo.c`](../../examples/group_badinfo.c), which calls
`PMIx_Group_construct` twice with a `PMIX_GROUP_INFO` the library cannot
use: once carrying a scalar instead of a data array, once carrying a NULL
array. Both used to be a SIGSEGV inside the library on what is an
ordinary application mistake.

**The two cases exercise different code, which is the point of having
both.** The scalar reaches `construct_msg()` in
[`src/client/pmix_client_group.c`](../../src/client/pmix_client_group.c),
which read `->darray->array` off whatever shared the union. The NULL
array passes any "is it a data array?" test and dies one layer down in
`pmix_bfrops_base_tma_copy_darray()`. Fixing the first and re-running
this test is literally how the second was found. Reverting either fix
alone makes this test fail.

**This asserts survival, not a return code.** What
`PMIx_Group_construct` returns for a malformed directive is deliberately
not checked — the group can legitimately fail to form for other reasons
under `simptest`. A regression shows up as the client dying on a signal,
which the driver catches through the exit status *and* through the
per-case progress lines the client prints. Do not "strengthen" this by
asserting `PMIX_ERR_BAD_PARAM`; that would couple the test to a policy
decision it is not about.

### `run_grpinviteothers.pl` — a leader that is not a member

[`run_grpinviteothers.pl.in`](run_grpinviteothers.pl.in) is the sibling
of `run_grpinvite.pl`: same four-client simptest run, but rank 0 invites
ranks 1..3 and does **not** join, so the group forms on the invitees
alone. The library credits the leader's own answer against the
membership, which is right only when the leader is itself an invitee;
crediting it here resolves the invitation one answer early and — the
construct being all-or-nothing — aborts it. The driver greps for
`CONSTRUCT_ABORT` explicitly so that regression reports itself rather
than showing up as a timeout. See `invite_setup()` in
[`src/client/pmix_client_group.c`](../../src/client/pmix_client_group.c).

## Running

```sh
make check          # from this directory, or from test/
```

`make check` runs against the components in the **build tree**, so it
needs no prior `make install`: each test sources the generated
`test/pmix_test_env.sh` by way of `AM_TESTS_ENVIRONMENT`, which supplies
the component search path.

**These are `check_PROGRAMS`, so `make all` does not build them.** A
plain `make` here prints "Nothing to be done for `all'" and leaves stale
binaries in place — so after changing library source you can run
yesterday's binary against today's library and get a failure that has
nothing to do with your change. Always use `make check`.

**A stale `$TMPDIR` fails the server-based tests for environmental
reasons.** If the whole `run_*.pl` family fails at once, rerun under
`TMPDIR=$(mktemp -d) make check` before investigating; see the top-level
guide's "Building and Testing".

**This directory was invisible to `make check` in any tree not
configured `--disable-visibility`, and that has been fixed — know the
shape of it.** `test/Makefile.am` wrapped its whole `SUBDIRS` line in
`if !WANT_HIDDEN`, on the grounds that these programs use internal
symbols. The effect was that a default-visibility build ran the 15
programs in `test/` itself, printed `# FAIL: 0`, and silently skipped
`unit/`, `unit/class/`, `unit/util/`, `simple/` and `topologies/`. So the
optimized configuration these tests exist to cover was never actually
tested by `make check`, and nothing said so.

The condition is gone: every internal symbol these programs reach is now
exported, and all five subdirectories build and pass with visibility on.
Verified in five configurations across both primary platforms —
Linux/gcc (`--enable-debug --disable-visibility`, `--enable-debug`,
`--disable-debug`) and macOS/clang (`--enable-debug`, `--disable-debug`)
— 80 tests passing and zero warnings in every one. The macOS
default-visibility build also confirms the symbol state the whole thing
rests on, using the `nm` audit described in
[`src/class/AGENTS.md`](../../src/class/AGENTS.md): 134 class
descriptors, 103 exported, and the 31 hidden ones all private to a
single `.c` or to `gds/hash`, whose header is not installed. **No class
declared in an installed header is hidden** — which is precisely why
these programs link now and did not before.

Two things guard against a repeat, and both were tested by triggering
them:

- `test/Makefile.am` has a `check-local` that **fails** if `SUBDIRS` ever
  comes out empty, rather than letting `make check` recurse into nothing
  and report success.
- The top-level `Makefile.am` prints a loud "no tests were run" notice
  when the tree is configured `--without-tests-examples`, which drops
  `test/` from `SUBDIRS` one level up and produces exactly the same
  silent, successful nothing.

If you hit a test that genuinely cannot link without
`--disable-visibility`, the fix is `PMIX_EXPORT` on what it needs (see
the export rule in [`src/class/AGENTS.md`](../../src/class/AGENTS.md)),
not a condition here that removes the tree from `make check` without a
word.

**These tests do not run in parallel, deliberately.**
[`Makefile.am`](Makefile.am) is marked `.NOTPARALLEL:`. Most of `TESTS`
drives a real PMIx server through `test/simple`'s `simptest`, which comes
up in the **scheduler** role — a node-wide singleton that claims a fixed
`$TMPDIR/pmix.sched.<host>` rendezvous file. Automake's parallel harness
gives each test its own `.log` target, so `make -j check` starts several
at once and every one but the winner dies with `PMIX_ERR_INIT` and "Only
one server can fill a given role on a node at a time". That is the server
enforcing a real invariant, not flakiness. Do not paper over it by giving
each test a private `$TMPDIR`: two schedulers on one node is exactly the
configuration PMIx forbids, so a suite arranged that way would be testing
something users cannot have. If you want concurrency here, the tests have
to stop being schedulers.

**Do not run this suite against an `--enable-test-build` tree.** Its
shimmed `pcompress`/`psec` components are non-functional by design, so
`preg` and `compress` will fail for reasons that are not defects.

## Adding a test

1. Write `foo.c` with the standard copyright header and a `main()` that
   returns non-zero on failure.
2. Add `foo` to both `check_PROGRAMS` and `TESTS` in
   [`Makefile.am`](Makefile.am), plus a `foo_SOURCES` / `foo_LDFLAGS` /
   `foo_LDADD` triple matching its neighbours.
3. Add `test/unit/foo` to the top-level [`.gitignore`](../../.gitignore)
   — the build products here are ignored from the root, not from a local
   `.gitignore` (unlike `class/`, which has its own).
4. Run `make check`. A `Makefile.am` edit needs only a plain `make`; no
   `autogen.pl`/`configure` re-run.

Note that `PMIX_HIDE_UNUSED_PARAMS` and the other
`__pmix_attribute_*__` wrappers are **not** available to these programs —
they are gated on `PMIX_BUILDING` in `pmix_config_bottom.h`, which is not
set here. Use a plain `(void) arg;` cast instead.

A test that needs a server is a different animal: add it to
`test/simple` with a `run_foo.pl.in` driver (and to `noinst_SCRIPTS`,
`EXTRA_DIST`, `TESTS`, and `.gitignore`), or to the dockerswarm suite
under [`contrib/dockerswarm`](../../contrib/dockerswarm).
