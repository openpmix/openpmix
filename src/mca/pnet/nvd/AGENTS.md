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
[`opa`](../opa/AGENTS.md) retargeted at NVIDIA/Mellanox NICs. It builds
in a stock configure and runs on any host whose topology shows a Mellanox
or NVIDIA InfiniBand controller; it was hardwired **off** in its
`configure.m4` until recently (see [Building](#building)).

## Files

| File | Contents |
|------|----------|
| `pnet_nvd.h` | Component struct type + `PMIX_PNET_NVD_BLOB` / inventory key `#define`s. |
| `pnet_nvd_component.c` | Component struct, `component_register`, `component_open` (hwloc gate), `component_query` (priority **10**). |
| `pnet_nvd.c` | The module: `allocate` / `setup_local_network` / `setup_fork` / `collect_inventory` / `deliver_inventory`. |

There is deliberately **no `configure.m4`** — see [Building](#building).

## When it is selected

Selection is a two-step gate mirroring `opa`:

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
    .setup_fork = setup_fork,
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
- **`setup_fork`** — names the NICs this one process was mapped against
  to the libraries that will use them. It asks
  `pmix_pnet_base_get_assigned_devices()` for the process's devices
  filtered by the same PCI `(vendor, class)` pairs `component_open`
  probed for, then writes:

  | Variable | Value | Why that form |
  |----------|-------|---------------|
  | `NCCL_IB_HCA` | `mlx5_0` | NCCL matches an HCA by name, so the selector goes in as it stands |
  | `UCX_NET_DEVICES` | `mlx5_0:*` | UCX names a device *port* and matches each entry as a glob, so this is "this card, whatever ports it has" without PMIx having to pick one. The `:` is load-bearing: `mlx5_1*` would also match `mlx5_10` on a node with enough cards |

  Both are overwritten if already set: a process mapped against a device
  made the more specific request, and the names come from the topology as
  this daemon sees it, so where an RM has already narrowed what the node
  presents they are a subset of what is visible. A process that was not
  mapped against one of our NICs is left alone (the helper returns
  `PMIX_ERR_TAKE_NEXT_OPTION`, which the base treats as "nothing to say").
- **`deliver_inventory`** — a stub returning `PMIX_SUCCESS`.

## Building

`nvd` ships **no `configure.m4`** and builds unconditionally; the MCA
machinery configures a component with no `configure.m4` by itself. Keep it
that way. It links nothing and needs no SDK — it reads info attributes
hwloc already recorded and sets environment variables — and whether it has
work to do is a property of the machine the *daemon* runs on, which only
`component_open` can know. It previously carried an
`AS_IF([test "yes" = "no"], …)` gate justified by "no real
NVIDIA-transport detection exists yet", which asked that question at build
time on a host that is routinely not the run host; the one visible
consequence of dropping the file is that configure's summary no longer
prints a `Transports / NVIDIA` line.

## Gotchas

- **`collect_inventory` reports presence but not contents.** It confirms a
  matching NIC exists but does not populate inventory; treat inventory
  support as unfinished.
- **The envar-glob defaults are the interface to the comms stacks.**
  `UCX_*,HCOLL_*,UCC_*,SHARP_*,NCCL_*` are what get forwarded to compute
  nodes; changing them changes which runtime settings propagate.
