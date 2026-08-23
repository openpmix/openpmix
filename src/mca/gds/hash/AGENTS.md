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

So `hash` bids its low default (and loses to `shmem3`'s 20 where `shmem3`
is available) unless the caller passes `PMIX_GDS_MODULE="hash"`, in which
case it bids 100 and wins. On macOS, or any host where `shmem3` did not
build or cannot run, `hash` is the only active module and is always
assigned. It is also the module the framework falls back to when a client's
`shmem3` attach fails.

## The module

`hash` fills in **every** slot of `pmix_gds_base_module_t` except that it is
`is_tsafe = false`. Notably it implements `store`, `assemb_kvs_req`,
`accept_kvs_resp`, and `fetch_arrays` — the very slots `shmem3` leaves
`NULL` — which is why the framework's macro fallbacks name `"hash"` as the
"no fallback available, return `PMIX_ERR_NOT_SUPPORTED`" terminus.

## Data model

All state hangs off `pmix_mca_gds_hash_component`, which owns two lists:
`myjobs` (one `pmix_job_t` per nspace) and `mysessions`. The tracker
classes, all in `gds_hash.h`:

- **`pmix_job_t`** — per nspace. Holds three `pmix_hash_table_t`s that are
  the heart of the store: **`internal`** (job-level data and this proc's
  own data, keyed by `PMIX_RANK_WILDCARD` or rank), **`remote`**, and
  **`local`**. Also carries `apps` and `nodeinfo` lists, a back
  pointer to its `pmix_namespace_t`, and its `session`.
- **`pmix_session_t`** — `sessioninfo` + `nodeinfo` for a session id.
- **`pmix_apptrkr_t`** — per-application `appinfo` + `nodeinfo`.
- **`pmix_nodeinfo_t`** — a node's id, hostname, aliases, and `info` list.

The three hash tables encode PMIx **scope**: `store` routes an
`INTERNAL`-scope kval to `internal`, `REMOTE` to `remote`, `LOCAL` to
`local`, and `GLOBAL` to *both* `remote` and `local`; `fetch` then honors
the requested scope when reading them back. The `PMIX_HASH_*` bitmask flags
(`JOB_SIZE`, `MAX_PROCS`, `NUM_NODES`, `NODE_MAP`, `PROC_MAP`) track which
job-defining keys have already been seen while caching, so duplicates
(e.g. two node maps) are caught as `PMIX_ERR_BAD_PARAM` and so `store_map`
knows which job-level values it still has to compute for itself. **Every
one of them is a job-wide statement**, which is what they are for; do not
add a flag to record something that is true of one rank or one key. There
used to be a sixth, `PROC_DATA`, that did exactly that — see the gotcha
below.

## What the module functions do

- **`cache_job_info`** (server) — the big one. Walks the job-level
  `pmix_info_t` array for an nspace and files each key into the right place:
  session/app/node/job info arrays go through the `process_*_array`
  helpers; `PMIX_NODE_MAP` / `PMIX_PROC_MAP` are decoded via `pmix_preg`
  (handling `PMIX_REGEX`, `PMIX_REGEX2`, and plain `PMIX_STRING` forms) and
  turned into per-rank location data by `store_map`; `PMIX_PROC_INFO_ARRAY`
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

- **Job-level data has exactly one home: `internal` under
  `PMIX_RANK_WILDCARD`.** That is true whether the key arrived at the top
  level of the `PMIx_server_register_nspace` info array
  (`cache_job_info`'s else-arm) or inside a `PMIX_JOB_INFO_ARRAY`
  (`pmix_gds_hash_process_job_array`'s else-arm, which mirrors it). The
  two arms are transcriptions of each other; if you add a case to one,
  add it to the other.

  There used to be a second home — a `jobinfo` list on the `pmix_job_t`,
  written only by the job-array arm — and the whole of what follows is
  why it is gone. Do not reintroduce a side list for "the whole job info"
  as a shortcut: it is a second copy of data that is rarely asked for
  whole, and every reader then has to remember to visit both.

  - It made the answer depend on *how the caller asked*. A keyed fetch
    at `PMIX_RANK_WILDCARD` read the table and never the list; a keyed
    fetch at `PMIX_RANK_UNDEF` read the per-rank tables and the list and
    never the job-level table. Neither of the two rank values that mean
    "job level" saw both stores, so a key was reachable one way and
    `PMIX_ERR_NOT_FOUND` the other.
  - It duplicated the job-defining keys. `process_job_array` raised
    `PMIX_HASH_JOB_SIZE` but not `PMIX_HASH_NUM_NODES` or
    `PMIX_HASH_MAX_PROCS`, so `store_map()` computed its own value for a
    key the host had already supplied and stored it in the table — the
    host's value on the list, the computed one in the table, and which
    you got depended on the rank you asked with.
  - It answered more than once. The list walk sat inside the `doover`
    loop, so a hit was collected again for the local table and again for
    the remote one, and `pmix_gds_hash_fetch()` reported success with
    three copies. The client's `process_values()` reads a count above one
    as "an aggregate of everything this proc put", so the application got
    a `PMIX_DATA_ARRAY` of duplicates where it asked for a scalar.

  **The `doover` loop is why the job-level fetch is guarded on
  `ht == &trk->internal`.** The `PMIX_RANK_UNDEF` branch asks the
  job-level table for the key after the per-rank search misses, and the
  loop comes back through that branch for `local` and then `remote`.
  Job data is in neither, and repeating the fetch on each pass is exactly
  the duplicate-answer bug the list had. Regression coverage for all of
  it is `test/unit/get_api.c`.

  **`gds/shmem3` had the same field and the same shape**, and never
  wrote to it at all — it is gone there too, along with the same
  missing `PMIX_RANK_WILDCARD` lookup. If a third module ever appears,
  this is the first thing to check: `src/client/pmix_client_get.c` no
  longer papers over it.

- **`hash` is the fallback terminus.** The framework macros treat a `NULL`
  slot on a `"hash"` module as `PMIX_ERR_NOT_SUPPORTED` rather than
  retrying elsewhere. Since `hash` implements every slot this never fires
  today, but do not remove a slot's implementation assuming "something else
  will pick it up" — for `hash`, nothing will.
- **A delete scope removes rather than stores, and is handled before
  anything reads the value.** `PMIX_DEL_INTERNAL`/`_LOCAL`/`_REMOTE`/
  `_GLOBAL` reach `store` like any other scope, but the kval carries no
  value - so the delete arm sits above the array-expansion cases and the
  verbose line at the top had to stop dereferencing `kv->value->type`.
  It removes from the same tables the storing counterpart writes,
  including the copy of a process's own data kept in `internal`.
  `pmix_hash_remove_data()` translates through the **non-registering**
  key lookup, which is what keeps a delete of a key nobody stored from
  growing the keyindex with that name; "not found" is success, because
  the caller asked for the key to be absent and it is.
- **Scope routing is the contract.** The `INTERNAL`/`REMOTE`/`LOCAL`/`GLOBAL`
  → table mapping in `store` must stay in lockstep with the scope filtering
  in `fetch`; a change to one without the other silently loses or leaks
  data across the local/remote boundary.
- **`internal` doubles as the job-level table.** Job-wide values live under
  `PMIX_RANK_WILDCARD` in `trk->internal`, alongside a proc's own copy of
  its data. Fetches for `rank=WILDCARD` read job-level data; do not assume
  `internal` holds only per-rank entries.
- **What `store_map` derives is a default, and the host always outranks
  it — per rank and per key.** `store_map` works out `PMIX_HOSTNAME`,
  `PMIX_NODEID`, `PMIX_LOCAL_RANK` and `PMIX_NODE_RANK` for every rank out
  of the node and proc maps, and those are assumptions: the nodeid is the
  node's index in the map and the node rank is computed as though this
  were the only job on the node. So a value the host stated itself in a
  `PMIX_PROC_INFO_ARRAY` has to win. Because `cache_job_info` expands
  those arrays onto `trk->internal` during its scan and calls `store_map`
  only after the scan finishes, the check is simply "is this key already
  there for this rank?" — `store_derived()` in `gds_utils.c` asks
  `pmix_hash_fetch` and skips if so. **Do not replace that with a flag.**
  It used to be one: a single `PMIX_PROC_INFO_ARRAY` anywhere in the array
  set `PMIX_HASH_PROC_DATA`, and `store_map` then skipped the nodeid,
  local rank and node rank derivation for the *entire job*. A host that
  described one proc lost those three keys for every other proc, and a
  host that described every proc but named only some of the keys lost the
  rest for all of them — `PMIx_Get` answering `PMIX_ERR_NOT_FOUND` with no
  recovery, since the node-info fallback in `fetch` applies only to
  wildcard ranks. `PMIX_HOSTNAME` sat outside the same gate, so it was
  simultaneously the one key that survived and the one key a host could
  not state without having it overwritten (`pmix_hash_store` replaces a
  differing value). PRRTE supplies the full set for every proc, which is
  why this stayed invisible.
- **Regex decode depends on `preg`, and goes through one decoder.**
  A `PMIX_NODE_MAP` / `PMIX_PROC_MAP` value may arrive as a `PMIX_REGEX`
  byte object, a `PMIX_REGEX2` (which needs `pmix_preg.parse_regex` first),
  or a plain `PMIX_STRING`. `pmix_gds_hash_parse_nodemap()` /
  `parse_procmap()` in `gds_utils.c` handle all three; call those rather
  than reading `value.data.bo.bytes`. The maps nested inside a
  `PMIX_JOB_INFO_ARRAY` used to be decoded separately and only understood
  the byte-object form, so a host that used the regex2 form there had its
  map misread. The node/proc maps are otherwise only as correct as the
  `preg` round-trip; see the `preg` framework doc.
- **A stack `pmix_kval_t` that borrows its value must not be
  `PMIX_DESTRUCT`ed.** Several places here build one as a view of a
  caller's `pmix_info_t` — `kv.key = ...; kv.value = &info[n].value;` — to
  hand to `PMIX_BFROPS_PACK`. Such an object was never constructed, so the
  destructor is reached through an uninitialized `obj_class`, and the kval
  destructor's next act is to free `kv.value`, which points into the middle
  of somebody else's array. Free the key and nothing else.
- **`accept_kvs_resp` returns `PMIX_SUCCESS`, not the unpack code.**
  Running off the end of the payload is how it knows it is finished;
  reporting that to the caller as an error is a different statement. The
  same applies to a `store_modex` callback — see the framework doc.

## Testing

`test/unit/gds_datastore` drives this component (it is the assigned module
on macOS and wherever `shmem3` is unavailable) through the `PMIX_GDS_*`
macros and `PMIx_server_register_nspace`: map decoding in every accepted
form, malformed job-level input, store/fetch scope routing, and the
per-rank/per-key derivation rule above — `test_derived_proc_info()`
registers a job whose proc-info arrays cover only some ranks and name
only some of the keys, and asserts both halves of the rule: what the host
said survives, and everything it did not say is still filled in. The
component's own symbols are not exported, so nothing can call
`pmix_gds_hash_*` directly.
