<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `nvd` Component

`nvd` is the `pnet` component for **InfiniBand and RoCE verbs adapters**
— NVIDIA/Mellanox InfiniBand and ConnectX RoCE, Broadcom NetXtreme-E
(`bnxt_re`) and Intel E810/X722 (`irdma`). Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `nvd`. It started as a near-copy of [`opa`](../opa/AGENTS.md)
retargeted at NVIDIA/Mellanox NICs, and its name still says so; what it
actually owns is the variables of the libraries that drive *any* IB or
RoCE verbs device — NCCL's IB transport, UCX and libfabric's verbs
provider. That is why the other vendors are entries in its table rather
than components of their own: two components writing `UCX_NET_DEVICES`
for one process would overwrite each other. It builds in a stock
configure and runs on any host whose topology shows one of those
adapters; it was hardwired **off** in its `configure.m4` until recently
(see [Building](#building)).

## Files

| File | Contents |
|------|----------|
| `pnet_nvd.h` | Component struct type + `PMIX_PNET_NVD_BLOB` / inventory key `#define`s. |
| `pnet_nvd_component.c` | Component struct, `component_register`, `component_open` (hwloc gate), `component_query` (priority **10**). |
| `pnet_nvd.c` | The module: `allocate` / `setup_local_network` / `setup_fork` / `collect_inventory` / `deliver_inventory`. |

There is deliberately **no `configure.m4`** — see [Building](#building).

## When it is selected

Selection is a two-step gate mirroring `opa`:

1. **`component_open`** asks `pmix_pnet_base_check_devices()` about the
   `pmix_pnet_nvd_match[]` table in `pnet_nvd.c`, and `setup_fork` and
   `collect_inventory` both work from the same table, so what the
   component opens for and what it claims cannot drift apart:

   | Vendor | Class | Type required | What it is |
   |--------|-------|---------------|------------|
   | `0x15b3`, `0x10de` | `0x207` | — | Mellanox/NVIDIA InfiniBand |
   | `0x15b3`, `0x10de` | `0x200` | OpenFabrics | ConnectX in ethernet mode (RoCE) |
   | `0x14e4` | `0x200` | OpenFabrics | Broadcom NetXtreme-E (`bnxt_re`) |
   | `0x8086` | `0x200` | OpenFabrics | Intel E810 / X722 (`irdma`) |

   Class `0x200` is what every plain ethernet port of those vendors
   reports too, so those entries require the OpenFabrics device hwloc
   found on the function; the InfiniBand entries are answered from the PCI
   ids alone, exactly as `pmix_hwloc_check_vendor` always answered them.
   Omni-Path (`0x8086`/`0x208`) belongs to `opa` and EFA to `efa`.
2. **`component_query`** returns `pmix_pnet_nvd_module` at priority
   **10**.

`component_open` returns whatever the check gave it, and only
`PMIX_ERR_NOT_AVAILABLE` is the MCA's "silently ignore me" cue — the
vendor check's other two failures (`PMIX_ERR_BAD_PARAM` for no topology,
`PMIX_ERR_TAKE_NEXT_OPTION` for one that did not come from hwloc) would be
reported as a component that failed to open. Neither is reachable as the
server stands: `PMIx_server_init` runs `pmix_hwloc_setup_topology`, and
fails outright if it cannot, before it opens this framework.

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
  special-case any transport key.) Two things about it are easy to get
  wrong and are covered by `test/unit/pnet_envar_blob.c`; see
  [Gotchas](#gotchas).
- **`collect_inventory`** — asks `pmix_hwloc_check_vendor` for each
  `mymatch[]` pair whether the node carries one of our NICs. It does not
  yet add anything to the inventory list — the "add this to the inventory"
  step is a comment — and it answers `PMIX_SUCCESS` either way; see
  [Gotchas](#gotchas) for why it must.
- **`setup_fork`** — names the adapters this one process was mapped
  against to the libraries that will use them. It asks
  `pmix_pnet_base_get_assigned_devices()` for the process's devices
  filtered by the table above, drops any that is not a channel adapter
  (below), then writes:

  | Variable | Value | Why that form |
  |----------|-------|---------------|
  | `NCCL_IB_HCA` | `mlx5_0` | NCCL matches an HCA by name, so the selector goes in as it stands |
  | `UCX_NET_DEVICES` | `mlx5_0:*` | UCX names a device *port* and matches each entry as a glob, so this is "this card, whatever ports it has" without PMIx having to pick one. The `:` is load-bearing: `mlx5_1*` would also match `mlx5_10` on a node with enough cards |
  | `FI_VERBS_DEVICE_NAME` | `mlx5_0` | libfabric's verbs provider — but it matches each entry as a **prefix** (`strncasecmp` over the entry's length), and no spelling anchors it. So it is written only when no other OpenFabrics device on the node begins with an assigned name; otherwise it is left unset, with a verbose message saying which device it would also have selected |

  **iWARP is dropped by its node type, and only Intel's adapters are
  asked.** Intel's `irdma` runs RoCE or iWARP depending on how the driver
  was loaded, and X722 runs iWARP only; neither NCCL nor UCX drives iWARP,
  and a `UCX_NET_DEVICES` naming only an iWARP device leaves UCX no device
  for any transport. The topology does not say which a device is — hwloc
  does not record it — but the kernel does, in
  `/sys/class/infiniband/<dev>/node_type` (`1: CA` versus `4: RNIC`), and
  `setup_fork` runs on the node it describes. An Intel device whose file
  cannot be read is dropped too. NVIDIA's and Broadcom's adapters cannot be
  iWARP, so they are named without the file: asking them would drop them
  wherever it is absent — a topology loaded on a machine without the
  hardware, as every test of this path is — for an answer the file could
  not change. The Intel names are picked out with a one-entry match table
  (`maybe_iwarp`) through the same base helper. The root of that tree is the
  `pnet_nvd_sysfs_root` parameter (default `/sys`), which exists so the
  check can be run against a captured tree — `test/unit/pnet_sysfs.h`
  builds one. If every assigned device is dropped, nothing is written.

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

- **The unpack loop's terminating status is not a failure.** The envar
  stream carries no count, so `setup_local_network` unpacks until the
  buffer runs out and the loop can only end on
  `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER`. That has to be turned back
  into `PMIX_SUCCESS` before returning: the base treats anything other
  than `SUCCESS` / `NOT_AVAILABLE` / `TAKE_NEXT_OPTION` as a hard error,
  which stops the fan-out to every pnet component behind us **and** fails
  `PMIx_server_setup_local_support` for the whole job. `opa` has always
  done this; `nvd` did not, so the only job it let launch was one whose
  blob it never found.
- **`pmix_compress.decompress()` writes nothing when it fails, and the
  failure is not exotic.** A node that built no compression component
  still receives compressed blobs from a lead server that did, and the
  base's stub decompressor simply returns `false` — as does a real one
  fed a damaged blob. It leaves the output pointer untouched, so its
  return value must be checked before the result is read *or freed*.
  Decline the blob rather than continuing.
- **`bkt` must never be destructed.** `PMIX_LOAD_BUFFER_NON_DESTRUCT`
  parks a *borrowed* pointer in the buffer — either the info's own byte
  object or the block the decompressor allocated, which this function
  frees itself. Destructing the buffer would free it a second time.
- **`allocate` hands the payload over exactly once.** `PMIX_UNLOAD_BUFFER`
  transfers the packed bytes to a `pmix_byte_object_t` without freeing
  anything, so when compression succeeds — and the `pmix_kval_t` therefore
  owns the *compressed* copy — the uncompressed block has to be freed by
  hand. `pgpu/{nvd,intel}` and `pnet/opa` all do this.
- **A name in `FI_VERBS_DEVICE_NAME` is a prefix.** The libfabric verbs
  provider compares each entry with `strncasecmp` over the entry's own
  length, so `mlx5_1` selects `mlx5_10`. Writing it unconditionally is a
  silent over-selection on any node with ten or more devices of one
  driver; `verbs_prefix_collides()` is what stops that, and it assumes
  the worst if the node's devices cannot be listed.
- **`collect_inventory` reports presence but not contents.** It confirms a
  matching NIC exists but does not populate inventory; treat inventory
  support as unfinished.
- **Any error out of `collect_inventory` aborts the whole fan-out.**
  Unlike `allocate` / `setup_fork`, the base has no decline convention
  there: it `PMIX_ERROR_LOG`s a non-`SUCCESS` return and stops, so the
  other components' inventory never gets collected. This is why "no
  matching NIC" answers `PMIX_SUCCESS` here rather than
  `TAKE_NEXT_OPTION`. The `PMIX_ERR_NOT_SUPPORTED` guard on a
  non-hwloc topology is defensive only — `component_open` already ran
  the same check, which applies the same source test and must have
  passed for this module to be active at all.
- **The envar-glob defaults are the interface to the comms stacks.**
  `UCX_*,HCOLL_*,UCC_*,SHARP_*,NCCL_*` are what get forwarded to compute
  nodes; changing them changes which runtime settings propagate.
