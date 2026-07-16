<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The `src/hwloc` Topology Layer

This document orients AI agents and human contributors working in
`src/hwloc`. It assumes you have already read the top-level
[`AGENTS.md`](../../AGENTS.md) — the golden rules (prefix conventions,
`pmix_config.h`-first include order, constant-on-the-left comparisons,
brace-everything, `#if` not `#ifdef` for logical macros, warning-free
under `--enable-devel-check`), the copyright-header requirement, the
**Version Interoperability / wire-format stability** rules, and the
**Backward Compatibility** rules (deprecated symbols live forever). All
of that applies here and is not repeated. This file covers what is
specific to `src/hwloc`: what this layer is, the topology-provenance
model it is built around, how the shared-memory / XML topology handoff
works, and the recurring pitfalls unique to wrapping hwloc.

## What this directory is

`src/hwloc` is **not** an MCA framework. There is no framework header, no
components, no selection logic — just three files compiled straight into
`libpmix` via [`Makefile.include`](Makefile.include) (the `sources +=` /
`headers +=` lists):

| File | Contents |
|------|----------|
| `pmix_hwloc.h` | the public-to-the-library API (all `pmix_hwloc_*` prototypes), plus four `pmix_ploc_base_*` ABI-preservation shims |
| `pmix_hwloc.c` | topology acquisition/sharing, cpuset & locality computation, device-distance computation, MCA params, finalize |
| `pmix_hwloc_datatype.c` | the `bfrops` datatype handlers for `pmix_cpuset_t` and `pmix_topology_t` (pack/unpack/copy/print/destruct/release/size) |

### Historical note: this *was* the `ploc` framework

Everything here used to be an MCA framework named `ploc` (PMIx LOCality)
with an `hwloc` component. That framework was collapsed into this plain
directory — there is **no `src/mca/ploc/` any more**. Three artifacts of
that history survive and you must respect them:

1. **`help-ploc.txt`** — the `show_help` file keeps the old name. It is
   referenced by string literal (`pmix_show_help("help-ploc.txt", …)`) in
   `pmix_hwloc.c`. Per the top-level golden rule, if you touch this file
   you must `rm src/util/pmix_show_help_content.*` and rebuild so the
   compiled-in help content is regenerated.
2. **The `pmix_ploc_base_*` shims** at the bottom of `pmix_hwloc.h` /
   `pmix_hwloc_datatype.c` (`_destruct_cpuset`, `_release_cpuset`,
   `_destruct_topology`, `_release_topology`). They exist **only** to
   preserve the exported symbol names (the `/**** PRESERVE ABI ****/`
   block). Each is a one-line forwarder to its `pmix_hwloc_*` twin. Do
   not delete them and do not add logic to them.
3. MCA parameter names are still registered under the `hwloc` component
   (`pmix_hwloc_verbose`, `..._hole_kind`, `..._topo_file`,
   `..._test_cpuset`), reachable as e.g. `PMIX_MCA_hwloc_hole_kind`.

## The single most important concept: the `source` string

Both public objects carry a provenance string:

```c
typedef struct { char *source; void *bitmap;   } pmix_cpuset_t;
typedef struct { char *source; void *topology;  } pmix_topology_t;
```

`source` names *which locality provider produced this object*. This layer
only ever answers for hwloc, so **every entry point begins by checking
`source`** and returns `PMIX_ERR_TAKE_NEXT_OPTION` when the object is not
ours:

```c
if (0 != strncasecmp(cpuset->source, "hwloc", 5)) {
    return PMIX_ERR_TAKE_NEXT_OPTION;
}
```

This is the vestige of the multi-component `ploc` design: the caller
walks providers until one claims the object. Today hwloc is the only
provider, but the contract remains — **preserve it in any new entry
point**. Two source spellings appear on the wire and must both be
matched by the 5-char `strncasecmp(..., "hwloc", 5)` test:

- `"hwloc"` — bare, used when the version is unknown (XML-adopted,
  externally-provided, unpacked).
- `"hwloc:<HWLOC_VERSION>"` — versioned, used for self-discovered and
  shmem-adopted topologies (`pmix_asprintf(&source, "hwloc:%s",
  HWLOC_VERSION)`).

`pmix_hwloc_generate_cpuset_string` and the locality strings prepend the
`hwloc:` tag to the string form so the parse/relative-locality routines
can re-check provenance from the string alone (`"hwloc:0-3"`, then
locality tokens like `"NM0:SK0:CR2:HT5"`).

## Topology provenance model (`pmix_globals.topology`)

There is one process-wide cached topology: `pmix_globals.topology` (a
`pmix_topology_t`), guarded by `pmix_globals.external_topology`. The
whole point of `pmix_hwloc_setup_topology()` (server/tool startup) and
`pmix_hwloc_load_topology()` (client, and lazy fallback) is to populate
that cache from the **cheapest available source**, in this priority
order:

1. **Externally provided** (`PMIX_TOPOLOGY2`, or deprecated
   `PMIX_TOPOLOGY`, passed as an info to setup). Sets
   `external_topology = true`; the topology pointer is *borrowed*, not
   owned — `pmix_hwloc_finalize()` must **not** destroy it.
2. **hwloc shared-memory segment** (`PMIX_HWLOC_SHMEM_FILE` / `_ADDR` /
   `_SIZE` fetched from the server via GDS, then
   `hwloc_shmem_topology_adopt`). Sets `topo_in_shmem = true`. This is
   the "instant on"/no-copy path: every client mmaps the *same* physical
   pages the server wrote.
3. **XML string** — `PMIX_HWLOC_XML_V2`, then `PMIX_HWLOC_XML_V1`
   (`hwloc_topology_set_xmlbuffer` + load). The v1 fallback exists to
   talk to peers built against hwloc 1.x.
4. **MCA `topo_file`** — an XML file named by `PMIX_MCA_hwloc_topo_file`
   (testing).
5. **Self-discovery** — `hwloc_topology_init` + `set_flags` +
   `hwloc_topology_load`.

`pmix_hwloc_setup_topology()` is guarded by the `passed_thru` static so
it runs **exactly once** per process. After acquiring the topology it
optionally *shares* it: exports v2 + v1 XML into
`pmix_server_globals.gdata` (so clients can fetch it) and, unless
`hole_kind == VMEM_HOLE_NONE`, writes it to a shmem segment in the
session dir and advertises the file/addr/size keys. All of the
`PMIX_HWLOC_*`/`PMIX_LOCAL_TOPO` keys it publishes are **deprecated keys
kept for older RMs** — see [`pmix_deprecated.h`](../../include/pmix_deprecated.h);
they are intentionally still emitted.

### Shared memory and the VM hole

Adopting a topology at a fixed virtual address requires the server and
every client to have the *same* address range free. `set_flags` +
`pmix_vmem_find_hole(hole_kind, …)` locate that range; `hole_kind` is set
from the `PMIX_MCA_hwloc_hole_kind` param (`none|begin|biggest|libs|heap|
stack`, default `biggest`). If a hole can't be found or the backing store
lacks space (`enough_space()`), the code **degrades gracefully to XML** —
none of these are fatal. The two `help-ploc.txt` topics (`target full`,
`sys call fail`) explain the shmem-disable escape hatch
(`PMIX_MCA_gds=hash`) to users.

## The datatype handlers (`pmix_hwloc_datatype.c`)

These are registered into `bfrops` for `PMIX_PROC_CPUSET` (`pmix_cpuset_t`)
and `PMIX_TOPO` (`pmix_topology_t`); see
[`src/mca/bfrops`](../mca/bfrops/AGENTS.md). They are reached through the
`bfrops` driver and through the `data_array` destruct path in
`bfrop_base_tma.h`. Key facts:

- **cpuset is packed as its `hwloc_bitmap_list_asprintf` string.** A NULL
  bitmap (unbound process) packs as a NULL string. Symmetric on unpack.
- **topology is packed as an XML string, followed by three raw
  `hwloc_topology_support` sub-structs** (`discovery`, `cpubind`,
  `membind`) as `PMIX_BYTE` blobs — because hwloc's XML export omits the
  support flags. **This is a wire-format fragility** (see Pitfalls): the
  blob length is `sizeof(struct hwloc_topology_*_support)`, an hwloc-ABI
  quantity, not a fixed protocol width.
- **`destruct` frees the innards; `release(ptr, sz)` loops destruct over
  an array and then frees the array block itself.** Both
  `pmix_hwloc_release_cpuset` and `pmix_hwloc_release_topology` end with a
  `free()` of the passed array — keep them symmetric.
- **`copy` uses `hwloc_topology_dup` / `hwloc_bitmap_dup`.** A duplicated
  topology is a full independent copy that its owner must
  `hwloc_topology_destroy`.

## Callers (who depends on this layer)

- **`src/server/pmix_server.c` / `pmix_server_ops.c`** — setup at server
  init; locality-string generation for clients; cpuset parse; distances.
- **`src/client/pmix_client.c` / `pmix_client_topology.c`** — load on
  demand; `PMIx_Get_cpuset`, `PMIx_Get_relative_locality`,
  `PMIx_Compute_distances`, `PMIx_Load_topology`, `PMIx_Parse_cpuset_string`.
- **`src/tool/pmix_tool.c`** — setup at tool init.
- **`src/mca/bfrops/base`** — the datatype handlers, via the registered
  type table and the `data_array` free path (`bfrop_base_tma.h`).
- **`src/mca/pgpu/{amd,intel,nvd}` and `pnet/{nvd,opa}`** — call
  `pmix_hwloc_check_vendor(topo, vendorID, class)` to detect vendor PCI
  devices in the topology.

## Threading

These functions are **synchronous transforms**, not thread-shifting
entry points — there is no caddy pattern here. They read and write
`pmix_globals.topology` (and, in setup, `pmix_server_globals.gdata`)
directly, on the assumption that they are called from the setup/teardown
path or from a progress-thread handler that already owns that state. Do
**not** call the acquisition routines (`setup`/`load`) concurrently; the
`passed_thru`/`external_topology` guards are plain booleans with no
locking. The pure computational helpers (`generate_locality_string`,
`get_relative_locality`, `compute_distances`, the datatype handlers)
operate only on their arguments and are safe to call from any thread that
owns those arguments.

## Building

All three files are statically compiled into `libpmix`. There is **no
`configure.m4`** in this directory; whether hwloc is available at all is
decided by [`config/pmix_setup_hwloc.m4`](../../config/pmix_setup_hwloc.m4),
which enforces a **minimum hwloc of 2.1.0** (the code assumes 2.x APIs
throughout — `hwloc_shmem_*`, component blacklisting, `osdev.type`, etc.,
with no version `#if` guards). Editing `Makefile.include` only needs a
plain `make`. Touching `help-ploc.txt` requires the
`rm src/util/pmix_show_help_content.* && make` dance (top-level golden
rule). Because the environment always has hwloc on the dev machines,
`--enable-test-build` is not needed to compile-check this directory, but
a normal build does exercise it.

## Pitfalls specific to this directory

- **Always check `source` first, and match it with `strncasecmp(...,
  "hwloc", 5)`.** Returning `PMIX_ERR_TAKE_NEXT_OPTION` (not an error) for
  a non-hwloc object is the contract. A `NULL` source is legal on *input*
  cpusets (means "fill me in") but not on objects you are asked to
  serialize/print.
- **Borrowed vs. owned topology.** If `external_topology` is set, the
  topology belongs to the caller — never `hwloc_topology_destroy` it. The
  self-discovered / XML / shmem-adopted topology is owned by the library
  and destroyed in `pmix_hwloc_finalize()`. `source` is heap-allocated
  for the owned cache and must be freed there; the static
  `"hwloc:" HWLOC_VERSION` string handed back by `pmix_hwloc_load_topology`
  to callers who did not specify a source is **read-only — callers must
  not free it** (see the long comment on that function).
- **Freeing hwloc objects with hwloc, PMIx allocations with PMIx.** XML
  buffers from `hwloc_topology_export_xmlbuffer` must be released with
  `hwloc_free_xmlbuffer` (not `free`); bitmaps with `hwloc_bitmap_free`;
  topologies with `hwloc_topology_destroy`. Strings built with
  `pmix_asprintf` use `free`.
- **The support-struct blob is not portable across hwloc ABIs.** Packing
  `sizeof(struct hwloc_topology_*_support)` raw bytes means two peers
  built against hwloc releases with different struct layouts can
  mis-unpack. This is inherent to how the support flags are shipped;
  don't "optimize" the XML/blob split without understanding the
  cross-version consequences described in the top-level interoperability
  rules.
- **`hwloc_topology_set_xmlbuffer` wants the NUL-inclusive length.**
  hwloc's `export_xmlbuffer` reports (and `set_xmlbuffer` expects) a
  length that *includes* the terminating NUL, so always pass
  `strlen(xml) + 1` — both `load_xml` and `pmix_hwloc_unpack_topology` do.
- **Device distances depend on the cpuset covering something below the
  machine.** `compute_distances` returns `PMIX_ERR_NOT_AVAILABLE` when
  the only object covering the cpuset is the whole machine (common in
  odd containers), and `PMIX_ERR_NOT_FOUND` when no matching devices
  exist — neither is a hard failure.

## Auditing history (recently fixed — kept as landmarks)

The following defects were found and fixed while first auditing this
directory. They are recorded so the reasoning behind the current code is
not lost and so a future refactor does not silently reintroduce them:

1. **Array leak in `pmix_hwloc_release_topology`** — it destructed each
   element but did not free the array block, unlike its
   `pmix_hwloc_release_cpuset` sibling. Now it does; keep them symmetric.
2. **Container/source leak in `pmix_hwloc_load_topology`** — the
   "found in storage" branch took ownership of the `popptr()` container
   but never freed the emptied struct or its source. It now adopts the
   source directly and frees the container.
3. **Unguarded `strrchr` in `enough_space`** — the parent-directory
   computation dereferenced the `strrchr` result without a NULL check;
   it now returns `PMIX_ERR_BAD_PARAM` when no separator is present.
4. **`pmix_hwloc_get_topology_size` did no validation** — it now guards a
   NULL `ptr`/`ptr->topology` and applies the same `source` check the
   other datatype handlers use.

If you find a genuine bug here, fix it in the source as a standalone,
signed-off commit per the contribution rules — never bend a test to make
buggy behavior pass.
