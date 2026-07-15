<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The GDS `hash` Component

`hash` is the default, always-available `gds` datastore: an in-process set
of hash tables keyed by rank. It is the baseline every PMIx process can
fall back to, and the reference implementation of the full
`pmix_gds_base_module_t` interface. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific to
`hash`.

## Files

| File | Contents |
|------|----------|
| `gds_hash.h` | Component struct, the per-nspace/job/app/node/session tracker classes, and the `PMIX_HASH_*` "what did we get" flags. |
| `gds_hash_component.c` | Component struct + `component_query` (priority **10**) + the `PMIX_CLASS_INSTANCE`s for the trackers. |
| `gds_hash.c` | The module itself: `assign_module`, `cache_job_info`, `register_job_info`, `store_job_info`, `store`, `store_modex`, the modex/kvs helpers, and the nspace/fork stubs. |
| `process_arrays.c` | Parsers that expand `PMIX_*_INFO_ARRAY` values into the session/job/app/node trackers (`pmix_gds_hash_process_*_array`). |
| `gds_utils.c` | Tracker lookup and node/host helpers (`pmix_gds_hash_get_tracker`, `check_session`, `check_nodename`, `store_map`, `store_qualified`, …). |
| `gds_fetch.c` | The retrieval side: `pmix_gds_hash_fetch` and the session/node/app fetch helpers. |

## When it is selected

`component_query` unconditionally hands back `pmix_hash_module` at priority
**10** — `hash` is *always* runnable (its `configure.m4` enables it on
every platform). Whether it is actually *assigned* to a given peer is a
separate decision made by `hash_assign_module`:

```c
*priority = 10;                        // default bid
if (info names PMIX_GDS_MODULE == "hash") {
    *priority = 100;                   // caller explicitly asked for us
}
```

So `hash` bids its low default (and loses to `shmem2`'s 20 where `shmem2`
is available) unless the caller passes `PMIX_GDS_MODULE="hash"`, in which
case it bids 100 and wins. On macOS, or any host where `shmem2` did not
build or cannot run, `hash` is the only active module and is always
assigned. It is also the module the framework falls back to when a client's
`shmem2` attach fails.

## The module

`hash` fills in **every** slot of `pmix_gds_base_module_t` except that it is
`is_tsafe = false`. Notably it implements `store`, `assemb_kvs_req`,
`accept_kvs_resp`, and `fetch_arrays` — the very slots `shmem2` leaves
`NULL` — which is why the framework's macro fallbacks name `"hash"` as the
"no fallback available, return `PMIX_ERR_NOT_SUPPORTED`" terminus.

## Data model

All state hangs off `pmix_mca_gds_hash_component`, which owns two lists:
`myjobs` (one `pmix_job_t` per nspace) and `mysessions`. The tracker
classes, all in `gds_hash.h`:

- **`pmix_job_t`** — per nspace. Holds three `pmix_hash_table_t`s that are
  the heart of the store: **`internal`** (job-level data and this proc's
  own data, keyed by `PMIX_RANK_WILDCARD` or rank), **`remote`**, and
  **`local`**. Also carries `jobinfo`, `apps`, `nodeinfo` lists, a back
  pointer to its `pmix_namespace_t`, and its `session`.
- **`pmix_session_t`** — `sessioninfo` + `nodeinfo` for a session id.
- **`pmix_apptrkr_t`** — per-application `appinfo` + `nodeinfo`.
- **`pmix_nodeinfo_t`** — a node's id, hostname, aliases, and `info` list.

The three hash tables encode PMIx **scope**: `store` routes an
`INTERNAL`-scope kval to `internal`, `REMOTE` to `remote`, `LOCAL` to
`local`, and `GLOBAL` to *both* `remote` and `local`; `fetch` then honors
the requested scope when reading them back. The `PMIX_HASH_*` bitmask flags
(`PROC_DATA`, `JOB_SIZE`, `NODE_MAP`, `PROC_MAP`, …) track which
job-defining keys have already been seen while caching, so duplicates
(e.g. two node maps) are caught as `PMIX_ERR_BAD_PARAM`.

## What the module functions do

- **`cache_job_info`** (server) — the big one. Walks the job-level
  `pmix_info_t` array for an nspace and files each key into the right place:
  session/app/node/job info arrays go through the `process_*_array`
  helpers; `PMIX_NODE_MAP` / `PMIX_PROC_MAP` are decoded via `pmix_preg`
  (handling `PMIX_REGEX`, `PMIX_REGEX2`, and plain `PMIX_STRING` forms) and
  turned into per-rank hostname data by `store_map`; `PMIX_PROC_DATA`
  arrays are expanded to per-rank keys; model/personality keys are handed to
  `pmix_pmdl`. Everything lands on `trk->internal` under
  `PMIX_RANK_WILDCARD` or the specific rank.
- **`register_job_info`** (server) — packs the cached job data for one
  connecting peer into a reply buffer (`register_info` does the work),
  formatting for the peer's version (there is explicit down-conversion of
  node info for peers earlier than v3.2). It caches the packed buffer on
  `ns->jobbkt` and reuses it for the remaining local clients of the nspace,
  releasing it once all have been served — the optimization the interface
  comment in `gds.h` anticipates.
- **`store_job_info`** (client) — the mirror image: unpacks that reply and
  populates this process's own `pmix_job_t` tables, including the
  `PMIX_PROC_BLOB` / `PMIX_MAP_BLOB` sub-buffers and, for the local proc,
  caching `appnum` / `nodeid` / `hostname` into `pmix_globals`.
- **`store`** — the `PMIx_Put` back-end; scope-routes one kval as described
  above.
- **`store_modex`** — delegates to `pmix_gds_base_store_modex` (the base
  envelope walker) with `_hash_store_modex` as the per-proc callback, which
  stores remote-proc kvals into `trk->remote`.
- **`fetch`** (`gds_fetch.c`) — resolves a `(peer, proc, key, scope)`
  request into a list of `pmix_kval_t`, reading the appropriate hash tables
  and the session/node/app lists, formatted for the requesting peer.
- **`setup_fork`, `add_nspace`, `mark_modex_complete`,
  `recv_modex_complete`** are no-ops for `hash` (it needs no child env, no
  per-nspace preparation, and no modex-complete handshake — the data is
  already in-process). `del_nspace` removes and releases the nspace's
  `pmix_job_t`.

## Gotchas

- **`hash` is the fallback terminus.** The framework macros treat a `NULL`
  slot on a `"hash"` module as `PMIX_ERR_NOT_SUPPORTED` rather than
  retrying elsewhere. Since `hash` implements every slot this never fires
  today, but do not remove a slot's implementation assuming "something else
  will pick it up" — for `hash`, nothing will.
- **Scope routing is the contract.** The `INTERNAL`/`REMOTE`/`LOCAL`/`GLOBAL`
  → table mapping in `store` must stay in lockstep with the scope filtering
  in `fetch`; a change to one without the other silently loses or leaks
  data across the local/remote boundary.
- **`internal` doubles as the job-level table.** Job-wide values live under
  `PMIX_RANK_WILDCARD` in `trk->internal`, alongside a proc's own copy of
  its data. Fetches for `rank=WILDCARD` read job-level data; do not assume
  `internal` holds only per-rank entries.
- **Regex decode depends on `preg`.** `cache_job_info` calls
  `pmix_preg.parse_nodes` / `parse_procs` (and `parse_regex` for
  `PMIX_REGEX2`). The node/proc maps are only as correct as the `preg`
  round-trip; see the `preg` framework doc.
