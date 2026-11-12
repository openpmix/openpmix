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
else.

## Files

| File | Contents |
|------|----------|
| `pnet_cxi.h` | Component struct type; the match table's declaration. |
| `pnet_cxi_component.c` | Component struct, `component_open` (topology gate), `component_query` (priority **10**). |
| `pnet_cxi.c` | The match table, the name check, and `setup_fork`. |

No `configure.m4`: it builds unconditionally and decides at run time.

## When it is selected

`component_open` asks `pmix_pnet_base_check_devices()` about
`pmix_pnet_cxi_match[]`, the same table `setup_fork` filters with:

| Vendor | Class | Type required |
|--------|-------|---------------|
| `0x17db` | any | network |

**The vendor id is not yet confirmed.** `0x17db` is Cray's, and the
Cassini parts are expected to report it, but no topology from a real
Slingshot node has been checked. Do that before relying on this
component, and update the table and this file with what it shows.

## `setup_fork`

Writes `FI_CXI_DEVICE_NAME` with the process's Slingshot NICs,
comma-separated — but only when **every** name is one the provider
accepts: `cxi` followed by 0–99 without a leading zero, the test
`cxip_env_validate_device_name()` in libfabric's `prov/cxi/src/cxip_info.c`
applies. The provider refuses the whole variable otherwise, so a wrong
value does not narrow the selection, it stops the provider from starting.

The selector is a `cxiN` name only because of two things outside this
component:

- **hwloc 2.14 or later.** Earlier versions report a Cassini function only
  as its `hsnN` interface. 2.14 adds the CXI device from `/sys/class/cxi`
  on the same function.
- **The enumerator's preference.** Both are network devices, and
  `osdev_preferred()` in `src/hwloc/pmix_hwloc.c` names the function by
  the CXI device, identified by the `CassiniVersion` / `NID` infos hwloc
  records only on it (see [`src/hwloc/AGENTS.md`](../../../hwloc/AGENTS.md)).

So under an older hwloc, or for a process mapped against a device named
by its interface (`hsn0`), the selector is `hsnN` and the variable is left
unset, with a verbose message. Turning `hsn0` into `cxi0` would be a
guess: they usually correspond, but nothing guarantees it. If that case
needs covering, the way to do it is to read the CXI device off the *same
PCI function* in the local topology, not to rewrite the name.

## Testing

`test/unit/pnet_rdma_fabrics.c` runs against `test/topologies/rdma-fabrics.xml`,
which carries one Cassini function with both OS devices (interface listed
first) and one with only the interface. The first is named `cxi0`; the
second, and the first asked for as `hsn0`, are left unset. The fixture is
synthetic; check it against a real Slingshot node's `lstopo --of xml`.
