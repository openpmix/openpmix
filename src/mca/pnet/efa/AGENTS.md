<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `efa` Component

`efa` names the Amazon **Elastic Fabric Adapter** devices a process was
mapped against to libfabric. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `efa`. It is the smallest
of the per-process components: it has a `setup_fork` and nothing else — no
envar harvesting, no inventory, no fabric plane.

## Files

| File | Contents |
|------|----------|
| `pnet_efa.h` | Component struct type; the match table's declaration. |
| `pnet_efa_component.c` | Component struct, `component_open` (topology gate), `component_query` (priority **10**). |
| `pnet_efa.c` | The match table and `setup_fork`. |

Like `nvd` and `opa` it ships **no `configure.m4`** and builds
unconditionally: it links nothing, and whether it has work to do is a
property of the node the daemon runs on, which only `component_open` can
know.

## When it is selected

`component_open` asks `pmix_pnet_base_check_devices()` about
`pmix_pnet_efa_match[]`, the same table `setup_fork` filters with:

| Vendor | Class | Type required |
|--------|-------|---------------|
| `0x1d0f` | any | OpenFabrics |

Amazon makes two network devices under that vendor id: **EFA**, which the
kernel registers as a verbs device (so hwloc reports it as an OpenFabrics
device, named like `rdmap16s27`), and **ENA**, a plain ethernet interface.
The type is the only thing that separates them, so the entry requires it
and leaves the class open. An instance with ENA and no EFA therefore does
not open the component.

## `setup_fork`

Writes `FI_EFA_IFACE` with the selectors of the process's EFA devices,
comma-separated, through `pmix_pnet_base_set_assigned_devices()`.

- **The match is exact.** libfabric's efa provider
  (`efa_env_allows_nic()` in `prov/efa/src/efa_env.h`) compares each
  comma-separated entry against the verbs device name as a whole token,
  so — unlike `FI_VERBS_DEVICE_NAME`, see [`nvd`](../nvd/AGENTS.md) — the
  variable cannot select a device it does not name.
- **Everything in the process sees it.** The restriction is applied
  inside the provider, so an MPI library that chooses its NICs on its own
  (Open MPI's `btl/ofi` slices them by local rank) is only offered the
  assigned devices.
- **It is overwritten if already set**, as the other components'
  variables are: a process mapped against a device made the more specific
  request.

EFA reports its GUIDs as all zeros. That matters to the uuid PMIx builds
for it (see `build_device_uuid()` in `src/hwloc/pmix_hwloc.c`), not here:
the selector is the OS device name.

## Testing

`test/unit/pnet_rdma_fabrics.c` runs against `test/topologies/rdma-fabrics.xml`,
which carries an EFA function beside an ENA one: the EFA device must be
named in `FI_EFA_IFACE`, the ENA interface must not, and neither may reach
the verbs libraries' variables. That fixture is synthetic; it should be
checked against a real `lstopo --of xml` from an EFA instance.
