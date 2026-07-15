<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `nvd` Component

`nvd` is the `pnet` component for **Mellanox / NVIDIA** fabric and
networking support. Read the framework [`AGENTS.md`](../AGENTS.md) first;
this file covers only what is specific to `nvd`. It is a near-copy of
[`opa`](../opa/AGENTS.md) retargeted at NVIDIA/Mellanox NICs, and it does
compile against the current `pmix_pnet_module_t` interface — but it is
**hardwired off in the build** (see [Building](#building)), so it is
current-but-dormant rather than a live code path.

## Files

| File | Contents |
|------|----------|
| `pnet_nvd.h` | Component struct type + `PMIX_PNET_NVD_BLOB` / inventory key `#define`s. |
| `pnet_nvd_component.c` | Component struct, `component_register`, `component_open` (hwloc gate), `component_query` (priority **10**). |
| `pnet_nvd.c` | The module: `allocate` / `setup_local_network` / `collect_inventory` / `deliver_inventory`. |
| `configure.m4` | Hardwired **off** (`test "yes" = "no"`); adds the `NVIDIA` summary line. |

## When it would be selected

If it were built, selection would be a two-step gate mirroring `opa`:

1. **`component_open`** probes hwloc twice via `pmix_hwloc_check_vendor` —
   first for Mellanox vendor `0x15b3`, then (on failure) for NVIDIA vendor
   `0x10de` — both against PCI class `0x207`. It opens only if a matching
   device is found.
2. **`component_query`** returns `pmix_pnet_nvd_module` at priority
   **10**.

## The module

```c
pmix_pnet_module_t pmix_pnet_nvd_module = {
    .name = "nvd",
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};
```

The signatures match the current framework interface (unlike `tcp` and
`simptest`).

## What the functions do

- **`allocate`** — like `opa`'s but envar-only (no security key). It reads
  `PMIX_SETUP_APP_ENVARS` / `PMIX_SETUP_APP_ALL`, harvests envars with
  `pmix_util_harvest_envars` using the component's `include`/`exclude`
  globs (default include `"UCX_*,HCOLL_*,UCC_*,SHARP_*,NCCL_*"` — the
  UCX/HCOLL/UCC/SHARP/NCCL stacks), packs each as `PMIX_ENVAR`, wraps the
  buffer in a `pmix_kval_t` keyed `PMIX_PNET_NVD_BLOB`, compresses it, and
  appends to `ilist`.
- **`setup_local_network`** — finds `PMIX_PNET_NVD_BLOB`, decompresses if
  needed, unpacks the `PMIX_ENVAR` stream, and appends each to
  `ns->envars` for fork-time injection. (Unlike `opa` it does **not**
  special-case any transport key.)
- **`collect_inventory`** — actually walks the hwloc PCI device list for a
  Mellanox (`0x15b3`) or NVIDIA (`0x10de`) device of class `0x207`; on the
  first match it returns `PMIX_SUCCESS`. It does not yet add anything to
  the inventory list — the "add this to the inventory" step is a comment.
  Returns `PMIX_ERR_NOT_SUPPORTED` if the topology is not hwloc-sourced.
- **`deliver_inventory`** — a stub returning `PMIX_SUCCESS`.

## Building

`nvd`'s `configure.m4` uses `AS_IF([test "yes" = "no"], …)`, so the
can-compile branch is never taken and the source is **not compiled into
`libpmix`** (the `NVIDIA` line in configure's summary reads `no`). Only
its `Makefile` is generated. To actually build it you would have to change
the `configure.m4` condition and re-run `./autogen.pl && ./configure … &&
make` — do not attempt that as a drive-by; the disablement is intentional
until the component is finished.

## Gotchas

- **It is dormant by design.** Because it is not built, changes here are
  not exercised by CI. If you edit it, verify it still matches the current
  `pmix_pnet_module_t` and only enable it deliberately.
- **`collect_inventory` reports presence but not contents.** It confirms a
  matching NIC exists but does not populate inventory; treat inventory
  support as unfinished.
- **The envar-glob defaults are the interface to the comms stacks.**
  `UCX_*,HCOLL_*,UCC_*,SHARP_*,NCCL_*` are what get forwarded to compute
  nodes; changing them changes which runtime settings propagate.
