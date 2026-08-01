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

Two subdirectories carry their own suites and their own guidance:

- [`class/`](class/) — the `src/class` object model and containers. See
  [`class/AGENTS.md`](class/AGENTS.md); it documents a build-configuration
  trap that let several real defects survive having a test suite.
- [`util/`](util/) — the `src/util` helpers.

## What lives here

Each program is a standalone `main()` linked against
`$(top_builddir)/src/libpmix.la`. There is no test framework, and there
should not be one: each prints its own pass/fail lines and exits non-zero
if anything failed. That keeps a failing test readable in a CI log
without cross-referencing a harness.

They fall into three groups.

**Self-contained library tests** — no server, no launcher, no network.
These run anywhere and are the bulk of the suite: `compress`, `preg`,
`bfrops_regex2`, `bfrops_alloc_inherit`, `info_support`, `iof_pattern`,
`hwloc_datatype`, `tracker_match`, `trk_complete`, `collective_status`,
`collect_job_info`, `progress_threads`, `pmix_log`.

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

**What it cannot cover, and why.** A singleton has no server, so every
path that round-trips — which is most of `src/client` — short-circuits
before it starts. `PMIx_Fence` and `PMIx_Commit` return success without
sending; `PMIx_Get` never leaves the local datastore;
publish/lookup/spawn/connect all stop at the "am I connected?" check.
That half of the directory is covered by
`contrib/dockerswarm/run-client-tests.sh`, which puts the ranks behind
different PMIx servers. Do not try to grow `client_api.c` into it: a
test that needs a server belongs in the swarm suite or in a `run_*.pl`
against `test/simple`.

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
