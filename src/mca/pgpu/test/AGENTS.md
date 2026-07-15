<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU `test` Component

`test` is the `pgpu` component that exists to **exercise the framework's
launch path on a host that has no GPU**. It harvests a configurable set of
environment variables, ships them in a blob, and replays them into local
children — the same producer/consumer shape a real vendor component uses —
so the `PMIx_server_setup_application` → `setup_local` → `setup_fork`
round trip can be tested without any vendor hardware. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `test`.

Unlike the vendor components (`amd`, `intel`, `nvd`), which gate selection
on a real hwloc hardware probe, `test` is **deterministically selectable**
on any machine — but only when you explicitly ask for it, in two
independent ways (build-time flag and runtime MCA selection). It never
activates by accident.

## Files

| File | Contents |
|------|----------|
| `pgpu_test.h` | Component struct `pmix_pgpu_test_component_t`, module extern, and the `PMIX_PGPU_TEST_BLOB` key. |
| `pgpu_test_component.c` | Component struct + open/close/register/query. Priority **10**. |
| `pgpu_test.c` | The module: `allocate` and `setup_local`. |
| `configure.m4` | Build gate — an `--with-pgpu-test` `AC_ARG_WITH`, **off by default**. |
| `Makefile.am` | Standard component build wiring. |

## Availability (how to turn it on)

Two independent opt-ins are required, so the component can never run in a
normal deployment:

1. **Build gate.** `configure.m4` adds `--with-pgpu-test`
   (`AS_HELP_STRING`), defaulting to *no*. Without the flag the component
   is compiled out and never appears in `base/static-components.h`; the
   configure summary reports `GPUs / Test` as not-happy. Because this is a
   `configure.m4` change, enabling it needs the full `./autogen.pl &&
   ./configure --with-pgpu-test && make` regen.
2. **Runtime selection.** Even when built, `component_open` returns
   success **only** if the local peer is a server **and** the `pgpu` MCA
   selection string names `test` (e.g. `PMIX_MCA_pgpu=test`). It looks up
   the `pmix`/`pgpu` MCA var and requires `"test"` to appear in it (via
   `strcasestr`), mirroring the opt-in gate `pnet/simptest` uses.

`component_query` sets priority **10** and hands back
`pmix_pgpu_test_module`.

## Component struct and MCA params

`pmix_pgpu_test_component_t` extends the base component with
`incparms`/`excparms` (the raw param strings) and `include`/`exclude`
(their `PMIx_Argv_split` argv forms). `component_register` registers:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_test_include_envars` | `PMIX_TEST_GPU_*` | comma-delimited glob list of envars to harvest (`*`/`?` supported) |
| `pgpu_test_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

The non-empty default means that with the component active, setting e.g.
`PMIX_TEST_GPU_FOO=bar` in the launcher's environment is enough to see the
variable travel through the whole pipeline and land in a child's
environment.

## The module functions

`pmix_pgpu_test_module` fills only `name`, `allocate`, and `setup_local`
(it implements neither inventory hook — those are irrelevant to the launch
path it tests). It mirrors the current `nvd` module, and deliberately
fixes two mistakes present in the vendor bodies:

- **`allocate`** — returns `PMIX_ERR_TAKE_NEXT_OPTION` if `info == NULL`
  or if neither `PMIX_SETUP_APP_ENVARS` nor `PMIX_SETUP_APP_ALL` was
  requested. Otherwise it harvests the `include` envars via
  `pmix_util_harvest_envars`, packs each as `PMIX_ENVAR`, compresses the
  buffer, and appends it to `ilist` under `PMIX_PGPU_TEST_BLOB`
  (`"pmix.pgpu.test.blob"`) as a `PMIX_COMPRESSED_BYTE_OBJECT` (or
  `PMIX_BYTE_OBJECT` if `pmix_compress` declined). **On the compression
  path it frees the uncompressed buffer** it unloaded — the vendor
  components leak it there.
- **`setup_local`** — finds `PMIX_PGPU_TEST_BLOB` in `info`, decompresses
  if needed, loads it into a scratch buffer with
  `PMIX_LOAD_BUFFER_NON_DESTRUCT`, and unpacks the `PMIX_ENVAR`s into
  `ns->envars` for the base `setup_fork` to inject. Two correctness
  points: it resets the expected `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER`
  terminator to `PMIX_SUCCESS` (so the base does not see a spurious hard
  error), and it does **not** `PMIX_DESTRUCT` the `NON_DESTRUCT` buffer —
  that buffer only borrows the caller's blob bytes, and freeing it would
  double-free the info array.

## Gotchas

- **It is a test harness, not a shipped feature.** Never enable
  `--with-pgpu-test` in a production build, and do not treat `test` as a
  reference for vendor *detection* (its `component_open` is opt-in, not a
  hardware probe). It *is* a good reference for the harvest/replay body
  and for the `NON_DESTRUCT` buffer handling.
- Keep `PMIX_PGPU_TEST_BLOB` unique across components; `setup_local`
  claims its data by matching the blob key string.
- The end-to-end test path runs through `test/simple/simptest`, which
  drives `PMIx_server_setup_application` (with `PMIX_SETUP_APP_ENVARS`),
  `PMIx_server_setup_local_support`, and `PMIx_server_setup_fork` — set
  `PMIX_MCA_pgpu=test` and a `PMIX_TEST_GPU_*` envar and watch it
  propagate.
