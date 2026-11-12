<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `cxi` Component

`cxi` names the HPE **Slingshot** (Cassini) NICs a process was mapped
against to libfabric's CXI provider. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `cxi`. Like [`efa`](../efa/AGENTS.md) it has a `setup_fork` and nothing
else, plus one parameter, `pnet_cxi_sysfs_root`.

## Files

| File | Contents |
|------|----------|
| `pnet_cxi.h` | Component struct type (with `sysfs_root`); the match table's declaration. |
| `pnet_cxi_component.c` | Component struct, `component_register` (`sysfs_root`), `component_open` (topology gate), `component_query` (priority **10**). |
| `pnet_cxi.c` | The match table, the name check, the sysfs lookup, and `setup_fork`. |

No `configure.m4`: it builds unconditionally and decides at run time.

## When it is selected

`component_open` asks `pmix_pnet_base_check_devices()` about
`pmix_pnet_cxi_match[]`, the same table `setup_fork` filters with:

| Vendor | Class | Type required |
|--------|-------|---------------|
| `0x17db` | any | network |

Cassini reports Cray's vendor id: `test/topologies/slingshot.xml`, taken
on an HPE Cray EX235n, shows each NIC as PCI `17db:0501`, class `0x200`.
The class is left open in the table all the same; the NIC is a network
device either way.

## `setup_fork`

Writes `FI_CXI_DEVICE_NAME` with the process's Slingshot NICs,
comma-separated — but only when **every** name is one the provider
accepts: `cxi` followed by 0–99 without a leading zero, the test
`cxip_env_validate_device_name()` in libfabric's `prov/cxi/src/cxip_info.c`
applies. The provider refuses the whole variable otherwise, so a wrong
value does not narrow the selection, it stops the provider from starting.

Where the name comes from depends on the hwloc that took the topology:

- **hwloc 2.14 or later** reports the CXI device from `/sys/class/cxi` on
  the same PCI function as the `hsnN` interface, and the enumerator names
  the function by it — `osdev_preferred()` in `src/hwloc/pmix_hwloc.c`
  identifies it by the `CassiniVersion` / `NID` infos hwloc records only
  on it (see [`src/hwloc/AGENTS.md`](../../../hwloc/AGENTS.md)). The
  selector is already `cxiN`.
- **An older hwloc** reports only `hsnN`; `slingshot.xml` (hwloc 2.10.0)
  is that case. So is a process mapped against a device named by its
  interface. `setup_fork` then looks the CXI device up where hwloc 2.14
  itself finds it: the interface's PCI device carries a `cxi` class
  directory, `<root>/class/net/<hsnN>/device/cxi/`, whose entry is the CXI
  device. This runs on the node that forks the process, so it reads that
  node's own tree.

The lookup takes **exactly one** entry of the form `cxiN`. None, more than
one, or a directory that cannot be read is no answer, and if any of the
process's NICs has no answer the variable is left unset rather than
written short, with a verbose message naming the interface and the path.
The number in `hsn0` is **never** used to make up `cxi0`: the two usually
correspond, and nothing guarantees it. The root of the tree is the
`pnet_cxi_sysfs_root` parameter (default `/sys`), which exists so the
lookup can be run against a captured tree, as `nvd`'s
`pnet_nvd_sysfs_root` does for node types.

## Testing

- `test/unit/pnet_cxi_sysfs.c` runs against `test/topologies/slingshot.xml`,
  a real node's topology from hwloc 2.10.0, with a captured sysfs tree
  (`test/unit/pnet_sysfs.h`): `hsn0` resolves to `cxi0`; an interface with
  no `cxi` directory, and one listing two CXI devices, are declined; a
  non-CXI entry beside the CXI device is ignored; two resolved interfaces
  are both named; one unresolved interface among two leaves the variable
  unset; the Intel management port is not claimed. `hsn3`'s CXI device
  is `cxi5` and `hsn1` has none, so a lookup that derived the name from
  the interface's number would fail.
- `test/unit/pnet_rdma_fabrics.c` runs against the synthetic
  `test/topologies/rdma-fabrics.xml`, which carries one Cassini function
  with both OS devices (interface listed first) and one with only the
  interface, and a sysfs tree with no `class/net` entries. The first is
  named `cxi0`; the second, and the first asked for as `hsn0`, are left
  unset.
