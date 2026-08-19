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
   device is found. The `mymatch[]` table in `pnet_nvd.c` holds the same
   two pairs, and `setup_fork` and `collect_inventory` both work from it,
   so what the component opens for and what it claims cannot drift apart.
2. **`component_query`** returns `pmix_pnet_nvd_module` at priority
   **10**.

`component_open` returns whatever `pmix_hwloc_check_vendor` gave it, and
only `PMIX_ERR_NOT_AVAILABLE` is the MCA's "silently ignore me" cue — the
helper's other two failures (`PMIX_ERR_BAD_PARAM` for no topology,
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
  `pmix_hwloc_check_vendor`, which applies the same source check and must
  have passed for this module to be active at all.
- **The envar-glob defaults are the interface to the comms stacks.**
  `UCX_*,HCOLL_*,UCC_*,SHARP_*,NCCL_*` are what get forwarded to compute
  nodes; changing them changes which runtime settings propagate.
