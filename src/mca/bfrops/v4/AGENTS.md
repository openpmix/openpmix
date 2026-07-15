<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v4` Component

`v4` is the `bfrops` component for the **PMIx 4.0** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v4`. It is the **oldest of the "modern" components**
(priority 50) and marks the most important wire-format break in the
framework's history.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix4.h` | Declares the component and module symbols. |
| `bfrop_pmix4_component.c` | Component struct + `component_query` (priority **50**), open/close of the `types` array, `assign_module`. |
| `bfrop_pmix4.c` | The module `pmix_bfrops_pmix4_module` and its `init` type table. |

## When it is selected

Always available. Chosen for a peer advertising `"v4"`; it is the
fallback modern format when a peer is too old for `v41`/`v51`/`v61` but
new enough to speak the flexible encoding.

## The module and the flexible-integer break

`v4` is the first component to register `pmix_bfrops_base_pack_general_int`
/ `unpack_general_int` for `PMIX_INT16/32/64` and their unsigned twins —
i.e. the **flexible ("squashed") base-7 varint** integer encoding from
`bfrop_base_squash.c`. Older components (`v21`, `v3`) still used
fixed-width integers with their own static packers. So the `v3` → `v4`
boundary is a genuine change in how *every integer* appears on the wire,
which is precisely why it required a new component rather than an edit to
`v3`.

The module otherwise has the standard modern shape: `pmix4_pack`/etc.
trampoline into the base driver with
`&pmix_mca_bfrops_v4_component.types`, and `copy_payload`/`value_*` point
at the base.

## What its `init` registers vs. `v3`

`v4` **drops** the two deprecated types `v3` still carried, and **adds**
thirteen new ones:

- **Removed:** `PMIX_MODEX`, `PMIX_INFO_ARRAY` (with their bespoke `v3`
  `pack_modex`/`pack_array` handlers — gone).
- **Added (13):** `PMIX_COORD`, `PMIX_REGATTR`, `PMIX_REGEX`,
  `PMIX_TOPO`, `PMIX_GEOMETRY`, `PMIX_PROC_CPUSET`, `PMIX_ENDPOINT`,
  `PMIX_DEVICE_DIST`, `PMIX_DEVTYPE`, `PMIX_LOCTYPE`, `PMIX_JOB_STATE`,
  `PMIX_LINK_STATE`, `PMIX_COMPRESSED_BYTE_OBJECT`.

(57 registered types.) All handlers are `pmix_bfrops_base_*` functions —
`v4` has no bespoke serialization code, unlike `v3`/`v21`.

## Gotchas

- **`v4` is where "modern" begins.** All newer components (`v41`, `v51`,
  `v61`) are `v4` plus additional registered types and a higher priority;
  they share `v4`'s base pack code verbatim, so anything they all
  register is byte-identical.
- The integer encoding here differs from `v3`/`v21`. Never "unify" a
  fixed-width component with a flexible one — the two produce different
  bytes by design.
- Frozen at the 4.0 type set. New types belong only in the newest
  component (`v61`).
