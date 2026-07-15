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

**Status:** `simptest` builds with either `--with-simptest` or
`--enable-test-build`, and it is **working** — it compiles against the
current `pmix_pnet_module_t` interface and drives the full
assign → pack → cache → `PMIx_Get` path end-to-end (exercised by the
[`test/simple/simpcoord.c`](../../../../test/simple/simpcoord.c) client,
which retrieves the assigned endpoints and coordinates). It was
previously stale (signature drift plus an internal `pnet_node_t`
field mismatch) and has been ported back to the live interface.

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

`setup_local_network` takes the current typedef's
`pmix_nspace_env_cache_t *nptr`; the underlying `pmix_namespace_t *` is
reached as `nptr->ns` when caching.

## The `pnet_node_t` topology cache

Each line of the config file becomes one `pnet_node_t`:

```c
typedef struct {
    pmix_list_item_t super;
    char *name;             // node name (first token on the line)
    pmix_coord_t coord;     // fabric coordinate (remaining integer tokens)
    pmix_byte_object_t endpt;  // synthesized endpoint string
} pnet_node_t;
```

`simptest_init` fills `coord` from the parsed integer vector (as
`uint32_t` values, logical view) and synthesizes `endpt` as the string
`"simptest.endpt.<name>"` — simptest has no real fabric, so the endpoint
is a recognizable placeholder.

## What the code does

- **`simptest_init`** reads the `config_file` line by line (skipping
  `#` comments), building the `mynodes` list. Missing file →
  `show_help("help-pnet-simptest.txt", "missing-file", …)`.
- **`allocate`** (scheduler only) parses `PMIX_PROC_MAP` and
  `PMIX_NODE_MAP` via `pmix_preg.parse_procs` / `parse_nodes`, then for
  each node emits **two** kinds of data into the blob:
  - a `PMIX_ALLOC_FABRIC_ENDPTS` kval whose value is a `PMIX_PROC_DATA`
    array — one entry per local rank holding the rank and its
    `PMIX_FABRIC_ENDPT` (a data array of byte objects). The endpoint is
    **per-process** data, fetched by rank.
  - a `PMIX_NODE_INFO_ARRAY` kval tagging this node (`PMIX_HOSTNAME`)
    with its `PMIX_FABRIC_COORDINATES` (a data array of `pmix_coord_t`).
    The coordinate is a **per-node** attribute — `PMIX_FABRIC_COORDINATES`
    is fetched at the node level (rank `PMIX_RANK_UNDEF`), not by rank, so
    it must be delivered as node info rather than proc data.

  All kvals are packed under a `"pmix-pnet-simptest-blob"` byte object
  appended to `ilist`. It returns `PMIX_ERR_TAKE_NEXT_OPTION` when no
  proc/node map is present.
- **`setup_local_network`** finds `"pmix-pnet-simptest-blob"`, unpacks the
  kvals, and caches each via `PMIX_GDS_CACHE_JOB_INFO`: the
  `PMIX_ALLOC_FABRIC_ENDPTS` proc-data becomes per-rank job info, and each
  `PMIX_NODE_INFO_ARRAY` becomes node-level info that the GDS matches to
  the local node. A client on that node then resolves both
  `PMIx_Get(rank, PMIX_FABRIC_ENDPT)` and
  `PMIx_Get(PMIX_FABRIC_COORDINATES)`.

## Gotchas

- **Endpoint is per-proc; coordinate is per-node.** This split is not
  cosmetic — `PMIX_FABRIC_ENDPT` is fetched by rank while
  `PMIX_FABRIC_COORDINATES` is fetched at the node level. Delivering the
  coordinate as proc data (as an earlier version did) caches it under a
  rank the client never queries, so `PMIx_Get` returns
  `PMIX_ERR_NOT_FOUND`. Keep the coordinate in a `PMIX_NODE_INFO_ARRAY`.
- **It is the canonical `PMIX_ALLOC_FABRIC_ENDPTS` producer.** The blob it
  builds is the shape a real fabric component's `allocate` should emit, so
  it is a working design reference.
- **Running it:** select the component and point it at a topology file,
  e.g. `PMIX_MCA_pnet=simptest
  PMIX_MCA_pnet_simptest_config_file=topo.txt ./simptest -n 2 -e
  ./simpcoord`, where `topo.txt` lists one `nodename coord...` line per
  node (the node name must match the launcher's node map).
- **Editing the help text** (`help-pnet-simptest.txt`) requires the
  regenerate-help golden rule: `rm src/util/pmix_show_help_content.* &&
  make`.
- **Priority 0 is intentional.** `simptest` must never outrank a real
  fabric component; keep it last in the fan-out.
