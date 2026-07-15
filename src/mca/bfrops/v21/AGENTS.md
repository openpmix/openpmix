<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v21` Component

`v21` is the `bfrops` component for the **PMIx 2.1** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v21`. It is the **first component built on the shared base
driver** (priority 30) — the bridge between the legacy self-contained
encoders (`v12`/`v20`) and the base-driven modern components.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix21.h` | Declares the component and module symbols. |
| `bfrop_pmix21_component.c` | Component struct + `component_query` (priority **30**), open/close of the `types` array, `assign_module`. |
| `bfrop_pmix21.c` | The module `pmix_bfrops_pmix21_module`, its `init` table, and its own static `pack_int*`/`unpack_int*`, `pmix21_bfrop_pack_modex`, and `pmix21_bfrop_pack_array` handlers. |

## When it is selected

Always available. Chosen for a peer advertising `"v21"`.

## Same types as `v20`, different framing

`v21` registers the **exact same 44-type set as `v20`** — the type-set
delta between them is empty. What changed at 2.0 → 2.1 is not *which*
types exist but *how they are framed*: `v21`'s module trampolines
`pack`/`unpack`/`copy`/`print` into `pmix_bfrops_base_*` (the shared
driver), whereas `v20` has its own standalone `pack.c`/`unpack.c`/…
files. This is the point where the framework's serialization consolidated
into `base/`.

Like `v3`, `v21` still registers **its own fixed-width integer packers**
(`pack_int`, `pack_int16/32/64`, `pack_sizet`) rather than the flexible
base encoding, and keeps the deprecated `PMIX_MODEX` / `PMIX_INFO_ARRAY`
types with bespoke `pmix21_bfrop_pack_modex` / `pack_array` handlers.
`copy_payload` and the `value_*` slots point at the base functions.

## Gotchas

- **`v21` and `v20` are not interchangeable despite identical type
  sets** — their framing differs, so they are distinct wire formats and
  distinct components. Assigning the wrong one to a peer corrupts the
  stream.
- `v21`'s integers are fixed-width; do not repoint them at the flexible
  base `general_int` functions.
- Frozen wire format — talks to real 2.1 peers. A format change is a new
  component (`v3`), not an edit here.
