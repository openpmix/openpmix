<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v41` Component

`v41` is the `bfrops` component for the **PMIx 4.1** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v41`. It is a **modern** component (priority 58) built
entirely on the base driver.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix41.h` | Declares the component and module symbols. |
| `bfrop_pmix41_component.c` | Component struct + `component_query` (priority **58**), open/close of the `types` array, `assign_module`. |
| `bfrop_pmix41.c` | The module `pmix_bfrops_pmix41_module` and its `init` type table. |

## When it is selected

Always available. Chosen for a peer advertising `"v41"`. Its priority
(58) sits just below `v51` (60) and above `v4` (50), so `assign_module`
prefers it over `v4` when a peer supports both.

## The module

Modern shape: `pmix41_pack`/`unpack`/`copy`/`print` trampoline into
`pmix_bfrops_base_*` with `&pmix_mca_bfrops_v41_component.types`;
`copy_payload` and the `value_*` slots are the base functions. Integers
use the **flexible base-7 varint** encoding (shared with `v4`/`v51`/
`v61`).

## What its `init` registers

`v41` registers everything `v4` does **plus** six types added in 4.1 —
the storage family and two others:

| Added type | Handlers |
|------------|----------|
| `PMIX_STOR_MEDIUM` | `pack_smed` / `unpack_smed` / `std_copy` / `print_smed` |
| `PMIX_STOR_ACCESS` | `pack_sacc` / `unpack_sacc` / `std_copy` / `print_sacc` |
| `PMIX_STOR_PERSIST` | `pack_spers` / `unpack_spers` / `std_copy` / `print_spers` |
| `PMIX_STOR_ACCESS_TYPE` | `pack_satyp` / `unpack_satyp` / `std_copy` / `print_satyp` |
| `PMIX_DATA_BUFFER` | `pack_dbuf` / `unpack_dbuf` / `copy_dbuf` / `print_dbuf` |
| `PMIX_PROC_NSPACE` | `pack_nspace` / `unpack_nspace` / `copy_nspace` / `print_nspace` |

(63 registered types.) It does not know the 5.1 types (`PMIX_DEVICE`,
`PMIX_RESOURCE_UNIT`, `PMIX_RESBLOCK_DIRECTIVE`) or 6.1's additions.

## Gotchas

- Frozen at the 4.1 type set. New types go in the newest component only.
- Byte-identical to `v4`/`v51`/`v61` for any type they share — the
  distinction is purely the registered type set plus version/priority.
