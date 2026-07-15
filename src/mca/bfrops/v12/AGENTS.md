<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v12` Component

`v12` is the `bfrops` component for the **PMIx 1.2** wire format — the
oldest format the library still supports. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `v12`. Like `v20` it is a **legacy, self-contained** component
(the lowest priority, **5**) with its own complete serialization code.

## Files

| File | Contents |
|------|----------|
| `bfrop_v12.h` | Declares the component and module symbols. |
| `bfrop_v12_component.c` | Component struct + `component_query` (priority **5**), open/close of the `types` array, `assign_module`. |
| `bfrop_v12.c` | The module `pmix_bfrops_pmix12_module` and its `init` table (registering `pmix12_bfrop_*` handlers). |
| `internal.h` | Prototypes for all `pmix12_bfrop_*` functions and the deprecated `pmix_info_array_t` / `pmix_modex_data_t` structs. |
| `pack.c` / `unpack.c` | The 1.2 packers/unpackers. |
| `copy.c` | Deep-copy, `copy_payload`, `value_xfer`, `value_cmp`. |
| `print.c` | Type printers. |

## When it is selected

Always available. Chosen only for a peer advertising `"v12"` — a genuine
PMIx 1.2-era process. Being lowest priority (5), it is the last resort in
any negotiation and never `assign_module(NULL)`'s pick.

## Self-contained legacy encoding

As with `v20`, every module slot points at a `pmix12_bfrop_*` function
and nothing is shared with `base/`; integers are fixed-width network
byte order. `v12` supplies its own `copy_payload`/`value_xfer`/
`value_load`/`value_unload`/`value_cmp`.

## Type set

`v12` registers **42 types** — the 1.2 baseline. Compared with `v20` it
**lacks** `PMIX_ALLOC_DIRECTIVE` and `PMIX_COMPRESSED_STRING` (both added
at 2.0). It still carries the deprecated `PMIX_MODEX` and
`PMIX_INFO_ARRAY`.

## Gotchas

- **Frozen and fragile.** `v12` is the compatibility floor; its bytes
  must never change. Do not route it through the base driver or alter any
  `pack.c`/`unpack.c` logic.
- If you are adding functionality, `v12` is almost never the place — new
  types and encodings go into the newest component (`v61`). `v12` only
  gains attention when fixing an interop bug against actual 1.2 peers.
