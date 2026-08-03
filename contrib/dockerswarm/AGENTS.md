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

- **A PRRTE `--output` qualifier attaches with a colon, not a comma.**
  `file=NAME:pattern` selects PMIx's `PMIX_IOF_FILE_PATTERN` handling;
  `file=NAME,pattern` is parsed as a second *directive* and rejected. Worse,
  omitting the qualifier entirely is not an error — a name containing `%` is
  then just a stem, and PMIx annotates it the default way. A pattern test
  that forgets the qualifier passes against any library and proves nothing.
  See README §13.

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
