<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: `bfrops/base`

This is where essentially all of `bfrops` actually lives. Read the
framework [`AGENTS.md`](../AGENTS.md) first — it explains why `bfrops`
is multi-select, what a component is (one wire-format version), and the
interoperability rules that make editing anything here consequential.
This file covers what is true of `base/` specifically: the four
operations and the contract that binds them together, who the callers
are, and the failure modes this code has actually had.

`base/` is not a component. The eight component directories are almost
entirely registration tables; the functions they register are all
`pmix_bfrops_base_*` and are defined here. **A change to a pack or
unpack byte layout here changes the wire format of every version that
registers it, simultaneously.**

## The files

| File | Contents |
|------|----------|
| [`base.h`](base.h) | the internal API: `pmix_bfrop_type_info_t`, `PMIX_REGISTER_TYPE`, the `PMIX_BFROPS_PACK_TYPE`/`_UNPACK_TYPE` driver macros, `PMIX_SQUASH_TYPE_SIZEOF`, and a prototype for every `pmix_bfrops_base_*` |
| [`bfrop_base_frame.c`](bfrop_base_frame.c) | framework declaration, the MCA parameters, `pmix_bfrops_globals`, and the `PMIX_CLASS_INSTANCE`s for `pmix_buffer_t`, `pmix_kval_t`, `pmix_bfrop_type_info_t` |
| [`bfrop_base_select.c`](bfrop_base_select.c) | build the priority-ordered `actives` list; reject a module whose `init` fails |
| [`bfrop_base_stubs.c`](bfrop_base_stubs.c) | `assign_module` (version string → component), `get_available_modules`, `PMIx_Data_type_string` |
| [`bfrop_base_pack.c`](bfrop_base_pack.c) | the pack driver and one packer per data type |
| [`bfrop_base_unpack.c`](bfrop_base_unpack.c) | the mirror-image unpack driver and per-type unpackers |
| [`bfrop_base_copy.c`](bfrop_base_copy.c) | per-type deep copy, plus `std_copy` for the fixed-size types |
| [`bfrop_base_print.c`](bfrop_base_print.c) | per-type render-to-string |
| [`bfrop_base_cmp.c`](bfrop_base_cmp.c) | per-type comparison behind `pmix_bfrops_base_value_cmp` |
| [`bfrop_base_squash.c`](bfrop_base_squash.c) | the flexible (base-7 varint) integer codec used by every modern component |
| [`bfrop_base_get_number.c`](bfrop_base_get_number.c) | `PMIx_Value_get_number` and its per-type range/precision checks |
| [`bfrop_base_fns.c`](bfrop_base_fns.c) | buffer helpers (`buffer_extend`, `too_small`, store/get data type), value load/unload/xfer, and a block of **public** `PMIx_Info_list_*` / `PMIx_Value_get_size` APIs |
| [`bfrop_base_macro_backers.c`](bfrop_base_macro_backers.c) | the out-of-line bodies behind the public inline `PMIx_*` utility macros (`PMIx_Argv_*`, `PMIx_Value_*`, `PMIx_Info_*`, `PMIx_Load_key`, …) |
| [`bfrop_base_tma.h`](bfrop_base_tma.h) | the inline TMA (custom-allocator) implementations that nearly everything above delegates to with `tma == NULL` |

The last two matter more than their names suggest: a large slice of the
**installed public API** is implemented here, not just internal wire
code. `bfrop_base_tma.h` is the single largest file in the framework and
holds the real bodies — `pmix_bfrops_base_value_xfer()` in
`bfrop_base_fns.c` is a one-line call into
`pmix_bfrops_base_tma_value_xfer()`. When you go looking for what a
value operation *does*, look in the TMA header.

## Six operations that have to agree, per data type

For any `pmix_data_type_t`, the tree has to answer six questions
consistently. They are answered in six different places, and every
defect the August 2026 review of this directory found was two of them
disagreeing:

| Question | Answered by |
|----------|-------------|
| How do I put one on the wire? | `pmix_bfrops_base_pack_<t>` in `bfrop_base_pack.c` |
| How do I take one off? | `pmix_bfrops_base_unpack_<t>` in `bfrop_base_unpack.c` |
| How do I deep-copy one? | `pmix_bfrops_base_tma_copy_<t>`, and the `case` for it in `pmix_bfrops_base_tma_value_xfer()` |
| How big is one **element of an array** of them? | `pmix_bfrops_base_tma_data_array_construct()` |
| How do I copy an **array** of them? | the `case` for it in `pmix_bfrops_base_tma_copy_darray()` |
| How do I release an **array** of them? | the `case` for it in `pmix_bfrops_base_tma_data_array_destruct()` |

The last three are the ones that get forgotten, because nothing forces
them to be written. A type registered in `v61` with pack, unpack and
copy but no arm in `data_array_construct()` looks completely finished:
it works as a scalar value, and it packs perfectly well inside an array.
It only fails on the way *back*, because `unpack_darray()` builds its
destination with `PMIX_DATA_ARRAY_CONSTRUCT` and gets `size 0, array
NULL`, which it reports as `PMIX_ERR_NOMEM`. Eighteen registered types
were in that state.

The width mismatch is worse than the omission, because it does not fail:
`PMIX_POINTER` was allocated one byte per element by
`data_array_construct()` and copied `sizeof(char*)` bytes per element by
`copy_darray()`, so copying such an array read eight times its length.

**If you add a data type, add all six.**
[`test/unit/bfrops_darray.c`](../../../../test/unit/bfrops_darray.c)
walks the whole registered type table holding construct, pack/unpack and
copy against each other; add your type to `all_types[]` there and it is
covered.

## Nothing here may read outside the buffer it was given

Every byte the unpacker sees came off a socket, including the count that
says how many values follow. A peer can be truncated, can be running a
different version than it claimed, or can be hostile. The contract is:

> Returning an error is fine. Returning a wrong value is tolerable.
> Reading outside the buffer is not.

Two specific things make that easy to get wrong here.

**The count and the payload are independent.** `pmix_bfrops_base_unpack`
reads a count off the wire and clamps it to the storage the *caller*
provided — not to the bytes remaining. So a per-type unpacker's loop can
run more iterations than there is data for, and every one of them has to
notice for itself. `pmix_bfrops_base_unpack_general_int` did not: it
checked `pack_ptr == unpack_ptr` once, before the loop, and thereafter
handed `pmix_bfrops_base_decode_int` an `avail_size` that reached zero
and kept going.

**The flexible integer decoder's length is decided by the data.** A byte
with the continuation flag set means "read one more", so the last
readable byte of a truncated value is an invitation to read past the
end. `flex_unpack_integer()` bounded its loop with `flex_size - 1`,
which is `SIZE_MAX` when the available size is zero — an unbounded
over-read that a three-byte message could trigger, and did (it
segfaults against a guard page). It now refuses an empty region outright
and reports truncation rather than assembling a value out of whatever
bytes happened to be there.

[`test/unit/bfrops_malformed.c`](../../../../test/unit/bfrops_malformed.c)
covers this, including a stage that truncates a well-formed message at
every possible offset. Add to it rather than to a scratch program.

## Defects found in the August 2026 review

Recorded because each is a shape that will recur, not for history's
sake.

- **`flex_unpack_integer()` read past the end of the buffer**
  (`bfrop_base_squash.c`). `flex_size - 1` underflowed for a zero-length
  source, and the caller
  (`pmix_bfrops_base_unpack_general_int`) produced zero-length sources
  whenever the wire claimed more values than it sent. Remotely
  triggerable crash. Both ends fixed: the decoder refuses an empty
  region and signals truncation, and the loop bound no longer wraps.
- **A typeless array desynchronized the stream** (`bfrop_base_pack.c` /
  `bfrop_base_unpack.c`). `pack_darray()` wrote a `PMIX_UNDEF` type tag
  *and* a size; `unpack_darray()` treated the tag as a terminator and
  never read the size. Everything after it in the message was misread.
  Reachable without malice: copying a value whose `darray` pointer was
  NULL produces exactly such a descriptor. The packer now emits the
  marker the unpacker reads.
- **`copy_darray()` freed the element block twice** for
  `PMIX_PROC_CPUSET` (`bfrop_base_tma.h`). It called
  `pmix_hwloc_release_cpuset()`, which frees the block, and then
  `pmix_tma_free()` on the same pointer. Reached by any
  `PMIx_Value_xfer` / `PMIX_INFO_XFER` of a cpuset array whose elements
  are not valid hwloc objects — which includes every array
  `PMIx_Data_array_create()` hands back.
- **An unpacked `PMIX_DATA_BUFFER` could not be read**
  (`bfrop_base_unpack.c`). `unpack_dbuf()` restored `base_ptr` and
  `bytes_used` but left `pack_ptr`, `unpack_ptr` and `bytes_allocated`
  untouched, so the caller got a buffer holding the payload and no way
  to reach it.
- **Eighteen registered types could not back a data array**
  (`bfrop_base_tma.h`). See the six-operations table above.
- **`PMIX_POINTER` arrays were allocated one byte per element and copied
  eight** (`bfrop_base_tma.h`) — an over-read of the whole block.
- **`copy_darray()` cast `pmix_data_buffer_t*` to `pmix_buffer_t*`.**
  They are not layout-compatible: `pmix_buffer_t` leads with a
  `pmix_object_t` and a type field. Use `PMIx_Data_copy_payload()` for
  the public struct and `pmix_bfrops_base_tma_copy_payload()` for the
  internal one; never cast between them.
- **`buffer_extend()` used its `realloc` result before checking it**
  (`bfrop_base_tma.h`) — `memset` through NULL on allocation failure,
  plus a leak of the old block.
- **`pack_kval()` dereferenced a NULL `value` pointer**
  (`bfrop_base_pack.c`). `pmix_kval_t.value` is a pointer and can
  legitimately be NULL; it now sends an undefined value in its place so
  the stream stays symmetric.
- **Unpacked strings were not guaranteed to be terminated**
  (`bfrop_base_unpack.c`). The packer includes the terminator in the
  length it writes; a peer need not. Everything downstream treats the
  result as a C string, so the invariant is now enforced on arrival
  rather than assumed.

## A known inconsistency, deliberately not "fixed"

**`PMIX_REGEX` has two incompatible readings as an array element
type.** `pmix_bfrops_base_pack_regex()` and `_unpack_regex()` stride the
block as `char *` — one regex string per element. `data_array_destruct()`
strides it as `pmix_byte_object_t`. Those are different widths, so one
of the two walks off the end of any array with more than one element.

For a *scalar* value the two readings coincide, because `bo.bytes` is
the first member of the union — which is why `PMIX_REGEX` values work
correctly everywhere else and why this went unnoticed. It diverges only
for arrays, and arrays of `PMIX_REGEX` are not constructible today
(`data_array_construct()` has no arm for the type, so it returns an
empty descriptor), so nothing currently reaches the divergence.

`PMIX_REGEX` was therefore left out of `data_array_construct()` on
purpose. Adding an arm for it without first settling which stride is
correct — and fixing pack, unpack, copy and destruct together — converts
a clean "unsupported" into a heap error. If you resolve it, resolve all
four.

## MCA parameters

Registered in `bfrop_base_frame.c`, all under `pmix_bfrops_base_`:

| Parameter | Meaning |
|-----------|---------|
| `initial_size` | starting allocation of a new buffer |
| `threshold_size` | size at which `buffer_extend` stops doubling and grows additively |
| `max_array_depth` | how deeply data arrays may nest before pack and unpack refuse; 0 disables the cap. A couple of bytes of message buys a stack frame, so this is what stops a peer from sinking the stack — see the depth tests in [`test/unit/nested_darray.c`](../../../../test/unit/nested_darray.c) |
| `default_type` | described vs. non-described for new buffers; described is the default in `PMIX_ENABLE_DEBUG` builds |

## Threading

Nothing here thread-shifts, blocks, or touches shared state beyond the
`actives` list and the per-component `types` array, both built once at
init. These are pure transforms of their arguments and are called
directly from progress-thread handlers throughout the library. There is
no caddy pattern. The one rule: the caller owns the buffer, and must not
let two threads mutate it at once.

## Testing

| Where | What it covers |
|-------|----------------|
| [`test/unit/bfrops_darray.c`](../../../../test/unit/bfrops_darray.c) | every registered type as a data-array element, held across construct / pack / unpack / copy; the typeless-array marker; nested data buffers |
| [`test/unit/bfrops_malformed.c`](../../../../test/unit/bfrops_malformed.c) | truncated and lying input; flexible-integer boundaries |
| [`test/unit/nested_darray.c`](../../../../test/unit/nested_darray.c) | array nesting and the depth cap |
| [`test/unit/bfrops_regex2.c`](../../../../test/unit/bfrops_regex2.c) | `PMIX_REGEX2` pack/unpack/copy/print/compare |
| [`test/unit/bfrops_alloc_inherit.c`](../../../../test/unit/bfrops_alloc_inherit.c) | `PMIX_ALLOC_INHERIT` |
| [`contrib/dockerswarm/run-bfrops-tests.sh`](../../../../contrib/dockerswarm/AGENTS.md) | the half a single process cannot reach — the peer's module and the negotiated buffer type — by moving every data type between ranks on different nodes |

The last row is the one that is easy to dismiss and should not be. A
round trip inside one process is self-consistent under any choice of
module and any buffer type, so it cannot fail on either. Only a real
socket between two servers makes those choices observable.
