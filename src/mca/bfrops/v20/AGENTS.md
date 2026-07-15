<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v20` Component

`v20` is the `bfrops` component for the **PMIx 2.0** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v20`. It is a **legacy, self-contained** component
(priority 20): unlike the base-driven components it carries its own
complete pack/unpack/copy/print implementation and predates the shared
base serialization driver.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix20.h` | Declares the component and module symbols. |
| `bfrop_pmix20_component.c` | Component struct + `component_query` (priority **20**), open/close of the `types` array, `assign_module`. |
| `bfrop_pmix20.c` | The module `pmix_bfrops_pmix20_module` and its `init` table (registering `pmix20_bfrop_*` handlers). |
| `internal.h` | Prototypes for all `pmix20_bfrop_*` functions and the deprecated `pmix_info_array_t` / `pmix_modex_data_t` structs. |
| `pack.c` / `unpack.c` | The 2.0 packers/unpackers. |
| `copy.c` | Deep-copy, `copy_payload`, `value_xfer`, and `value_cmp`. |
| `print.c` | Type printers. |

## When it is selected

Always available. Chosen for a peer advertising `"v20"` — i.e. a real
PMIx 2.0 process.

## Self-contained legacy encoding

Every module slot points at a `pmix20_bfrop_*` function; nothing is
shared with `base/`. Integers are **fixed-width in network byte order**
— e.g. `pmix20_bfrop_pack_int32` does `htonl` then `memcpy` — the classic
described-buffer-era encoding. The module even supplies its own
`copy_payload`, `value_xfer`, and `value_cmp` (`pmix20_bfrop_*`) rather
than the base versions.

The public `pmix_value_cmp()` symbol (declared in `bfrops.h`) is, for
historical reasons, defined in this component's `copy.c`.

## What its `init` registers vs. `v12`

`v20` registers everything `v12` does **plus** two types:

| Added type | Handlers |
|------------|----------|
| `PMIX_ALLOC_DIRECTIVE` | `pmix20_bfrop_*` |
| `PMIX_COMPRESSED_STRING` | `pmix20_bfrop_*` (via the byte-object handlers) |

(44 registered types, including the deprecated `PMIX_MODEX` and
`PMIX_INFO_ARRAY`.)

## Gotchas

- **Do not touch `v20`'s bytes.** It exists solely to talk to PMIx 2.0
  peers; any change to `pack.c`/`unpack.c` breaks that interop with no
  actionable error for the user.
- `v20` shares no code with the base driver, so a "cleanup" that routes
  it through `pmix_bfrops_base_*` would silently change its wire format —
  never do it.
- Its type set is identical to `v21`'s, but the framing differs; they are
  separate wire formats (see the `v21` doc).
