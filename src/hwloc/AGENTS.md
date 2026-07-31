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

`pmix_hwloc_generate_cpuset_string` prepends the `hwloc:` tag to its string
form (`"hwloc:0-3"`) so `parse_cpuset_string` can re-check provenance from
the string alone.

**The locality strings do not.** `pmix_hwloc_generate_locality_string`
returns bare tokens — a real one off a 4-core Mac is
`"SK0:L20:L13:CR3:HT3:NM0"`, no tag — and that is what a host
environment stores as `PMIX_LOCALITY_STRING`. Note the ordering: the depth
loop walks the topology top-down (`SK`, `L3`, `L2`, `L1`, `CR`, `HT`) and the
**NUMA token is appended last**, because in hwloc 2.x NUMA nodes live at a
special negative depth (`HWLOC_TYPE_DEPTH_NUMANODE`) and are handled by a
separate call after the loop. Do not assume `NM` comes first. (The
`HWLOC_OBJ_NUMANODE` case inside the depth loop is consequently dead but
harmless — `hwloc_get_depth_type` never returns it for a positive depth.)
There is no room for a provider tag: the
token separator is itself `:`, so a leading `hwloc:` is indistinguishable
from a token unless you know to look for it. `get_relative_locality`
therefore accepts *either* spelling and identifies an unprefixed string by
its leading two-letter type code (`pmix_hwloc_locality_payload`). Requiring
the tag is what broke the documented use of `PMIx_Get_relative_locality` —
see item 8 below.

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
   `hwloc_shmem_topology_adopt`). This is the "instant on"/no-copy path:
   every client mmaps the *same* physical pages the server wrote. (There
   used to be a `topo_in_shmem` flag set here; it gated the finalize-time
   cleanup of a segment only the *server* ever creates, so it made that
   cleanup unreachable — see item 9.)
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
- **topology is packed as an XML string only.** hwloc 2.3+ embeds the
  `hwloc_topology_support` flags in the exported XML, and the unpacker
  recovers them by loading with `HWLOC_TOPOLOGY_FLAG_IMPORT_SUPPORT`
  (guarded by `#if HWLOC_API_VERSION >= 0x00020300`, since PMIx's floor is
  2.1). PMIx used to hand-serialize the three support sub-structs
  (`discovery`, `cpubind`, `membind`) as raw `PMIX_BYTE` blobs of
  `sizeof(struct ...)`; because those structs grow across hwloc releases,
  that made the wire format depend on the builder's hwloc ABI and could
  desync the buffer between peers on different hwloc versions. That
  serialization has been removed — do not reintroduce it.
- **the unpacked topology does NOT assert `IS_THISSYSTEM`.** A
  deserialized `PMIX_TOPO` is a *foreign* topology used only for
  structural queries (distances, copy, print, vendor check); the flag is
  reserved for the topology PMIx actually binds against, which is
  established in `pmix_hwloc.c`. This is what lets the print handler
  distinguish a real local machine from a support-less import.
- **`get_*_size` reports the storage the object carries BEYOND its struct.**
  The callers — `PMIx_Value_get_size` and the `data_array` size walker in
  [`bfrop_base_fns.c`](../mca/bfrops/base/bfrop_base_fns.c) — add
  `sizeof(pmix_topology_t)` / `sizeof(pmix_cpuset_t)` themselves (and
  `PMIx_Value_get_size` adds `sizeof(pmix_value_t)` on top, so the number a
  caller sees is larger than what this layer returns). A topology reports
  `hwloc_shmem_topology_get_length`; a cpuset reports the length of its
  list-format string, i.e. exactly what `pack_cpuset` writes. Anything that
  ignores its argument is wrong by construction — see item 10 below.
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
- **Never hand-serialize hwloc structs by `sizeof`.** The
  `hwloc_topology_*_support` structs (and others) are hwloc-ABI
  quantities that grow across releases, so packing `sizeof(struct ...)`
  raw bytes makes the wire format depend on the builder's hwloc version
  and desyncs the buffer between mismatched peers. The topology support
  flags now ride inside the XML via `IMPORT_SUPPORT` instead (see the
  datatype section). If you ever need to ship another hwloc-derived value,
  serialize its *semantic content* (a string, a fixed-width field), never
  its in-memory struct.
- **Feature-test hwloc with `HWLOC_API_VERSION`, not `#ifdef`.** Flags
  like `HWLOC_TOPOLOGY_FLAG_IMPORT_SUPPORT` are enum values, not macros,
  so `#ifdef HWLOC_TOPOLOGY_FLAG_...` is always false and would silently
  disable the feature. Gate version-dependent code on
  `#if HWLOC_API_VERSION >= 0x000NNN00` (e.g. `0x00020300` for the 2.3
  support-import machinery, including the `misc`/`imported_support`
  member that does not exist in older headers).
- **`hwloc_topology_set_xmlbuffer` wants the NUL-inclusive length.**
  hwloc's `export_xmlbuffer` reports (and `set_xmlbuffer` expects) a
  length that *includes* the terminating NUL, so always pass
  `strlen(xml) + 1` — both `load_xml` and `pmix_hwloc_unpack_topology` do.
- **Device distances depend on the cpuset covering something below the
  machine.** `compute_distances` returns `PMIX_ERR_NOT_AVAILABLE` when
  the only object covering the cpuset is the whole machine (common in
  odd containers), and `PMIX_ERR_NOT_FOUND` when no matching devices
  exist — neither is a hard failure.
- **`compute_distances` has one exit path, `cleanup:`.** It builds two heap
  things — the `dists` list and the `devids` argv from `PMIX_DEVICE_ID` — and
  the function has a dozen error returns scattered through a doubly-nested
  loop. Every one of them must reach `cleanup:` (set `rc`, `goto cleanup`).
  Adding a bare `return` there is how `devids` came to leak on *every* call,
  success included (item 12).
- **Render into `char string[PMIX_HWLOC_MAX_STRING]` and pass
  `sizeof(string)`.** The print handler formats object names, attributes and
  cpusets into one fixed stack buffer. Never hand a length constant that is
  not the buffer's own size — the two silently drifted apart once and a
  machine wide enough to need the full length smashed the stack (item 11).
- **`hwloc_bitmap_weight()` returns `-1` on an infinitely-set bitmap**, not a
  count. Any bitmap that has been through `hwloc_bitmap_fill()` is infinite.
  Assigning that to a `size_t` yields `SIZE_MAX` and every arithmetic use
  downstream overflows.

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
5. **`pmix_hwloc_parse_cpuset_string` wrote through its `const char *`
   input** — it temporarily NUL-terminated the caller's string to compare
   the `hwloc:` prefix, which is undefined behavior and crashes on a
   read-only string literal. It now compares in place without mutating.
6. **`pack_cpuset` / `print_cpuset` misread `hwloc_bitmap_list_asprintf`**
   — that function returns the character count (`-1` only on error), *not*
   `0` on success as its header comment claims, so the `0 != rc` checks
   treated every bound (non-empty) cpuset as a failure. They now test for
   a negative return.
7. **`generate_cpuset_string` / `generate_locality_string` read
   `cpuset->source` without a NULL check** — both compared the source with
   `strncasecmp` before validating it, so a cpuset carrying a bitmap but no
   source segfaulted. `generate_locality_string` did not check the `cpuset`
   pointer itself either. Both are reachable only through the public
   `PMIx_server_generate_*_string` APIs, i.e. from arbitrary host-environment
   input, so the crash was reachable from outside the library. They now
   return `PMIX_ERR_BAD_PARAM`, matching what `pmix_hwloc_compute_distances`
   already did, and set the output string to NULL on every failure path
   (the `TAKE_NEXT_OPTION` path previously left the caller's pointer
   untouched). Covered by `test_cpuset_string_bad_source` in the unit test.

8. **`get_relative_locality` demanded a prefix the generator never wrote**
   — `pmix_hwloc_generate_locality_string` emits a bare
   `SK0:L20:CR0:HT0` with no `hwloc:` tag, and a host environment stores
   that verbatim as `PMIX_LOCALITY_STRING`. But
   `pmix_hwloc_get_relative_locality` tested for an `hwloc:` prefix and
   returned `PMIX_ERR_TAKE_NEXT_OPTION` without it — so the usage
   [`include/pmix.h`](../../include/pmix.h) documents for
   `PMIx_Get_relative_locality` ("String returned by the
   `PMIx_server_generate_locality_string` API") yielded no locality at all,
   and every consumer saw unrelated processes. It now accepts either
   spelling via `pmix_hwloc_locality_payload`, which claims an unprefixed
   string only when it starts with one of our type codes so another
   provider's string is still passed on. That helper also NULL-checks, which
   the old `strncasecmp` did not. Found by the dockerswarm Python harness
   (`contrib/dockerswarm/run-python.sh`); the unit test had missed it for
   years by using hand-written `"hwloc:NM0:SK0:CR0:HT0"` literals instead of
   the generator's output — **when testing a consumer, feed it the
   producer's real output, not a literal you wrote to match.**

A second audit (July 2026) of the same directory found these:

9. **`pmix_hwloc_finalize` never reclaimed the shmem segment it created.**
   The cleanup block was gated on a `topo_in_shmem` flag that was set *only*
   on the ADOPT path — the client. But a client never sets `shmemfile` or
   `shmemfd`; the process that owns those is the **server** that *wrote* the
   segment in `setup_topology`, and it never takes the adopt path. So the
   block was unreachable for the only process it applied to: the descriptor
   and the path string leaked at every server finalize, and removal of the
   backing file was left entirely to session-dir teardown. The cleanup is now
   unconditional and runs before the external-topology early-out; the flag,
   having no remaining reader, is gone.
10. **`pmix_hwloc_get_cpuset_size` ignored its argument and returned
    `SIZE_MAX`.** It measured `hwloc_bitmap_weight()` of a *filled* bitmap —
    which is infinitely set, so hwloc returns `-1` — and cast that to
    `size_t`. `PMIx_Value_get_size` then added `sizeof(pmix_cpuset_t)` and
    wrapped; the `data_array` walker in `bfrops` accumulated `SIZE_MAX` per
    element. It now reports the length of the cpuset's list-format string,
    which is what actually goes on the wire.
11. **Stack buffer overflow in the topology print handler.**
    `print_hwloc_obj` declared `char string[1024]` and then passed
    `PMIX_HWLOC_MAX_STRING` (2048) as the length to
    `hwloc_bitmap_snprintf`. A machine with enough PUs for the machine-level
    cpuset to render past 1024 characters overwrote `tmp`/`tmp2`/`pfx` and
    the frame beyond. The buffer is now `PMIX_HWLOC_MAX_STRING` and every
    call takes `sizeof(string)`. Covered by `test_topology_print_wide`,
    which builds a 4096-PU synthetic topology; it segfaults against the old
    code.
12. **`compute_distances` leaked its `devids` argv on every path** — the
    array built from `PMIX_DEVICE_ID` info was freed nowhere, success
    included, and several error returns also skipped the `dists` list. The
    function now has a single `cleanup:` exit that releases both.
13. **Missing NULL screening on public entry points.**
    `PMIx_Parse_cpuset_string` passes its arguments straight through, so a
    NULL string reached `strchr` and crashed; `PMIx_Get_cpuset`,
    `PMIx_Compute_distances` and `pmix_hwloc_check_vendor` dereferenced
    caller-supplied pointers unchecked. A failed
    `pmix_hwloc_parse_cpuset_string` also left a half-built bitmap and source
    on the caller's struct, which the caller had no reason to destruct.
14. **`setup_topology` did `strdup(topo->source)` on a host-provided
    `PMIX_TOPOLOGY2`.** A host is not required to label the topology it hands
    over, and `strdup(NULL)` is undefined. It now falls back to the
    unversioned `"hwloc"` spelling, as the deprecated `PMIX_TOPOLOGY` path
    already did, and skips an entry with no topology at all.
15. **`generate_cpuset_string` did not check
    `hwloc_bitmap_list_asprintf`** — the same misread corrected in
    `pack_cpuset`/`print_cpuset` (item 6), left behind in the third caller.
    On failure the output pointer was uninitialized.

16. **`compute_distances` could not produce a distance *range*.** It
    measured `hwloc_get_common_ancestor_obj(topo, obj, tgt)` inside the
    loop over PUs — but `obj` is the single lowest object covering the
    *whole* cpuset and `tgt` is the device, so neither varies with the
    loop index. Every iteration computed the same value, the PU was
    fetched and then ignored, and `mindist == maxdist` came back for every
    device on every machine.
    [`pmix_device_distance_t(5)`](../../docs/man/man5/pmix_device_distance_t.5.rst)
    is explicit that the pair exists "to support cases where the process
    may be bound to more than one location, and those locations are at
    different distances from the device" — a case the implementation could
    not express. The ancestor is now taken between **this PU** and the
    device. `obj` still serves its other purpose: proving the cpuset
    covers something below the machine (the `PMIX_ERR_NOT_AVAILABLE`
    early-out). Additionally, when no PU in the topology intersects the
    cpuset nothing was measured and the result was `mindist = UINT16_MAX,
    maxdist = 0` — simultaneously "unknown" and "as close as possible",
    and `min > max` besides; both fields now report the documented
    `UINT16_MAX` sentinel. Covered by `test_compute_distances_range`,
    which drives `test/topologies/test-topo.xml` (two packages, real
    NICs) and fails against the old code.

Not a defect, but worth knowing before you "fix" it:

- **`hwloc_get_type_depth(..., HWLOC_OBJ_PU)` is cast to `unsigned`.** If
  PU is absent it returns `HWLOC_TYPE_DEPTH_UNKNOWN` (-1); the cast makes
  `width` zero and the loop simply does not run. Safe, but not obviously
  so.
- **A cpuset spanning the topology's top-level children yields
  `PMIX_ERR_NOT_AVAILABLE`.** `dsearch` looks for a *single* object at
  each depth that completely covers the cpuset, so a binding spanning two
  packages finds none at depth 1 and `obj` stays NULL. That is the
  documented "nothing useful can be done" case, not the range case — a
  range needs multiple locations *under* one covering object.

Items 5, 6, 7, 8 and 10–16 are covered by the `hwloc_datatype` unit test
([`test/unit/hwloc_datatype.c`](../../test/unit/hwloc_datatype.c), wired
into `make check`), which round-trips and prints topologies and cpusets
through the public API. Extend it when you touch this directory.

**What the unit test structurally cannot reach** — and where the multi-node
harness comes in:

| Not testable in one process | Covered by |
|---|---|
| the topology **handoff** (a launched client adopts the segment/XML its local server published; it never discovers its own) | [`examples/topology.c`](../../examples/topology.c) via `contrib/dockerswarm/run-topology.sh` |
| `PMIX_LOCALITY_STRING` **as the host stores it**, fed to `PMIx_Get_relative_locality` | same |
| the **multi-node answer** — on one host every peer is a node-mate, so a run cannot distinguish a correct result from "everything is local" | same (the exerciser fails itself if it finds no off-node peer) |
| the **XML fallback** when no VM hole is available (`hwloc_hole_kind=none`) | `run-topology.sh` runs the whole suite a second time with shmem disabled |
| the shmem segment actually being **reclaimed** at daemon teardown | `run-topology.sh` checks for a surviving `hwloc.sm` |

Item 8 is the cautionary tale for all of this: the unit test had hand-written
`"hwloc:NM0:SK0:CR0:HT0"` literals that matched what the *consumer* wanted
rather than what the *producer* emits, so it passed for years against a
producer/consumer format mismatch that left every process reporting no shared
locality with its own node-mates. **When testing a consumer, feed it the
producer's real output, not a literal you wrote to match.**
`test_locality_generator_to_consumer` and `examples/topology.c` both do that
now.

If you find a genuine bug here, fix it in the source as a standalone,
signed-off commit per the contribution rules — never bend a test to make
buggy behavior pass.
