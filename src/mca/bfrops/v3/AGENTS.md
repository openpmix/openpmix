<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v3` Component

`v3` is the `bfrops` component for the **PMIx 3.x** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v3`. It is a **transitional** component (priority 40): it
uses the shared base driver for framing and composite types, but keeps
its **own fixed-width integer packers** and the deprecated
`PMIX_MODEX`/`PMIX_INFO_ARRAY` types.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix3.h` | Declares the component and module symbols. |
| `bfrop_pmix3_component.c` | Component struct + `component_query` (priority **40**), open/close of the `types` array, `assign_module`. |
| `bfrop_pmix3.c` | The module `pmix_bfrops_pmix3_module`, its `init` table, **and its own static `pack_int*`/`unpack_int*`, `pmix3_bfrop_pack_modex`, and `pmix3_bfrop_pack_array` handlers**. |

## When it is selected

Always available. Chosen for a peer advertising `"v3"`.

## The module — base framing, local integers

`pmix3_pack`/`unpack`/`copy`/`print` trampoline into the base driver, and
`copy_payload`/`value_*` use the base functions — so `v3` shares the
modern *framing*. But its `init` registers **its own** `pack_int`,
`pack_int16`, `pack_int32`, `pack_int64`, `pack_sizet` (and the matching
unpackers), which are **fixed-width, not squashed**. That is the key
difference from `v4`: a `v3` buffer's integers are wide, a `v4`+ buffer's
are flexible varints. Because the base composite packers dispatch integer
sub-fields through this component's `types` array, every integer inside a
`v3` `pmix_value_t`/`pmix_info_t` is also fixed-width.

`v3` also keeps the deprecated `PMIX_MODEX` and `PMIX_INFO_ARRAY` types,
served by its bespoke `pmix3_bfrop_pack_modex` / `pmix3_bfrop_pack_array`
handlers (and matching unpack/copy/print).

## What its `init` registers vs. `v21`

`v3` registers everything `v21` does **plus** two types added in 3.x:

| Added type | Handlers |
|------------|----------|
| `PMIX_ENVAR` | `pack_envar` / `unpack_envar` / `copy_envar` / `print_envar` (base) |
| `PMIX_IOF_CHANNEL` | `pack_iof_channel` / `unpack_iof_channel` / `std_copy` / `print_iof_channel` (base) |

(46 registered types, still including `PMIX_MODEX` and
`PMIX_INFO_ARRAY`.)

## Gotchas

- **`v3`'s integers are fixed-width.** Do not repoint them at the base
  `general_int` (flexible) functions — that would silently change `v3`'s
  wire format and break communication with real 3.x peers.
- The bespoke `modex`/`array` handlers exist only to serve deprecated
  types for old peers. Leave them alone.
- Frozen wire format: a 3.x encoding change would have been (and was) a
  new component (`v4`), never an edit here.
