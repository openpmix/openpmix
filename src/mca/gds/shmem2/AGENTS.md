<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The GDS `shmem2` Component

`shmem2` is the **shared-memory** `gds` datastore. Instead of keeping job
data in per-process hash tables like [`hash`](../hash/AGENTS.md), the local
PMIx server builds the job's data structures *inside an mmap'd segment* and
lets every local client map that same segment **read-only** at the same
virtual address — so N local clients share one physical copy of the job
data and pay no per-client unpack/store cost. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific to
`shmem2`. (The `2` distinguishes it from an earlier shared-memory design;
it is the current shared-memory component.)

## Files

| File | Contents |
|------|----------|
| `gds_shmem2.h` | Component struct, the job/session/app/nodeinfo trackers, the shared `sm*_data_t` structs that live *in* shared memory, status flags, and segment IDs. |
| `gds_shmem2_component.c` | Component struct + `component_query` (priority **20**) + the two MCA parameter registrations. |
| `gds_shmem2.c` | The module: the TMA shared-memory allocator, segment create/attach, `assign_module`, `register_job_info`/`store_job_info`, modex, and the seg-blob pack/unpack. |
| `gds_shmem2_store.c` | Writing job/modex data into a segment via the TMA (`pmix_gds_shmem2_store_local_job_data_in_shmem2`, `store_qualified`). |
| `gds_shmem2_fetch.c` | Read-side retrieval from the shared structures (`pmix_gds_shmem2_fetch`). |
| `gds_shmem2_utils.c` | Tracker lookup, per-segment handle/status helpers (`get_job_tracker`, `get_job_shmem2_by_id`, `set_status`/`has_status`, verbose dumpers). |

## When it is selected

Two gates must both pass:

1. **Build gate (`configure.m4`).** `shmem2` builds **only** on a 64-bit,
   non-Apple host — it depends on a large virtual address space and the
   `/proc/self/maps`-based virtual-memory-hole finder (`pmix_vmem_find_hole`).
   On macOS, or a 32-bit target, the component is not compiled at all.
2. **Runtime gate (`component_query`).** Even where built, it disqualifies
   itself (`*priority = 0`, returns an error) if `/proc/self/maps` is not
   accessible. Otherwise it bids priority **20**.

Because 20 > `hash`'s 10, `shmem2` is assigned to a peer in preference to
`hash` wherever it is available and the caller did not request a specific
module. `assign_module` bids 20 by default, **100** if the caller named
`PMIX_GDS_MODULE="shmem2"`, and disqualifies itself (**0**) if the caller
named some *other* module.

## The module

`shmem2` fills in most of `pmix_gds_base_module_t` but deliberately leaves
several slots **`NULL`**: `store`, `assemb_kvs_req`, `accept_kvs_resp`
(and, implicitly, `fetch_arrays`). This is intentional — the framework
macros (`PMIX_GDS_STORE_KV`, `PMIX_GDS_ASSEMB_KVS_REQ`,
`PMIX_GDS_ACCEPT_KVS_RESP`) detect the `NULL` slot on a non-`"hash"` module
and route those operations to the local server's own module. `shmem2` only
needs to own the *bulk job/modex data* path; individual `put`/`get` traffic
falls back. `cache_job_info` also returns `PMIX_ERR_NOT_SUPPORTED`: unlike
`hash`, `shmem2` does not pre-cache — it fetches the fully-assembled
job data lazily inside `register_job_info` when the first client connects.
Like `hash`, it is `is_tsafe = false`.

## The shared-segment model

The core mechanism is a **bump allocator over shared memory**, the
`pmix_tma_t` ("temporary memory allocator") wired up in `gds_shmem2.c`:

- A job uses up to three segments, identified by
  `pmix_gds_shmem2_job_shmem2_id_t`: **`JOB`** (`smdata` — jobinfo,
  nodeinfo, appinfo lists + a local hash table), **`SESSION`**
  (`session->smdata`), and **`MODEX`** (`smmodex` — the modex hash table).
- **Server (writer).** `shmem2_segment_create_and_attach` pre-sizes a
  segment (data size × empirical "fluff" × the `segment_size_multiplier`
  MCA param), creates a backing file under the session/nspace tmpdir,
  finds a free virtual-address hole with `pmix_vmem_find_hole`, and mmaps
  the segment there with `PMIX_SHMEM_MUST_MAP_AT_RADDR`. It retries on a
  lost hole (another thread, `dlopen`, the allocator, or ASan can steal the
  located address between find and map — up to `MAX_ATTACH_ATTEMPTS`
  times). All PMIx list/hash-table structures for the job are then
  allocated *inside* the segment through the TMA, so the pointers they
  contain are valid at the chosen base address. The TMA `free` is a no-op
  (it is a bump allocator); overflowing a segment `abort()`s with a
  guidance message — size segments generously.
- **Client (reader).** The server packs, per segment, a *seg blob*
  containing the backing-file path, size, and the **header address** it was
  mapped at, plus the server's key-index blob. These are bundled into
  `job->conni` and copied into the `register_job_info` reply. The client's
  `store_job_info` → `client_connect_to_shmem2_from_buffi` unpacks each seg
  blob and calls `shmem2_attach`, mapping the segment **at the same fixed
  address** the server used (`PMIX_SHMEM_MUST_MAP_AT_RADDR`). Because the
  address matches, every in-segment pointer resolves correctly with no
  fix-up. Clients never install the TMA function pointers — they only read.

### The fixed-address attach failure and GDS fallback

A client may be unable to map the segment at the server's chosen address —
its own VM layout (ASLR, what is already mapped) differs. `shmem2_attach`
translates that specific failure into **`PMIX_ERR_TAKE_NEXT_OPTION`**,
which propagates up unchanged through `client_connect_to_shmem2_from_buffi`
and triggers the framework's **GDS fallback** (`fallback_to_next_gds` in
`src/client/pmix_client.c`): the client switches to the next module
(`hash`) via `peer->gds` and re-requests its job data with
`PMIX_GDS_FALLBACK_CMD`. This is why a failed attach is *not* fatal to
`PMIx_Init`. The `force_client_attach_failure` MCA parameter exists solely
to exercise this path in tests without depending on each process's VM
layout — never set it in production.

## MCA parameters (registered in `gds_shmem2_component.c`)

| Parameter | Type | Meaning |
|-----------|------|---------|
| `gds_shmem2_segment_size_multiplier` | double (default `1.0`) | scales the computed segment sizes; raise it if a job overflows a segment (which aborts). |
| `gds_shmem2_force_client_attach_failure` | bool (default `false`) | **testing only** — force the client's fixed-address attach to fail so the GDS fallback can be exercised. |

These are the *only* `gds` MCA parameters in the tree; the framework core
registers none.

## Modex

Modex data gets its own segment. `server_store_modex` delegates to the base
envelope walker (`pmix_gds_base_store_modex`) with a callback that writes
each proc's kvals into the modex hash table in shared memory;
`server_mark_modex_complete` packs the modex seg blob for local peers, and
the client's `recv_modex_complete` attaches that segment the same way it
attached the job segment.

## Gotchas

- **Never `PMIX_CONSTRUCT` data destined for shared memory.** As the file's
  own developer note says, `PMIX_CONSTRUCT` records the constructing
  process's stack address into the object; shared objects must be built
  through the TMA (`PMIX_NEW(type, tma)`) so their addresses live in the
  segment. This is the single easiest way to corrupt the store.
- **Clients are read-only.** The server writes; clients map read-only and
  must not mutate the segment. Do not add a client-side write path.
- **The fixed-address contract is load-bearing.** In-segment pointers are
  only valid because client and server map at the *same* virtual address.
  Anything that changes how the address is chosen, packed, or reattached
  must keep both ends in agreement, and must preserve the
  `PMIX_ERR_TAKE_NEXT_OPTION` fallback for the address-mismatch case.
- **Size segments generously.** Overflow `abort()`s the process; the
  pre-sizing math plus `segment_size_multiplier` is the only guard. Prefer
  raising the multiplier over shaving the estimates.
- **`store`/`assemb_kvs_req`/`accept_kvs_resp` are `NULL` on purpose.**
  They rely on the framework macros' fallback to the local module; do not
  "complete the interface" by pointing them at half-implemented functions.
