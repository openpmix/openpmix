<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The GDS `shmem3` Component

`shmem3` is the **shared-memory** `gds` datastore. Instead of keeping job
data in per-process hash tables like [`hash`](../hash/AGENTS.md), the local
PMIx server builds the job's data structures *inside an mmap'd segment* and
lets every local client map that same segment at the same virtual address —
so N local clients share one physical copy of the job data and pay no
per-client unpack/store cost. Clients are meant to treat what they map as
**read-only**, though nothing currently enforces that; see "Clients are
read-only" under the invariants below for what that costs. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific to
`shmem3`.

## The number in the name is load-bearing — do not remove it

**The version suffix is the cross-version compatibility mechanism for this
component, and renaming it is a required step whenever the shared layout
changes.** This is not cosmetic, and it is the single most important thing
to understand before editing anything here.

A client selects its `gds` module **by name**. The server picks the module
and hands the name to each client it forks, in `PMIX_GDS_MODULE`. So a
client from an older release, seeing a name it recognizes, will select this
component and map the segment — whatever the segment now contains.

That matters because this component does not exchange a *wire format*; it
shares **memory layout**. The server builds `pmix_list_t`,
`pmix_hash_table_t` and the rest *inside* the segment, and a client reads
those structures in place. Every one of them derives from `pmix_object_t`.
So a change to the size or layout of `pmix_object_t`, or of anything
derived from it, silently repoints every field an older peer reads. The
observed failure is a segfault in the client, a long way from the change
that caused it.

This is why the component cannot be versioned the way `bfrops` is.
`bfrops` keeps `v12`/`v20`/`v3`/`v4`/`v41` alive side by side because each
packs different *bytes* from the same in-memory types — several can be
linked in at once and chosen per peer. Supporting two generations of this
component simultaneously would instead require two live definitions of
`pmix_object_t`, hence of every class derived from it, cascading into
`bfrops` pack/unpack and the `gds` store/fetch paths. That is a parallel
type system, not a second component.

So the discipline is: **each generation replaces its predecessor.** When
the shared layout changes, bump the number. Older peers do not recognize
the new name, so `hash_assign_module()` — which never disqualifies itself,
returning `PMIX_SUCCESS` at priority 10 whatever was requested — wins for
them, and they fall back to `hash`. That fallback is the whole point, and
it is why the rename must land in the *same* change as the layout it
protects, never after it.

### ...and the name alone is not enough — the segment is stamped

Renaming guards *released* peers, which is all it can do: it works because
an old client does not recognize the new name. It cannot guard two builds
that both call themselves `shmem3` and still lay these structures out
differently. That is not hypothetical — the most common case needs no code
change at all:

**`--enable-debug` changes `pmix_object_t`.** It adds `obj_magic_id` at the
*front* and two fields at the back, so a debug build and a default build of
the very same commit disagree:

| | `--enable-debug` | default |
|---|---|---|
| `sizeof(pmix_object_t)` | 104 | 80 |
| `sizeof(pmix_list_t)` | 248 | 192 |
| `offsetof(pmix_list_t, pmix_list_length)` | 240 | 184 |

A debug server sharing a segment with a default-build client corrupts it
exactly as an old peer would. So does any struct here gaining a field
mid-series without a rename.

Every segment therefore carries `PMIX_GDS_SHMEM3_LAYOUT_ID` in its header,
and `pmix_shmem_segment_attach()` refuses a segment whose stamp differs,
returning `PMIX_ERR_NOT_SUPPORTED`. `shmem3_attach()` turns that into
`PMIX_ERR_TAKE_NEXT_OPTION` — the same fallback the fixed-address failure
already used — so the client quietly uses `hash` instead.

Two properties of that ID matter:

- **It is computed, not maintained.** It folds the `sizeof()` of every type
  that goes in the segment, with distinct prime multipliers so a growth in
  one cannot cancel a shrink in another. A hand-maintained version number is
  one somebody has to remember to bump, and the history of this component is
  that it does not get bumped — that is how the layout change got as far as
  CI. **Extend the list when a new type starts living in the segment**;
  types reached only through a pointer count too.
- **The header holding it must never move.** `pmix_shmem_header_t` is
  fixed-width and free of any class-derived type or `PMIX_ENABLE_DEBUG`
  conditional, on purpose: a stamp that can itself shift cannot detect a
  shift. Keep it that way.

The stamp and the rename cover different halves of the problem and neither
replaces the other: the rename reaches peers built before the stamp existed;
the stamp catches everything that calls itself `shmem3`.

The component has been renamed twice for exactly this reason:

- `shmem` → `shmem2` (Feb 2024), to stop v5.0.0/v5.0.1 attaching a revised
  segment.
- `shmem2` → `shmem3`, when `pmix_object_t` lost its embedded
  `pthread_mutex_t` and shrank from 168 bytes to 104 — which moved
  `pmix_list_length` from offset 368 to 240 and segfaulted every v4.2/v5.0
  peer that mapped the segment.

Note that neither rename kept a compatibility alias, and neither should:
an alias is a name an old client recognizes, which is precisely the thing
that must not happen.

## Files

| File | Contents |
|------|----------|
| `gds_shmem3.h` | Component struct, the job/session/app/nodeinfo trackers, the shared `sm*_data_t` structs that live *in* shared memory, status flags, and segment IDs. |
| `gds_shmem3_component.c` | Component struct + `component_query` (priority **20**) + the two MCA parameter registrations. |
| `gds_shmem3.c` | The module: the TMA shared-memory allocator, segment create/attach, `assign_module`, `register_job_info`/`store_job_info`, modex, and the seg-blob pack/unpack. |
| `gds_shmem3_store.c` | Writing job/modex data into a segment via the TMA (`pmix_gds_shmem3_store_local_job_data_in_shmem3`, `store_qualified`). |
| `gds_shmem3_fetch.c` | Read-side retrieval from the shared structures (`pmix_gds_shmem3_fetch`). |
| `gds_shmem3_utils.c` | Tracker lookup, per-segment handle/status helpers (`get_job_tracker`, `get_job_shmem3_by_id`, `set_status`/`has_status`, verbose dumpers). |

## When it is selected

Two gates must both pass:

1. **Build gate (`configure.m4`).** `shmem3` builds **only** on a 64-bit,
   non-Apple host — it depends on a large virtual address space and the
   `/proc/self/maps`-based virtual-memory-hole finder (`pmix_vmem_find_hole`).
   On macOS, or a 32-bit target, the component is not compiled at all.
2. **Runtime gate (`component_query`).** Even where built, it disqualifies
   itself (`*priority = 0`, returns an error) if `/proc/self/maps` is not
   accessible. Otherwise it bids priority **20**.

Because 20 > `hash`'s 10, `shmem3` is assigned to a peer in preference to
`hash` wherever it is available and the caller did not request a specific
module. `assign_module` bids 20 by default, **100** if the caller named
`PMIX_GDS_MODULE="shmem3"`, and disqualifies itself (**0**) if the caller
named some *other* module.

## The module

`shmem3` fills in most of `pmix_gds_base_module_t` but deliberately leaves
several slots **`NULL`**: `store`, `assemb_kvs_req`, `accept_kvs_resp`
(and, implicitly, `fetch_arrays`). This is intentional — the framework
macros (`PMIX_GDS_STORE_KV`, `PMIX_GDS_ASSEMB_KVS_REQ`,
`PMIX_GDS_ACCEPT_KVS_RESP`) detect the `NULL` slot on a non-`"hash"` module
and route those operations to the local server's own module. `shmem3` only
needs to own the *bulk job/modex data* path; individual `put`/`get` traffic
falls back. `cache_job_info` also returns `PMIX_ERR_NOT_SUPPORTED`: unlike
`hash`, `shmem3` does not pre-cache — it fetches the fully-assembled
job data lazily inside `register_job_info` when the first client connects.
Unlike `hash`, it is `is_tsafe = true`: a fetch holds a reference on the
job tracker and reads only data that is never written again, so it may
run off the progress thread.

## The shared-segment model

The core mechanism is a **bump allocator over shared memory**, the
`pmix_tma_t` ("temporary memory allocator") wired up in `gds_shmem3.c`:

- A job uses up to three segments, identified by
  `pmix_gds_shmem3_job_shmem3_id_t`: **`JOB`** (`smdata` — jobinfo,
  nodeinfo, appinfo lists + a local hash table), **`SESSION`**
  (`session->smdata`), and **`MODEX`** (`smmodex` — the modex hash table).
- **Server (writer).** `shmem3_segment_create_and_attach` pre-sizes a
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
  mapped at. These are bundled into
  `job->conni` and copied into the `register_job_info` reply. The client's
  `store_job_info` → `client_connect_to_shmem3_from_buffi` unpacks each seg
  blob and calls `shmem3_attach`, mapping the segment **at the same fixed
  address** the server used (`PMIX_SHMEM_MUST_MAP_AT_RADDR`). Because the
  address matches, every in-segment pointer resolves correctly with no
  fix-up. Clients never install the TMA function pointers — they only read.

### The allocator keeps its bookkeeping in-band, and does not zero

Two properties of `tma_malloc`/`tma_calloc`/`tma_strdup` that are easy to
undo by accident:

- **Every block carries a 16-byte header** (`pmix_gds_shmem3_tma_alloc_t`:
  the extent plus a magic) immediately ahead of the address handed out.
  The *only* consumer is `tma_realloc()`, which needs the old size to know
  how much to copy — nothing else in the segment is reached by walking
  allocations, so this is not part of the shared layout and does **not**
  belong in `PMIX_GDS_SHMEM3_LAYOUT_ID`. Keep the header a multiple of 8:
  `addr_align()` keeps the bump pointer 8-byte aligned and the payload
  address is the header address plus its size, so an odd-sized header
  misaligns every object in the segment.

  This replaced a side hash table mapping address → extent, which the
  allocator kept on the process heap. It cost **two heap allocations and a
  ptr-keyed hash insert on every allocation**, grew monotonically (`free`
  is a no-op, so nothing was ever removed), and had to be walked entry by
  entry at teardown — all to answer a question `tma_realloc` is the sole
  asker of. Worth knowing *why* that was so expensive: `pmix_hash_store()`
  makes three to five allocations per key/value stored, so the tax landed
  on every byte of every modex. Do not reintroduce out-of-band
  bookkeeping here.

- **Nothing zeroes a block, and nothing needs to.** A bump allocator only
  ever hands out space no caller has touched, and
  `pmix_shmem_segment_create()` opens its backing file `O_CREAT | O_TRUNC`
  and `ftruncate`s it from empty — so every page of a fresh segment reads
  as zero. `tma_calloc()` therefore returns zeroed storage without writing
  a byte, and so, incidentally, does `tma_malloc()`. The `memset` that used
  to be there only forced the whole segment resident up front instead of
  letting demand paging bring in what is actually used, and the segment is
  sized *far* larger than its contents (see `get_modex_sizing_data()`), so
  that was the largest avoidable cost in building one.

  **The `O_TRUNC` is load-bearing**, not tidiness. Segments are unlinked
  when their last holder lets go, so a backing path collides only with a
  file some earlier server left behind when it died — but the path is built
  from the pid, and pids get reused. Without the truncate, `ftruncate()` to
  the same or a smaller size leaves that corpse's bytes in place and only
  the extension past the old end reads as zero. If that guarantee ever
  weakens, restore the `memset` in `tma_carve()` rather than at the call
  sites.

### These hash tables are keyed by rank, not by key

Both `job->smdata->local_hashtab` and `job->smmodex->hashtab` hold **one
element per rank**. `pmix_hash_store()` looks up a single
`pmix_proc_data_t` for the rank and hangs that rank's values off it in a
pointer array; the values are not table elements. Job-level keys all
share the one element belonging to `PMIX_RANK_WILDCARD`.

That makes *two* numbers matter when pre-sizing a segment, and they are
not interchangeable:

| quantity | sizes | where it comes from |
|---|---|---|
| **elements** | the table itself, and the per-rank structures its elements point at | the modex: `job->nspace->nprocs`. The job segment: one per `PMIX_PROC_INFO_ARRAY`, plus one if there is any plain job-level key |
| **key/value pairs** | the stored values, and the key index describing them | the modex: estimated from the blob. The job segment: the infos inside each proc array, plus each plain key |

Both estimates used to feed the *pair* count into the table, so a 32-rank
job built a table with tens of thousands of elements — each of which had
to be zeroed into a fresh mapping before a single value could be stored.
Keep the two apart when you touch `get_modex_sizing_data()` or
`get_local_job_data_info()`; collapsing them is the original bug.

Two smaller traps in the same arithmetic:

- **`pmix_hash_table_init()` takes an element count, not a capacity.** It
  applies the density ratio itself (`est_capacity = n * denom / numer`),
  so passing it `get_actual_hashtab_capacity(n)` — as both paths did —
  doubled the table a second time. Use the raw count for `init()` and
  `get_actual_hashtab_capacity()` only to turn a count into *bytes* for
  the segment estimate.
- **An element costs more than a `pmix_hash_element_t`.** Each one points
  at a `pmix_proc_data_t` with two pointer arrays of its own, and that
  storage is in the segment too. `pmix_hash_sizeof_proc_storage()` in
  `src/util/pmix_hash.c` reports it, because the type is private to that
  file; keep it in step with `pdcon()` there.

### Every segment carries its own key index — for the non-reserved keys

The hash tables in a segment store keys as **integers**, and an integer
in there crosses a process boundary: the server writes it, every local
client reads it. The two ends agree in two different ways, and only one
of them needs anything stored in the segment.

**Reserved attributes agree by construction.** Their ids come from the
generated dictionary, and those ids are pinned in
`contrib/dictionary_ids.txt` (see
[`src/include/AGENTS.md`](../../../include/AGENTS.md)) precisely so that
they are the same in every process and every release. Nothing has to be
carried, so nothing is — `lookup_key()` resolves any id below
`PMIX_INDEX_BOUNDARY` against the process-global index instead. That is
what removed the need for the old dictionary exchange, in which the
server shipped its whole key index in the job-info reply and the client
renumbered its own to match; **do not bring it back**, and see the
Gotchas below.

**Non-reserved keys do not.** A key the Standard does not define is
numbered on first encounter, in an order that depends on what arrived
and when, so no two processes can be assumed to agree. Those are what
the per-segment index holds.

So each segment carries `job->smdata->keyindex` and
`job->smmodex->keyindex`, allocated in the segment through its TMA and
holding the non-reserved keys only. The server mints against them when
it stores, and `pmix_gds_shmem3_fetch()` translates through them when it
reads. A private index numbers from `PMIX_INDEX_BOUNDARY` upward, which
is what makes the id itself say which half it came from.

The reserved half used to live here too. That meant the server allocated
several hundred entries out of the segment — a record and three copies
of the key string apiece — for every generation, to say what the
dictionary already said.

**Clients must reach these only through the non-registering lookups**
(`pmix_hash_find_key()` and `pmix_hash_lookup_key()` with a known
index). Registering would mean a reader writing into the segment.

**Ask the code that spends the memory how much it spends.** The estimate
is a shadow model of what storing will cost, and every time it has been
wrong it was because it restated something it should have asked for.
Four sizing entry points exist for exactly this, each living beside the
code it describes so the two cannot drift:

| ask | for | lives beside |
|---|---|---|
| `pmix_hash_table_sizeof_storage(n)` | the element array `init(ht, n)` allocates | `pmix_hash_table_init()` |
| `pmix_hash_sizeof_proc_storage()` | one rank's per-proc object and its arrays | `pdcon()` |
| `pmix_keyindex_sizeof_fixed_storage()` | an *empty* key index (~170 KB) | `keyindex_construct()` |
| `pmix_hash_sizeof_key_entry(len)` | one newly registered key | `lookup_key()` |

Do not open-code any of these here. The estimate previously reserved
`3 * (PMIX_MAX_KEYLEN + 1)` per assumed key/value pair — three copies of
a *511-byte* key — which came to roughly fifty times the payload and was
single-handedly the largest term in the calculation. Real keys run tens
of bytes, and their true bound is the data: every key that will be
registered arrived in the buffer being packed or unpacked, so the key
strings cannot total more than that buffer holds. Bound them by the
payload, never by the maximum.

**And whatever you put in a segment has to be in that segment's size
estimate before it is put there.** A keyindex is not small — it
constructs a 2048-slot lookup table and a 1024-slot pointer array
regardless of how many keys it ends up holding, plus three copies of
every key string. Adding one to the job segment without extending the
estimate in `prepare_shmem3_stores_for_local_job_data()` overran the
bump allocator, so the server aborted partway through
`register_job_info()` and every client waited forever for job data that
was never coming — which presents as a hang, not a crash, and not on
the process that actually failed. The fixed capacities are named
(`PMIX_KEYINDEX_LOOKUP_SIZE`, `PMIX_KEYINDEX_TABLE_SIZE` in
`src/include/pmix_globals.h`) precisely so the constructor and the two
estimates cannot drift apart; use them rather than repeating the
numbers.

The job segment did not always have one: its indices were minted against
the global keyindex on the server and the client was handed the server's
copy to merge into its own
(`client_update_global_keyindex_if_necessary()`). That worked, but it
made reads of shared data depend on a process-global structure — and the
merge itself does a `PMIX_DESTRUCT` and rebuild of it. **That machinery
is now redundant** and should be removed once someone confirms nothing
else leans on the client and server agreeing about global indices;
until then it runs harmlessly at `store_job_info` time, before an
application has threads.

### The `/proc/self/maps` scan is not the expensive part — measured

`pmix_vmem_find_hole()` reads and parses all of `/proc/self/maps` on
every segment creation, which looks like an obvious thing to cache: one
scan per job segment, per session segment, and per modex generation.
It is not worth caching, and this is recorded so nobody re-derives the
suspicion and pays for the fix.

Measured cost is about **0.17 µs per VMA**, essentially linear:

| VMAs | scan |
|---|---|
| 16 | 5 µs |
| 516 | 91 µs |
| 2016 | 345 µs |
| 8016 | 1.3 ms |

A PRRTE daemon in `contrib/dockerswarm` carries **72** VMAs, so the scan
costs it roughly 15 µs — under 1% of the per-segment cost. Caching a
hole would trade that for a stale-address failure mode on a path whose
whole correctness rests on client and server agreeing about an address.

Reconsider only where the VMA count is genuinely large — a DSO-heavy
build with several fabric providers and a GPU runtime could plausibly
reach the high hundreds, and the table above says what that would cost.
Measure the daemon before assuming it.

### The fixed-address attach failure and GDS fallback

A client may be unable to map the segment at the server's chosen address —
its own VM layout (ASLR, what is already mapped) differs. `shmem3_attach`
translates that specific failure into **`PMIX_ERR_TAKE_NEXT_OPTION`**,
which propagates up unchanged through `client_connect_to_shmem3_from_buffi`
and triggers the framework's **GDS fallback** (`fallback_to_next_gds` in
`src/client/pmix_client.c`): the client switches to the next module
(`hash`) via `peer->gds` and re-requests its job data with
`PMIX_GDS_FALLBACK_CMD`. This is why a failed attach is *not* fatal to
`PMIx_Init`. The `force_client_attach_failure` MCA parameter exists solely
to exercise this path in tests without depending on each process's VM
layout — never set it in production.

## MCA parameters (registered in `gds_shmem3_component.c`)

| Parameter | Type | Meaning |
|-----------|------|---------|
| `gds_shmem3_segment_size_multiplier` | double (default `1.0`) | scales the computed segment sizes; raise it if a job overflows a segment (which aborts). |
| `gds_shmem3_force_client_attach_failure` | bool (default `false`) | **testing only** — force the client's fixed-address attach to fail so the GDS fallback can be exercised. |

These are the *only* `gds` MCA parameters in the tree; the framework core
registers none.

## Modex

Modex data gets its own segment. `server_store_modex` delegates to the base
envelope walker (`pmix_gds_base_store_modex`) with a callback that writes
each proc's kvals into the modex hash table in shared memory;
`server_mark_modex_complete` packs the modex seg blob for local peers, and
the client's `recv_modex_complete` attaches that segment the same way it
attached the job segment.

**Each modex gets a *new* segment — one per generation.** A finished
segment has been advertised and local clients have it mapped, so it is
never written again. `server_store_modex_cb()` treats a blob arriving for
a modex already flagged `READY_FOR_USE` as the start of the next one:
it hands the current segment off, bumps `job->modex_generation`, and
creates a fresh segment named after the generation (the backing path is
built from the nspace, pid and name, so the name has to differ or they
collide).

Handing off is safe because the backing file is reference counted —
dropping the server's handle leaves a client that still has it mapped
with a valid mapping, and the file survives until the last holder lets
go (`shmem_destruct` in `src/util/pmix_shmem.c`).

The client tells generations apart by the **backing path**, which the
seg blob already carries, so this is not a wire-format change and needs
no version gate. An older client short-circuits on `ATTACHED` and keeps
reading the generation it has: stale, but a consistent snapshot.

This replaced writing every modex into the first one's segment, which
was not a latent problem. A segment is sized from the payload that
created it, and the allocator behind it is a bump allocator, so a larger
second modex overran it and **aborted the server** — taking the daemon,
and the job, with it. `examples/modex_twice.c` reproduces that from four
nodes up.

**Whether the finished generation can be dropped depends on what is
arriving**, and the flag byte in the modex envelope is what says so (see
openpmix#4087 and `docs/how-things-work/modex.rst`).

A **cumulative** contribution repeats everything its processes have
published, so the generation carrying it stands on its own: the previous
one is released exactly as before, and every retired one behind it goes
with it (`drop_modex_priors`). That is still the default, because
`pmix_server_fence_delta_modex` defaults off.

A **delta** contribution repeats nothing, so the previous generation is
still the only copy of everything the delta left out. It is *retired*
onto `job->modex_prior` rather than released (`retire_modex_segment`),
and `modex_fetch()` in `gds_shmem3_fetch.c` walks that list newest-first.
A keyed lookup stops at the newest generation holding the key; a
NULL-key lookup consults every generation and drops a copy of a key a
newer one already supplied, or the caller sees it twice with the stale
value second.

`job->modex_prior` is empty unless a delta has been stored, so the
ordinary case is the single lookup it has always been. The chain grows
only while deltas keep arriving and collapses on the next cumulative
contribution - which the participant-set guard on the server side forces
whenever a fence's membership changes.

The client makes the same keep-or-drop decision, from a
`SHMEM3_SEG_DELTA_KEY` field in the segment blob. Adding a field there
was possible because this component has never been in a release; once it
ships, that blob is a wire format like any other.

`examples/modex_twice.c` is the canary, and
`contrib/dockerswarm/run-gds-tests.sh` drives it **twice**: once
cumulatively and once with `pmix_server_fence_delta_modex=1`. Only the
second actually tests the chain - its `gen1` keys are published before
the first fence and never again, so under a delta they exist *only* in
the retired generation.

## Deletion: a tombstone, and why it is not in the segment

This component cannot take a key out. The data is in a segment local
clients have mapped, and a segment a client can see is never written
again — so `del_key` records that the key is gone and every read
consults the record.

**The record is process-local**, on `job->tombstones`. Putting it in
shared memory would mean a new segment, mapped by every local client,
for a few bytes per deleted key — and it would not save the step that
matters, because a client attaching after the removal has to be told
either way. Each process builds its own from the notification its server
sends (`pmix_server_notify_deleted`), and `pack_tombstones()` adds the
list to the cached job-info reply so a later arrival gets it at attach
time. Note `del_key()` drops `job->conni` for exactly that reason: the
cached reply still says the key exists.

**A tombstone carries the modex generation it was recorded at.** Job
data is written once and never re-published, so `job_fetch()` asks with
`UINT32_MAX` and a tombstone against it always applies. Modex data can
legitimately come back, so `modex_fetch()` asks with each generation's
own number as it walks, and a tombstone shadows only generations up to
the one it was made at — a key deleted and then published again in a
later fence is alive again.

Two shapes of read, two filters. A keyed lookup that finds only
tombstoned entries in a generation keeps walking rather than reporting a
hit; a NULL-key lookup — "everything this process published" — has its
result filtered per generation, bounded by the `mark` so entries from
newer generations are judged against theirs and not this one's.

`gds/hash` leaves `del_key` NULL: a delete reaches its store like any
other scope and it removes the key outright. The macro reads a NULL slot
as success.

## Gotchas

- **Never `PMIX_CONSTRUCT` data destined for shared memory.** As the file's
  own developer note says, `PMIX_CONSTRUCT` records the constructing
  process's stack address into the object; shared objects must be built
  through the TMA (`PMIX_NEW(type, tma)`) so their addresses live in the
  segment. This is the single easiest way to corrupt the store.
- **Clients are read-only, and the MMU now enforces it.** The server
  writes; a client drops write access to the data region as the last
  step of attaching (`pmix_shmem_segment_protect_data()`), so a
  client-side write is a SIGSEGV on the instruction that did it rather
  than corruption some other process trips over later. Do not add a
  client-side write path — it will not silently work.

  The internal header stays writable on purpose: it holds the reference
  count that attach and detach maintain, and a reader that could not
  write it could not let go of the segment. That is why the protection
  starts at the data region, which begins a page-aligned header into the
  mapping — the geometry lives in `pmix_shmem.c` rather than at the call
  site so nobody has to rederive it.

  Getting here took removing two writes a reader was making. A
  hash-table lookup used to record the table's key type, stamping one
  process's address of a `static const` into memory the others read; and
  the job segment's indices used to be minted against the process-global
  keyindex, which the client rewrote to match the server's. Both are
  gone. If you enable this on a path that still writes, you will find
  out immediately, which is the point.
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
- **`smdata` is NULL until its segment exists.** A job tracker is created
  by `server_add_nspace()` at `PMIx_server_register_nspace` time, long
  before `register_job_info()` builds the job segment or a client maps it
  — and a session object exists from `job_construct()` with no session
  data at all. Anything reaching `job->smdata->…` or
  `job->session->smdata->…` has to establish it is there first; a fetch
  arriving in that window is `PMIX_ERR_NOT_FOUND`, not a fault. Note that
  `pmix_gds_shmem3_get_session_tma()` forming `&NULL->tma` is not a fault
  either — it just hands the bad pointer onward.
- **The modex half of this component went live in `9d9842e5`** — before
  that the three modex macros resolved `pmix_globals.mypeer` (pinned to
  `"hash"` on both server and client) instead of the peer they were
  given, so it was written but never reached. Anything you read claiming
  this code is dormant predates that commit. It remains the
  least-exercised code here; `contrib/dockerswarm/run-gds-tests.sh` is
  what covers it.
- **`server_store_modex_cb()` must return `PMIX_SUCCESS` for a proc blob
  it consumed.** Its natural exit is the
  unpack end-of-buffer code, and the base envelope walker reads any
  non-success return as a failure of the whole server contribution: it
  stops, and then converts the same code to success for its own caller.
  Returning it would store the first proc of each contribution and
  discard the rest, reporting success. See
  [`../base/AGENTS.md`](../base/AGENTS.md).
- **There is no key-index blob on the wire any more, and do not bring
  one back.** The server used to ship its whole dictionary in the
  job-info reply so the client could renumber its own to match
  (`pack_server_keyindex_info()` / `unpack_srv_kindx_info()` /
  `client_update_global_keyindex_if_necessary()`, about 600 lines). It
  existed because the indices in a segment were minted against the
  *server's global* keyindex, which the client had no reason to agree
  with. Each segment carrying its own index removes the requirement
  rather than satisfying it, so all of that is gone - along with a
  parser that had to be told how many elements it had actually filled,
  and a merge that destructed and rebuilt `pmix_globals.keyindex`
  wholesale. If you find yourself needing to reconcile two dictionaries
  across a process boundary, put the index next to the data instead.

## Testing

`shmem3` gets **no coverage at all on macOS** — `configure.m4` gates it on
a 64-bit, non-Apple host, with no `|| test "$pmix_testbuild" = "1"` escape,
so it is not even compiled there. A change made on a Mac has not been
compiled until it has been built on Linux.
`contrib/dockerswarm/run-gds-tests.sh` is the suite that does that and then
exercises it: server and clients on `shmem3`, a client forced onto the
fallback path with `PMIX_MCA_gds_shmem3_force_client_attach_failure`, and
cross-node fetches that reach the modex.

`test/unit/gds_datastore` covers the *framework* contracts this component
has to honor — notably the modex callback contract above — but runs against
whichever module is assigned, which on a developer's Mac is `hash`.
