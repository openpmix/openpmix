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

  What that suite cannot cover *in a developer's own build* is the
  `dlopen` half of `src/mca/base`: the component repository is
  `#if PMIX_HAVE_PDL_SUPPORT` and does nothing at all in a
  statically-linked build, which is what a developer on macOS has. The
  `mca-dso` job in `.github/workflows/builds.yaml` runs these same
  programs against an `--enable-mca-dso` Linux tree on every pull
  request, and `contrib/dockerswarm/run-mca-tests.sh` does it across
  nodes. Reproduce a failure from either with
  `./configure --enable-mca-dso` locally — the programs are unchanged,
  only what they are linked against is.

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
`bfrops_helpers`, `info_support`, `iof_pattern`, `iof_inherit`,
`hwloc_datatype`, `tracker_match`, `trk_complete`, `collective_status`,
`collect_job_info`, `progress_threads`, `runtime_init`, `pmix_log`,
`server_get`, `resolve_api`, `spawn_api`.

`iof_inherit` covers the rule that a spawned job takes its parent's
output forwarding when the spawn request names no channel of its own
(see [`src/server/AGENTS.md`](../../src/server/AGENTS.md) and
[openpmix#4120](https://github.com/openpmix/openpmix/issues/4120)). It
drives `pmix_server_spawn_parser` and `pmix_server_process_iof` directly
and reads `pmix_globals.iof_requests` back rather than moving real
output: what is under test is which subscriptions get created and for
whom, and no spawn is needed to establish that — nor could one be used,
since `test/simple/simptest` cannot host a spawn. Its two inheritance
cases fail against an unfixed library, which was checked by neutering
the inheritance branch and re-running. Note the deliberate absence of a
"nothing to inherit" fallback: the case asserting that **no**
subscription is created there is load-bearing, because forwarding a
child's output to the process that spawned it is a loopback.

**Singleton client tests** — call the real public API in a process that
comes up with no server. `client_cycle` (init/finalize cycling),
`tool_cycle`, `singleton_register`, `rndz_stale`, `event_chain`,
`gds_fallback`, `client_api`, `client_commit`, `iof_pending`.

### `iof_pending` — output held for a spawn reply that has not landed

Comes up as a plain **tool**, because the thing under test is tool-only:
a client is never sent a spawned job's output. (`pfexec_iof` is a tool
too, but a launcher one, which is a different path - see below.) It
stands pipes up in place of stdout and stderr the way `iof_output` does,
then drives `pmix_iof_spawn_begin` / `pmix_iof_write_output` /
`pmix_iof_release_pending` / `pmix_iof_spawn_end` directly, each
thread-shifted onto the progress thread — all of them touch
`pmix_globals` state that belongs to it.

No spawn is issued and none could be (`test/simple/simptest` cannot host
one), so what it pins down is the cache's own contract rather than the
round trip: that nothing is held when no spawn is in flight, that two
concurrent spawns' output is released separately and formatted with each
one's own directives — the case a single process-wide slot cannot serve,
and the reason this exists — that arrival order survives, that the
last spawn being answered flushes what is left, and that the byte limit
makes the cache fall back rather than grow. Most of its cases fail
against an unfixed library, which was checked by making
`hold_for_spawn_reply` decline unconditionally and re-running.

The two assertions worth reading before editing it are the negative
ones. "held while a spawn is in flight" asserts that **nothing** appears
within a settle window, which is the only way to distinguish held from
written; and the second concurrent spawn's case asserts its output is
*not* tagged, which is what fails when both spawns share one set of
flags. A change that makes the release eager, or that drops the
per-namespace match, passes every positive case and fails those two.

**Perl-driven server tests** — `run_*.pl`, generated from `.pl.in` by
`configure`, which start a `test/simple` server and drive real clients
against it. The `run_grp*.pl` family covers group construct/invite/
destruct including the failure and timeout cases.

### `pfexec_iof` — the output directives a fork/exec'd job gets

[`pfexec_iof.c`](pfexec_iof.c) is the only program here that runs a
**real spawn end to end**, and it can because it comes up as a
`PMIX_LAUNCHER` with `PMIX_TOOL_DO_NOT_CONNECT` — the combination
`pfexec_params.c` uses. That is what selects `PMIx_Spawn_nb`'s fork/exec
dispatch: a launcher with no server fork/execs locally instead of asking
one. It stands pipes up in place of its own stdout and stderr the way
`iof_output` does, then fork/execs `/bin/sh` writing one line to each.

`docs/todo.rst` carried this as "a locally fork/exec'd job gets none of
its spawn's IOF directives" and recorded that there was no in-tree way to
exercise it. Both halves of that entry are answered here, and they are
different kinds of assertion:

- **The formatting directives already worked, by a route that is easy to
  miss.** The fork/exec path calls no spawn parser, but `pfexec`'s
  `register_nspace()` copies the spawn's job-level info into the job
  description it registers, and `gds/hash`'s
  `apply_job_value_effects()` runs the same `pmix_iof_check_flags()`
  over it. The `PMIX_IOF_TAG_OUTPUT` cases pass against an unfixed
  library and are here to keep that indirection from being refactored
  away silently — they check the tag names **this spawn's** namespace,
  so flags landing on the process-wide defaults instead would fail them.
- **The channel set did not.** `PMIX_FWD_STDOUT`/`STDERR` set false is a
  request for silence (see
  [`docs/how-things-work/iof_inheritance.rst`](../../docs/how-things-work/iof_inheritance.rst));
  those two cases fail against an unfixed library.

The negative cases pair a `settle()` on the silenced stream with a
`collect_until()` on the surviving one, and the order matters: the
surviving stream is the sentinel saying the job ran and was drained, so
the silence is a real absence rather than an answer we were too early to
see. Keep that pairing if you add a case.

`spawn_inheriting()` is the one case that does **not** go through
`PMIx_Spawn`. It builds a caddy and calls `pmix_pfexec_base_spawn_job()`
directly, which is all `pmix_server_spawn()`'s pfexec fallback does — the
path that serves a spawn a *client* sent. A client is not a tool, so the
parser's "forward everything to me" default does not apply and the caddy
names no channel at all, asking to inherit instead. Reading that empty
set as silence would mute every such spawn, and there is no client here
to send one. It fails if the inherit arm is removed, which was checked.

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

The seventh sweep added `test_get_empty_group()`, which plants a
`pmix_group_t` with no members on `pmix_client_globals.groups` and then
does a wildcard `PMIx_Get` against its group id. Expanding that used to
hand the caller the NULL `PMIx_Proc_create(0)` returns, with a count of
zero, which `PMIx_Get` then `memcpy`'d out of — so this one **segfaults**
against the unfixed library rather than failing.

Two things about its shape are deliberate. The group is planted directly
instead of being constructed and then emptied, because a singleton has no
server to construct one with, and the subject is what the expansion does
with an empty membership rather than how it got that way. And it covers
`PMIx_Get` **only**: the other callers of the expansion — fence,
connect/disconnect, group construct — answer their singleton or
`connected` check well before they reach the group list, so a case for any
of them would pass against the unfixed library and assert nothing. That
is the same unreachability described at the end of this section; resist
adding the "obvious" sibling cases.

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

### `client_commit` — what `PMIx_Commit` will send

[`client_commit.c`](client_commit.c) covers the delta-commit record
described in [`src/client/AGENTS.md`](../../src/client/AGENTS.md): which
keys `PMIx_Put` marks as owed to the server, and the cases that fall back
to the cumulative fetch instead.

It runs as a **singleton, and therefore never reaches `_commitfn`** —
`PMIx_Commit` short-circuits with `PMIX_SUCCESS` when there is no server.
What it reaches is the record the commit consults, which is readable
because `pmix_client_globals` is exported. That is deliberate rather
than a compromise: which keys a commit *would* fetch is the half of the
change that decides correctness, and it is the half a single process can
observe. The other half — that the commit sends them and the server
stores them — needs a server and lives in `contrib/dockerswarm`;
`examples/modex_twice.c` is the end-to-end shape.

Two cases carry the weight, and both fail against an unfixed library
(checked by neutering each independently):

- `test_repeat_put_recorded_once()` — a key published eight times before
  one commit is one key to send. This is what keeps the change from
  regressing the metric it exists to improve: the datastore replaces such
  a value in place, so a record that grew per put would send it eight
  times where the old cumulative fetch sent it once.
- `test_qualified_put_forces_resync()` — a `PMIX_QUALIFIED_VALUE` cannot
  be recorded by key at all, so it must set the cumulative flag.

The scope cases assert the record mirrors the datastore's own routing:
`PMIX_GLOBAL` is owed to both scopes because `gds/hash` files it into
both tables, and `PMIX_INTERNAL` to neither because it never leaves the
process.

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

### `event_forward` — what a server forwards, and what a client keeps

[`event_forward.c`](event_forward.c) is the only program here that runs a
real **client** against a real server, and it has to: both behaviors it
pins down live on the socket between them. It registers an nspace, forks,
and **execs itself** with a `client` argument — an exec rather than a bare
fork, because the parent has an initialized PMIx server in it and
`PMIx_Init` in a forked child finds the one-time-init latch already set.

Two cases, from [openpmix#4101](https://github.com/openpmix/openpmix/issues/4101):

- the server's fan-out must not drop an event a local handler wanted. The
  client registers a handler restricted to one process and then an
  unrestricted one — which sends nothing to the server, the code being
  already active — and the event names a third process;
- an event the client's handlers all declined must be parked and replayed
  to a handler registering afterwards. The declining handler restricts on
  its *source range*, which never leaves the client's process, so the
  server forwards an event it has no way to know is unwanted.

Both fail against the unfixed library, which was checked by reverting
each fix independently.

**Passing the parent's environment to the exec is load-bearing**, and
getting it wrong produces a very convincing false result. Handing
`execve` only what `PMIx_server_setup_fork` produces strands the child
without the library search path — and libtool starts an
uninstalled test through a wrapper script that sets exactly that — so the
child silently runs against the *installed* PMIx instead of the one under
test. That is the stale-install trap from
[`src/client/AGENTS.md`](../../src/client/AGENTS.md) wearing a different
hat: the parent behaves correctly, the child behaves like last month's
library, and the disagreement looks like a bug in the code under review.
`PMIx_Argv_copy(environ)` first, then `PMIx_server_setup_fork`, exactly
as `test/simple/simptest.c` does.

The second case's blocking round trip is the other load-bearing detail:
it puts the notification ahead of the second registration on the same
socket, so a library that parks nothing cannot pass by delivering the
event live.

### `server_get` — the server-side `PMIx_Get` regression test

[`server_get.c`](server_get.c) comes up as a PMIx server with a stub host
module that deliberately has **no** `direct_modex` entry point, registers
an nspace whose job size (4) exceeds its local size (2), and then calls
`pmix_server_get()` directly. That combination is what puts the two
behaviors it asserts on one code path.

The first is **local-vs-remote classification**: a target rank that is
not one of our local ranks must be classified remote. The rank walk used
to consult its `PMIX_LIST_FOREACH` variable after the loop had run to
completion — i.e. after it pointed at the list sentinel — and read a peer
id out of it, then used that garbage as an index into the client array.

**Be clear about what those cases are worth**: they pin the behavior, they
do not reproduce the defect. The stale read stays inside the enclosing
`pmix_namespace_t`, so it is undefined behavior that no sanitizer flags,
and the value is usually out of range — which accidentally produces the
right answer. Verified: the test passes against the unfixed library. It is
here so a rework of the classification cannot regress the answer silently.
Do not "strengthen" it by asserting something the old code happened to do.

The second is **deferred-request cleanup**, and that one is a leak check.
A request the server cannot launch must have its tracker fully discarded;
only the creation reference was being dropped, and since every parked
requester holds one of its own, the tracker survived unreachable. The
`local_reqs`-is-empty assertion here was satisfied by the old code too
(it did unlink the tracker); what it did not do was free it. **Run this
one under valgrind** — the stranded tracker, its request, its info array
and its key are what the leak report shows.

Two things about the setup are easy to get wrong and were:

- `PMIx_server_register_nspace`'s second argument is the number of
  **local** procs, not the job size. Passing the job size makes every
  rank local, `all_registered` never becomes true, and every case
  defers instead of classifying — which looks like a library bug.
- The node map needs **two** nodes. With one node in the map every rank
  is local however the counts are set, and the decision under test is
  never taken.

The multi-node half of this file — the actual remote fetch — is
[`contrib/dockerswarm/run-server-tests.sh`](../../contrib/dockerswarm/AGENTS.md).

### `server_group` — the server-side group handler regression test

[`server_group.c`](server_group.c) drives `pmix_server_group()` from
hand-packed wire buffers against a stub host module whose `group` entry
point **declines** every request. Declining is what makes the file
composable: the handler's refusal arm hands the whole block to
`grpcbfunc`, which answers every participant and tears the block down, so
each case leaves `pmix_server_globals.grp_collectives` empty and the next
one starts from a known state. Accepting would leave the completion to
the test, and a host stub cannot drive one without a peer that has a
socket.

Two of its cases are the reason it exists.

The **empty group id** takes an unfixed library down with SIGSEGV rather
than failing it (exit 139 with the buffered output lost — the signature
described under `bfrops_malformed`). A zero-length string unpacks to a
NULL pointer and reports success, and `get_tracker` `strcmp`s the id
against every block on the list and then `strdup`s it. So
`PMIx_Group_construct("")` from any local client killed the server's
progress thread and every client on the node with it. The client library
screens a NULL pointer, which an empty string is not.

The **late-registration pair** is the only thing in the suite that
reaches `pmix_server_grp_check_pending()`. `check_definition_complete`
gives up the moment it meets a participant namespace this server has not
been told about, and until that function existed the only thing that
called it again was the arrival of another local participant — so once
the last local participant had contributed, nothing was left to complete
the block. The two cases drive a construct naming an unregistered
namespace (it parks, and must not reach the host), then register that
namespace and assert the block goes up. Against an unfixed library the
first two pass and the last two fail, which is exactly the hang.

Its driver thread-shifts, like `server_fence`'s: the handler touches
`pmix_server_globals.grp_collectives` and must not be called from
`main()`. And the namespace it registers has **zero** local procs — that
is enough for `check_definition_complete`, which needs the namespace
known and its local count settled, and a count of zero settles it with
nothing to wait for.

What it cannot reach is anything past the host up-call: a group spanning
servers, the departed-member accounting, and the two-level block/tracker
engine under a real host. Those are
[`contrib/dockerswarm/run-tests.sh`](../../contrib/dockerswarm/AGENTS.md)
and `run-server-tests.sh`, and the `run_grp*.pl` family here.

### `server_inventory` — refusing a call the process is not entitled to make

[`server_inventory.c`](server_inventory.c) covers
`PMIx_server_collect_inventory` / `PMIx_server_deliver_inventory`, and its
two interesting cases are the ones that run in a **forked child**. Both
entry points fan out to `pmix_pnet` and `pmix_pgpu`, which only
`PMIx_server_init` opens; called from a tool they used to be answered
`PMIX_SUCCESS` and then crash on the progress thread, walking a list that
is only statically initialized. The child pattern is what makes that
reportable: the crash lands *after* the API has returned, so a
same-process case would take the whole suite down instead of printing a
FAIL. The child brings up a `PMIX_TOOL_DO_NOT_CONNECT` tool, calls one
entry point, requires `PMIX_ERR_INIT`, then makes a `PMIx_Get` to give
the progress thread a turn before exiting — without that turn a broken
library exits 0 and the case passes for the wrong reason. Against an
unfixed library both cases report "child died on a signal".

Copy the shape for any public entry point whose misuse is asynchronous;
`tool_api.c`'s `bad_directive_child` is the older instance of it.

### `server_op_replies` — reading back what the server actually queued

[`server_op_replies.c`](server_op_replies.c) covers the lookup reply, and
its shape is worth copying for anything in
[`src/server/pmix_server_op_replies.c`](../../src/server/pmix_server_op_replies.c):
it drives the *host callback* directly and then reads the reply off
`pmix_globals.mypeer->send_msg`, per the `server_control.c` idiom, rather
than inspecting a stub's arguments. What crossed the wire is exactly what
is under test.

`pmix_lookup_cbfunc_t` carries no release function, so the server has to
copy the host's array — through the **requesting peer's** bfrops module,
which is why a failed copy is reachable without an allocation failure.
The case forces one with a type no module implements, and the two fixes
behind it were told apart by re-breaking them one at a time: with
neither, nothing is queued at all and the client hangs in `PMIx_Lookup`
(the case reports `PMIX_ERR_NOT_FOUND`, i.e. no reply); with only the
status-only fallback, `PMIX_ERROR` — the client told rather than left
waiting; with both, `PMIX_ERR_PARTIAL_SUCCESS` and the elements that did
copy. That status is not arbitrary: `wait_lookup_cbfunc` and
`lookup_cbfunc` in
[`src/client/pmix_client_pub.c`](../../src/client/pmix_client_pub.c) both
treat it as data-carrying, and discard the whole array under any other
error.

Its barrier is `PMIx_Store_internal`, which blocks onto the same progress
thread the completion shifts to — the `server_dmodex.c` idiom, and the
reason there are no sleeps in it.

### `server_fabric` — and what a SKIP is for

[`server_fabric.c`](server_fabric.c) drives `pmix_server_device_dists()`
the way the switchyard does, to pin down that a locally computed distance
array is handed to the completion callback with a **release function**
rather than freed on the next line. That callback only thread-shifts, so
the reply is packed from the array after the handler has returned, and
freeing it there put freed heap on the wire.

It is the one program here that reports **SKIP**, and the reason is worth
copying rather than avoiding: hwloc needs at least one OS device to
measure a distance against, and a bare macOS host has none, so the
library correctly answers `PMIX_ERR_NOT_FOUND` and there is no result to
hold the contract against. Reporting that as a pass would be a green line
asserting nothing — on the developer platform, permanently. The
assertions run on Linux, where hwloc reports network and block devices.
Everything ahead of the computation runs on both.

### `resolve_api` — the local half of `PMIx_Resolve_peers`/`_nodes`

[`resolve_api.c`](resolve_api.c) is the only program in this suite that
reaches the *local computation* behind `PMIx_Resolve_peers` and
`PMIx_Resolve_nodes` rather than just their argument checks (those are in
`client_api.c`). It comes up as a PMIx server with a stub host module,
and that is what routes it there: both APIs consult
`pmix_host_server.query` first, and a stub module has none, so each falls
through to the thread-shifted local path. No client, no socket, no DVM.

The subject is a node that hosts **none** of a namespace's processes —
an ordinary state, since a namespace's map can name a node it placed
nothing on, and the datastore records that as a `PMIX_LOCAL_PEERS` value
that is the *empty string* rather than a NULL one. `PMIx_Argv_split()`
returns NULL for a string with no tokens, so the aggregate walk recorded
such a namespace and then indexed that NULL, and the single-namespace
path handed a zero count to `PMIX_PROC_CREATE` and reported the NULL it
gets back as `PMIX_ERR_NOMEM`.

Both halves were verified by reverting each fix independently: the
aggregate case **segfaults** against the unfixed library (exit 139, with
the buffered output lost, which is the signature described under
`bfrops_malformed` above), and the single-namespace case fails its
assertion.

Two things about the setup are load-bearing:

- The empty namespace is registered with a `PMIX_NODE_INFO_ARRAY` and
  **no proc map**. `store_map()` derives its own `PMIX_LOCAL_PEERS` from
  the proc map and *replaces* a host-supplied one, so adding a proc map
  would quietly overwrite the value under test.
- The namespace arguments are `pmix_nspace_t` variables, not string
  literals. These APIs take a fixed-size array, and gcc rejects a shorter
  literal outright under `-Werror=stringop-overread`.

What it cannot reach is the matching empty-list guard in
`resolve_nodes()`: `PMIX_NODE_LIST` is derived by joining the node map,
so the hash datastore cannot produce an empty one through
`PMIx_server_register_nspace`. That guard is hardening against a host
that stores the key itself. The node cases here only pin the ordinary
answers.

### `spawn_api` — `PMIx_Spawn`'s argument handling

[`spawn_api.c`](spawn_api.c) borrows `resolve_api`'s stub-host-server
trick for a different reason. `PMIx_Spawn_nb` gates on `connected`
*before* it looks at `job_info` or copies the apps array, so a singleton
client is turned away with `PMIX_ERR_UNREACH` and none of that code is
reachable from `client_api.c` — which is why the only spawn checks there
are the `apps`/`napps` ones that deliberately precede the gate. A
**server** is let through the gate so it can hand the request to its own
host, so with a stub module the directive scan and the apps copy both run
and the call stops at the absent `pmix_host_server.spawn`.
`PMIX_ERR_NOT_SUPPORTED` is therefore this file's "reached the end of the
argument handling" marker, and every case asserts it.

Two defects are pinned, both verified by reverting each fix
independently (each case fails on its own, and only its own):

- `PMIx_Spawn(job_info, 0, …)` — a non-NULL pointer with a zero count is
  "no directives", but the scan entered on the pointer alone and the
  resulting empty info list came back from `PMIx_Info_list_convert()` as
  `PMIX_ERR_EMPTY`, which failed the spawn.
- an app whose directives are **end-marked rather than counted**: the
  scan wrote the count it worked out back into the caller's `const
  pmix_app_t apps[]`. The sentinel has to sit past index 0 for the case
  to mean anything — a sentinel at index 0 counts zero, and writing zero
  over a zero proves nothing.

The apps here carry `cmd = "true"` and are never launched; nothing in
this file gets past the host up-call, by design.

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

### `run_grpinviteendpts.pl` — what an invited group carries beyond membership

[`run_grpinviteendpts.pl.in`](run_grpinviteendpts.pl.in) drives
[`examples/group_invite_endpts.c`](../../examples/group_invite_endpts.c)
over the same four-client simptest run, and covers the two things that
path gained in August 2026: the context ID its
`PMIX_GROUP_ASSIGN_CONTEXT_ID` directive promises (openpmix#4068, which it
had been accepting and dropping), and the members' endpoint data
(openpmix#4082).

**The absence of a `PMIx_Commit` is the test.** Each rank posts one value
at `PMIX_REMOTE` scope and deliberately never commits or fences it, so no
modex can carry it; a `PMIx_Get` that finds it afterwards can only have
been satisfied out of what the group exchanged. The gets are
`PMIX_OPTIONAL` so the request cannot leave the process, and carry the
context ID as a **qualifier** — a contribution to a group that was
assigned an ID is stored qualified by it, and `lookup_keyval()` in
[`src/util/pmix_hash.c`](../../src/util/pmix_hash.c) deliberately does not
match a qualified entry against an unqualified fetch. Both halves were
checked by disabling `store_endpts()` and confirming the run fails.

It also needs a host that answers the context-id request, which arrives
as a `PMIx_Job_control` directive because the invite method runs no
server collective. `jctrl_fn` in
[`test/simple/simptest.c`](../simple/simptest.c) does; a host that does
not is a legitimate configuration but not one this test can check, so a
missing ID is reported as a failure rather than passing quietly.

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

Note which unused-parameter spelling is available here.
`PMIX_HIDE_UNUSED_PARAMS` **is** — it is defined in
`src/include/pmix_globals.h`, which these programs include, and 35 of
them use it. The `__pmix_attribute_*__` wrappers are **not**: those are
gated on `PMIX_BUILDING` in `pmix_config_bottom.h`, which is not set
here, so a plain `(void) arg;` cast is the fallback where you would have
reached for one of them.

A test that needs a server is a different animal: add it to
`test/simple` with a `run_foo.pl.in` driver (and to `noinst_SCRIPTS`,
`EXTRA_DIST`, `TESTS`, and `.gitignore`), or to the dockerswarm suite
under [`contrib/dockerswarm`](../../contrib/dockerswarm).
