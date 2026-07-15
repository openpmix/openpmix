<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v51` Component

`v51` is the `bfrops` component for the **PMIx 5.1** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v51`. It is a **modern** component (priority 60): all
serialization is done by the base, and the version is defined by its
`init` type table.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix51.h` | Declares the component and module symbols. |
| `bfrop_pmix51_component.c` | Component struct + `component_query` (priority **60**), open/close of the `types` array, `assign_module`. |
| `bfrop_pmix51.c` | The module `pmix_bfrops_pmix51_module` and its `init` type table. |

## When it is selected

Always available (no `configure.m4`, no runtime gate). Chosen for a peer
that advertises `"v51"`, and it is the highest-priority module below
`v61`.

## The module

Identical shape to the other modern components: `pmix51_pack`/`unpack`/
`copy`/`print` trampoline into `pmix_bfrops_base_*` with
`&pmix_mca_bfrops_v51_component.types`; `copy_payload` and the `value_*`
slots point straight at the base. Integers use the **flexible base-7
varint** encoding.

## What its `init` registers

`v51` registers everything `v41` does **plus** three types added in 5.1:

| Added type | Handlers |
|------------|----------|
| `PMIX_DEVICE` | `pack_device` / `unpack_device` / `copy_device` / `print_device` |
| `PMIX_RESOURCE_UNIT` | `pack_resunit` / `unpack_resunit` / `copy_resunit` / `print_resunit` |
| `PMIX_RESBLOCK_DIRECTIVE` | `pack_resblock_directive` / `unpack_resblock_directive` / `std_copy` / `print_resblock_directive` |

(66 registered types.) It does **not** register `PMIX_ALLOC_INHERIT`,
`PMIX_NODE_PID`, or `PMIX_REGEX2` — those are `v61`'s additions. A `v51`
peer therefore cannot be sent those types, which is exactly why traffic
to it must use this module.

## Gotchas

- **Do not add newer types here.** `v51` must stay frozen at the 5.1 type
  set so it keeps refusing (via the empty-slot "unknown type" path) what
  5.1 peers cannot read. New types belong in `v61`.
- Everything `v51` and `v61` both register is byte-identical on the wire
  (shared base code) — the components differ only by type set and
  version/priority.
