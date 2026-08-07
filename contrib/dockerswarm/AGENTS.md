# AGENTS.md: The PMIx dockerswarm test harness

This directory holds the multi-node test harness: a ten-container Docker
swarm (plus a native macOS mode) that exercises PMIx in the
configurations a developer's own `make check` cannot produce. Read the
top-level [`AGENTS.md`](../../AGENTS.md) first for the project-wide
rules.

**[`README.md`](README.md) is the authoritative document for this
directory** — 500+ lines covering what each runner does, how the swarm is
brought up, cleanup hygiene, and a section per suite. This file is the
short orientation an agent needs before touching anything here; when the
two disagree, the README wins, and please fix this file.

## What is here

| Script | Subject |
|--------|---------|
| `build.sh` | Builds the image, PMIx, PRRTE (and optionally Open MPI) into the `pmix-build` volume |
| `run-tests.sh` | The group construct/invite/destruct cases across real servers — the core multi-node suite |
| `run-group-events.sh` | Group event delivery |
| `run-topology.sh` | Topology / hwloc behavior |
| `run-python.sh` | The Python bindings |
| `run-class-tests.sh` | The `src/class` unit suite, in the two configurations nobody develops in (README §11) |
| `run-client-tests.sh` | The `src/client` API suite with ranks behind different servers (README §12) |
| `run-common-tests.sh` | The `src/common` role-shared API suite — query/log/job-control/allocation/monitor/IOF across separate servers (README §13) |
| `run-mca-tests.sh` | The `src/mca/base` unit suite plus hostile MCA parameters, in the `--enable-mca-dso` configuration where the component repository is actually exercised (README §14) |
| `run-bfrops-tests.sh` | The `src/mca/bfrops` unit programs on Linux/optimized/`--enable-mca-dso`, **plus** `examples/datatypes` moving every data type between ranks on different nodes (README §15) |
| `run-gds-tests.sh` | The `src/mca/gds` datastore: compiles `shmem3` (which macOS does not build at all), the gds unit programs in two configurations, and the collective/direct/fallback modex paths across separate servers (README §16) |
| `run-ptl-tests.sh` | The `src/mca/ptl` transport over real sockets between real hosts: interface selection, node-local rendezvous discovery, a tool attaching across nodes, the inbound message-size ceiling, an exhausted port range (README §17) |
| `run-runtime-tests.sh` | The `src/runtime` bring-up/tear-down suite in the optimized configuration and on Linux — where the progress thread's CPU-affinity path exists at all — plus that path driven through a real `PMIx_Init`, and node identity across real servers (README §18) |
| `run-server-tests.sh` | `src/server` — the server-role half of libpmix — across separate servers: direct modex, cross-namespace get, group blocks spanning nodes, IOF pull/dereg against a persistent DVM, and a valgrind pass on the daemon (README §19) |
| `swarm-common.sh` | **Sourced, never executed.** `$PMIX_SWARM` naming and the one copy of `cleanup_swarm` |

## Things that will bite you

- **`swarm-common.sh` is sourced on the host, but most work happens
  inside a `docker run ... bash -c '...'` string.** A helper function
  defined there is *not* available in that string. Duplicated logic
  inside those strings has to be kept in step by hand — which is why
  `cleanup_swarm` lives in exactly one place and the runners call it
  from the host side.

- **An uninstalled libtool `check_PROGRAM` is a `/bin/sh` wrapper, not
  an executable.** The binary is under `.libs/`. Copying the wrapper
  anywhere else produces a script that reports `.libs/<prog> does not
  exist` and exits 1. `run-class-tests.sh` staged the wrapper for a
  while, so its ten-node pass silently ran nothing at all.

- **A renamed or deleted MCA component directory wedges every existing
  build directory in the volume.** Automake records each component's
  `configure.m4` as a prerequisite of `aclocal.m4`; when one disappears
  (`gds/shmem2` → `gds/shmem3`), make decides `aclocal.m4` is stale,
  shells out to `aclocal-1.<n>`, and dies — the image carries no
  autotools. `build.sh` now detects a missing recorded prerequisite and
  reconfigures; anywhere else, delete `config.status` and re-run
  `configure`. The error message names `aclocal`, never the component.

- **`set -e` does not fire for a failing command in a non-final position
  of an `&&` list.** `make && make install` therefore swallows a build
  failure and reports success over a stale install. Write the two as
  separate statements. Several runners carry a comment saying so.

- **Ten containers on one host share a kernel and a CPU set.** A
  "ten-node" pass is a contention and load pass, not a distributed one,
  unless the thing under test genuinely crosses a server boundary. Say
  which you mean; `run-class-tests.sh` is explicit that its ten-node
  stage is only meaningful for the one multi-process program in that
  suite.

- **Ranks sharing a node hide the very thing a wire test is for.**
  `run-bfrops-tests.sh` launches with one slot per host
  (`--host node1:1,node2:1,... --map-by node`) rather than the
  `node1:2,node2:2` the other runners use, because a rank asking for a
  peer's data on its *own* node is answered out of the local datastore
  and never packs anything. With two ranks per node those pairs pass
  regardless of the state of the encoders, so the run can be green with
  the cross-node half broken. If you change the geometry of a test whose
  subject is serialization, keep one rank per node.

- **A PRRTE `--output` qualifier attaches with a colon, not a comma.**
  `file=NAME:pattern` selects PMIx's `PMIX_IOF_FILE_PATTERN` handling;
  `file=NAME,pattern` is parsed as a second *directive* and rejected. Worse,
  omitting the qualifier entirely is not an error — a name containing `%` is
  then just a stem, and PMIx annotates it the default way. A pattern test
  that forgets the qualifier passes against any library and proves nothing.
  See README §13.

- **A srcdir that has been built in place breaks the VPATH stages in
  two different ways, and only one of them says so.** autoconf refuses
  to configure alongside a `config.status` and names the problem. But
  even a stage that only *reuses* an already-configured tree in the
  volume will fail, because make resolves a missing prerequisite through
  VPATH, finds `foo.o` in the **source** directory, and then runs the
  link recipe with the bare name in the build directory. The error is
  `cannot find foo.o`, which mentions neither VPATH nor the srcdir, and
  it strikes exactly the objects that are new — so it looks like a bug
  in whatever you just added. `run-bfrops-tests.sh` gates both stages on
  one check for this reason; do not loosen it to let the reuse stage
  through.

- **Run a fuzz harness in ONE process, not one process per input.**
  Heap corruption planted by input N is usually noticed by the allocator
  at input N+k, so a harness that forks per input discards the evidence
  with the child. The bfrops fuzz stage found a remote heap overflow
  only after it was folded into `test/unit/bfrops_malformed.c` and run
  in-process; the scratch version that forked had been running against
  the same defect and reporting clean.

- **The shared volume is mounted READ-ONLY in the node containers.**
  Only the throwaway builder container gets it read-write, which is how
  the runners stage binaries into it. Anything a test needs a *node* to
  write has to go to that node's own filesystem instead. This fails
  quietly where it matters most: `prte --report-uri /opt/prte/...` does
  not complain, it simply never produces the file, and the case that
  needed the URI skips itself with a message about the DVM rather than
  about the path. `run-ptl-tests.sh` writes its URI to `/tmp` for this
  reason — and deliberately not to a name matching `pmix*`, which is the
  glob `cleanup_swarm` sweeps out of `/tmp` between cases.

- **`--host` IS the allocation, so an example that spawns needs slots
  nobody is using.** Three of the `run-server-tests.sh` examples
  (`dynamic`, `multi_nspace_group`, `resolve`) call `PMIx_Spawn`. Size the
  parent job to half the listed nodes or the spawn has nowhere to land and
  the job blocks until the launch timeout — which is indistinguishable
  from a library hang in the log, and it is not one. Leaving room is also
  what makes the child job land on *other* servers, which is the whole
  point of the cross-namespace cases.

- **`examples/resolve.c` spawns its child as the relative path
  `./resolve`.** Launch it from the staging directory or the spawned job
  dies with "could not access an executable" and the surviving parent rank
  hangs at its fence. Nothing in the message mentions the working
  directory.

- **Do not count completion markers per rank without checking who
  prints them.** `examples/dmodex.c` announces its finalize from rank 0
  only, so "one per rank" is the wrong expectation and a correct run looks
  like a partial one. Check the example before writing the assertion.

- **`ON` does not set the run-as-root variables; `RUN` does.** A stage
  that reaches for `ON <n>` to launch `prterun` gets the run-as-root
  advisory and a non-zero exit that looks like whatever the stage was
  actually testing. `run-server-tests.sh`'s valgrind stage uses `RUN` for
  the launch and `ON 1` only to read the log back.

- **valgrind prints bare basenames unless you pass `--fullpath-after=`.**
  That matters here because PRRTE has its own `pmix_server.c`,
  `pmix_server_fence.c` and `pmix_server_gen.c`: a basename match
  attributes PRRTE's leaks to PMIx, and a path match finds nothing at all.
  With the full path, PMIx frames read `/pmix-src/src/server/...` and
  PRRTE's read `/src/prrte/src/prted/pmix/...`, which separates them
  cleanly. The image carries no valgrind, so the stage installs it into
  the one node it needs at run time and skips if there is no network.

- **The build volume outlives your branch.** `pmix-build` persists
  across runs and across checkouts, so a tree in it can be older than
  the source you are testing. When results make no sense, check what is
  actually in `/opt/prte` before doubting the code.

## Adding a runner

Follow the shape of the existing ones: source `swarm-common.sh`, use
`swarm_up_or_die`, print `PASS`/`FAIL`/`SKIP` per case through the local
`ok`/`bad`/`skp` helpers, and exit non-zero if anything failed. Derive
the list of things to run from the relevant `Makefile.am` rather than
repeating it, so a new test is picked up without editing the script.
Then add a numbered section to [`README.md`](README.md) describing what
it covers and — just as important — what it deliberately does not.
