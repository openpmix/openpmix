<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `simptest` Component

`simptest` is the `pnet` component used as a **test / example fabric** — a
scheduler reads a fabric-topology description from a file and hands each
process static endpoints and coordinates, so the endpoint-assignment path
can be exercised without real fabric hardware. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `simptest`.

**Status warning:** `simptest` builds only with `--with-simptest`, and in
its current form it is **stale** — its `setup_local_network` signature
does not match the current `pmix_pnet_module_t`, and its module body
references struct fields (`nd->coord`, `nd->endpt`) that its own
`pnet_node_t` definition does not contain. As written it would not compile
even with `--with-simptest`. Treat it as an illustrative sketch of the
static-endpoint assignment flow, not as working test infrastructure.

## Files

| File | Contents |
|------|----------|
| `pnet_simptest.h` | Component struct type + `PMIX_PNET_SIMPTEST_BLOB` key. |
| `pnet_simptest_component.c` | Component struct, `component_register` (`config_file` param), `component_open`/`component_query` (priority **0**). |
| `pnet_simptest.c` | The module: init/finalize, allocate, setup_local_network, plus the `pnet_node_t` topology cache. |
| `configure.m4` | Guarded by `--with-simptest`; adds the `Simptest` summary line. |
| `help-pnet-simptest.txt` | `show_help` for the `missing-file` topic. |

## When it would be selected

Selection is deliberately opt-in and gated in `component_open`:

- The `config_file` MCA parameter (`pmix_pnet_simptest_config_file`) must
  be set **and** the local peer must be a server, otherwise `open` returns
  an error.
- The user must have explicitly named this component in the `pnet` MCA
  selection string — `component_open` looks up the `pmix`/`pnet` MCA var
  and requires `"simptest"` to appear in it (via `strcasestr`).
- `component_query` then returns `pmix_simptest_module` at priority **0**
  (the lowest, so it never shadows a real component).

## The module

```c
pmix_pnet_module_t pmix_simptest_module = {
    .name = "simptest",
    .init = simptest_init,
    .finalize = simptest_finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network
};
```

Note `setup_local_network` here is written as
`(pmix_namespace_t *nptr, …)`, which does **not** match the current
typedef's `pmix_nspace_env_cache_t *` — one of the reasons the file is
stale.

## What the code is trying to do

- **`simptest_init`** reads the `config_file` line by line (skipping
  `#` comments), building a `mynodes` list of `pnet_node_t` — one per
  node, each with a name and an integer coordinate vector parsed from the
  rest of the line. Missing file → `show_help("help-pnet-simptest.txt",
  "missing-file", …)`.
- **`allocate`** (scheduler only) parses `PMIX_PROC_MAP` and
  `PMIX_NODE_MAP` via `pmix_preg.parse_procs` / `parse_nodes`, then for
  each node assigns every local rank a `PMIX_PROC_DATA` array holding its
  rank, a `PMIX_FABRIC_COORDINATE`, and a `PMIX_FABRIC_ENDPT`. The kvals
  are packed under `PMIX_ALLOC_FABRIC_ENDPTS` into a
  `"pmix-pnet-simptest-blob"` byte object appended to `ilist`. It returns
  `PMIX_ERR_TAKE_NEXT_OPTION` when no proc/node map is present.
- **`setup_local_network`** finds `"pmix-pnet-simptest-blob"`, unpacks the
  kvals, and for `PMIX_ALLOC_FABRIC_ENDPTS` caches the per-rank endpoint /
  coordinate arrays as job info via `PMIX_GDS_CACHE_JOB_INFO`.

The `pnet_node_t` destructor frees `pmix_geometry_t` devices,
`pmix_endpoint_t` endpts, and `pmix_device_distance_t` distances — but the
`allocate`/`init` code instead reads/writes `nd->coord` and `nd->endpt`,
fields the struct does not declare. That mismatch is the concrete bug that
prevents compilation.

## Gotchas

- **It does not compile as-is.** Before using `simptest` for anything you
  must reconcile the `pnet_node_t` fields (`coord`/`endpt` vs.
  `devices`/`endpts`/`distances`) and update `setup_local_network` to the
  current `pmix_nspace_env_cache_t *` signature.
- **It is the canonical `PMIX_ALLOC_FABRIC_ENDPTS` producer.** The blob it
  builds (rank + `PMIX_FABRIC_COORDINATE` + `PMIX_FABRIC_ENDPT` per proc)
  is the shape a real fabric component's `allocate` should emit, so it is
  useful as a design reference even while broken.
- **Editing the help text** (`help-pnet-simptest.txt`) requires the
  regenerate-help golden rule: `rm src/util/pmix_show_help_content.* &&
  make`.
- **Priority 0 is intentional.** `simptest` must never outrank a real
  fabric component; keep it last in the fan-out.
