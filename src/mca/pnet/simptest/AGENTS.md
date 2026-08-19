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
| `help-pnet-simptest.txt` | `show_help` for the `missing-file`, `node-not-found` and `bad-coordinate` topics. |

## When it would be selected

Selection is deliberately opt-in and gated in `component_open`:

- The `config_file` MCA parameter (`pmix_pnet_simptest_config_file`) must
  be set **and** the local peer must be a server.
- The user must have explicitly named this component in the `pnet` MCA
  selection string — `component_open` looks up the `pmix`/`pnet` MCA var
  and requires `"simptest"` to be an *entry* of it.
- `component_query` then returns `pmix_simptest_module` at priority **0**
  (the lowest, so it never shadows a real component).

Every one of those declines returns `PMIX_ERR_NOT_AVAILABLE`, the MCA's
"silently ignore me" cue; any other status is reported to the user as a
component that *failed to open*, which is not what declining an
invitation looks like — and for this component declining is the normal
case.

The selection check is narrower than it looks, and it is worth knowing
why it is there. The MCA base has **already** filtered on that list
before any component is opened — `pmix_mca_base_component_find`'s
`use_component()` honors both the include and the exclude (`^simptest`)
forms — so `component_open` only runs when this component was named, or
when **nothing** was named at all. That second case is the one the gate
exists for: with no `pnet` setting the base opens every component it
built, and building a component is not the same as asking for it. `tcp`
applies the same rule for the same reason.

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
  - a `PMIX_ALLOC_FABRIC_ENDPTS` kval whose value is a `PMIX_PROC_INFO_ARRAY`
    array — one entry per local rank holding the rank and its
    `PMIX_FABRIC_ENDPT` (a data array of byte objects). The endpoint is
    **per-process** data, fetched by rank.
  - a `PMIX_NODE_INFO_ARRAY` kval tagging this node (`PMIX_HOSTNAME`)
    with its `PMIX_FABRIC_COORDINATES` (a data array of `pmix_coord_t`).
    The coordinate is a **per-node** attribute — `PMIX_FABRIC_COORDINATES`
    is fetched at the node level (rank `PMIX_RANK_UNDEF`), not by rank, so
    it must be delivered as node info rather than proc data.

  All kvals are packed under a `PMIX_PNET_SIMPTEST_BLOB` byte object
  appended to `ilist`. It returns `PMIX_ERR_TAKE_NEXT_OPTION` when no
  proc/node map is present.
- **`setup_local_network`** finds `PMIX_PNET_SIMPTEST_BLOB`, unpacks the
  kvals, and caches each via `PMIX_GDS_CACHE_JOB_INFO`: the
  `PMIX_ALLOC_FABRIC_ENDPTS` proc-data becomes per-rank job info, and each
  `PMIX_NODE_INFO_ARRAY` becomes node-level info that the GDS matches to
  the local node. A client on that node then resolves both
  `PMIx_Get(rank, PMIX_FABRIC_ENDPT)` and
  `PMIx_Get(PMIX_FABRIC_COORDINATES)`.

## Invariants worth knowing before you edit

- **The node map and the proc map must be the same length, and nothing
  upstream guarantees it.** `allocate` walks them in lockstep - it reads
  `procs[n]` for every `nodes[n]` - but they are two separate strings
  parsed by two separate `pmix_preg` calls, and `parse_procs` splits with
  the *empty-collapsing* `PMIx_Argv_split`, so a proc map naming a node
  that hosts nothing comes back shorter than the node map it belongs to.
  Every later node then gets another node's ranks, and once `n` passes
  the proc array's terminator the read is off the end of the allocation
  entirely. Compare the two counts first and refuse the pair when they
  disagree; `gds/hash`'s `store_map` has always made exactly this check
  on exactly this pair.

- **Either map can arrive in three shapes.** `PMIX_STRING`, the
  deprecated encoded `PMIX_REGEX` (payload in `value.data.bo.bytes`), or
  the `PMIX_REGEX2` that `PMIx_generate_regex2` produces and that new
  hosts should send. Reading `value.data.string` unconditionally works
  for the first two only because `.string` and `.bo.bytes` alias; for a
  `PMIX_REGEX2` it hands the parser a `pmix_regex2_t *` cast to `char *`.
  The file-local `parse_map` dispatches on the type, mirroring the
  helper of the same name in `gds/hash`'s `gds_utils.c`. If a third
  caller ever needs it, that is the point to hoist it into `preg`.

- **The blob key lives in the header, once.** `PMIX_PNET_SIMPTEST_BLOB`
  used to read `"pmix.pnet.simptest.blob"` while both ends of the
  exchange hand-wrote `"pmix-pnet-simptest-blob"` - so the macro was
  dead, and anyone "tidying up" by using it would have moved the key out
  from under the receiver. Both sites name the macro now; the value is
  the one that has always been on the wire, so do not change it.

- **`setup_local_network` must load the blob non-destructively.** The
  info array belongs to the caller and is handed to every active module
  in turn. `PMIX_LOAD_BUFFER` NULLs the source byte object, and the
  buffer is deliberately never destructed since the bytes are not ours,
  so a destructive load orphans the blob and empties it for everyone
  behind us. The manual restore this used to do at the end of the
  iteration was skipped by both error returns. Use
  `PMIX_LOAD_BUFFER_NON_DESTRUCT`, as `opa`, `nvd` and `tcp` do.

- **A module whose `init` fails is never added to `actives`, so its
  `finalize` never runs.** `pmix_pnet_base_select` skips it. `simptest_init`
  therefore has to destruct `mynodes` itself before returning an error;
  nothing else will.

- **Both entry points run on the progress thread**, so the static
  `mynodes` list needs no locking - `allocate` and `setup_local_network`
  are reached from server code that has already thread-shifted. simptest
  has no `setup_fork` slot, no inventory hooks and no fabric entry
  points, which are the places the framework `AGENTS.md` warns arrive on
  someone else's thread.

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
  node.
- **The config-file node names must match the launcher's node map.**
  `allocate` looks up each node from the `PMIX_NODE_MAP` in `mynodes` by
  exact name (`strcmp`), so a name in the topology file that does not
  appear in the job's node map is silently skipped and its procs get no
  fabric data. This is intrinsic to a topology-description file - it names
  the real nodes the RM placed the job on - not a bug to fix. The
  **converse** is not silent: a node the job *was* placed on that the
  topology file does not describe gets a `node-not-found` `show_help`
  naming it, and the whole request is then declined. It used to be
  `PMIX_ERR_NOT_FOUND` commented "should be impossible", which is both
  wrong - it is the ordinary spelling mistake - and damaging, since a
  hard error from `allocate` aborts the base's fan-out for every other
  component too. Note the
  node map may carry the **short** host name (e.g. `node01`) while the OS
  reports an FQDN; use whatever name the launcher's node map uses. (The
  per-node coordinate is tagged with that same config name as its
  `PMIX_HOSTNAME`; the backend GDS reconciles short-name/FQDN/alias
  differences when it matches the node-info to the local node, so the
  coordinate still resolves on the compute node.) With
  `test/simple/simptest` the map is generated from the local host, so the
  topology file needs a single line for this node.
- **Editing the help text** (`help-pnet-simptest.txt`) requires the
  regenerate-help golden rule: `rm src/util/pmix_show_help_content.* &&
  make`.
- **Priority 0 is intentional.** `simptest` must never outrank a real
  fabric component; keep it last in the fan-out.

## Tests

[`test/unit/pnet_simptest_map.c`](../../../../test/unit/pnet_simptest_map.c)
drives `allocate` through `PMIx_server_setup_application` against a
topology file the test writes, and covers the three map encodings, the
node/proc count mismatch, a node the topology file omits, and a topology
file whose last line carries no trailing newline (it checks the
coordinate that line supplies, since truncating it loses a dimension
rather than the node name).

Because the component is opt-in at configure time, the test asks
`pmix_mca_base_var_find("pmix", "pnet", "simptest", "config_file")`
whether it was built and exits **77** (automake's "skip") when it was
not. That probe answers "built", not "selected".

The end-to-end path still has to be run by hand - see **Running it**
above; `make check` does not cover it, because `test/simple/simptest`
generates its node map from the local host and so needs a topology file
naming that host.
