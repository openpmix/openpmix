<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `opa` Component

`opa` is the `pnet` component for Intel **Omni-Path** fabric support. It
and [`nvd`](../nvd/AGENTS.md) are the two `pnet` components compiled into
a default build; check `base/static-components.h` rather than trusting
any prose about it. Read the framework [`AGENTS.md`](../AGENTS.md) first;
this file covers only what is specific to `opa`. Compared with the other
components it is the closest thing to a live reference — it compiles
against the current module interface — but note it only becomes *active*
when Omni-Path hardware is present, so on most machines it is built but
never selected. `nvd` is a near-copy of it, so a defect found in one is
worth looking for in the other.

## Files

| File | Contents |
|------|----------|
| `pnet_opa.h` | Component struct type + blob/inventory key `#define`s. |
| `pnet_opa_component.c` | Component struct, `component_register`, `component_open` (hwloc gate), `component_query` (priority **10**). |
| `pnet_opa.c` | The module: `allocate` / `setup_local_network` / `setup_fork` / `collect_inventory` / `deliver_inventory`. |

There is deliberately **no `configure.m4`**: it used to hold
`AS_IF([test "yes" = "yes"], …)` — always succeed, and add an `OmniPath`
summary line — which is worth less than the file it lived in. The
component links nothing and always builds; whether it has work to do is
settled at run time by `component_open`.

## When it is selected

Selection is a two-step gate:

1. **`component_open`** returns `pmix_hwloc_check_vendor(&pmix_globals.topology,
   0x8086, 0x208)` — i.e. it only opens if hwloc finds an Intel
   (`0x8086`) PCI device of class `0x208` (an Omni-Path HFI). On any host
   without that device, `open` fails and the component is never queried.
2. **`component_query`** (reached only if open succeeded) unconditionally
   hands back `pmix_opa_module` at priority **10**.

Because `pnet` is multi-select, priority 10 only determines `opa`'s
position in the fan-out order relative to any other active component; it
does not exclude anyone.

## The module

```c
pmix_pnet_module_t pmix_opa_module = {
    .name = "opa",
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};
```

`init`, `finalize`, the cleanup hooks, and the fabric slots are `NULL`.
The signatures match the current `pmix_pnet_module_t`
(`setup_local_network` takes a `pmix_nspace_env_cache_t *`; the inventory
functions take a list / plain args, not callbacks).

## What the functions do

### `allocate` — build the launch blob

Runs on the "lead" server during `PMIx_server_setup_application`. It scans
the info array for `PMIX_SETUP_APP_ENVARS` / `PMIX_SETUP_APP_ALL` /
`PMIX_SETUP_APP_NONENVARS` and, nested inside a `PMIX_ALLOC_NETWORK`
array, for `PMIX_ALLOC_NETWORK_SEC_KEY` / `PMIX_ALLOC_NETWORK_ID`, to
decide two booleans: **seckeys** and **envars**. Then:

- **seckeys** → generate two 64-bit random words (from `/dev/urandom`,
  falling back to `pmix_srand`/`pmix_rand`), format them as a hex
  `NNNN-NNNN` string via `transports_print`, and pack it as the envar
  `OMPI_MCA_orte_precondition_transports`. This is the shared transport
  "pre-conditioning" key every process in the job must agree on.
- **envars** → `pmix_util_harvest_envars` using the component's
  `include`/`exclude` globs (default include `"HFI_*,PSM2_*"`), packing
  each matched `PMIX_ENVAR` into the buffer.

If nothing was packed, it returns `PMIX_SUCCESS` with no blob. Otherwise
it wraps the buffer in a `pmix_kval_t` keyed `PMIX_PNET_OPA_BLOB`,
compresses it with `pmix_compress.compress` (marking the type
`PMIX_COMPRESSED_BYTE_OBJECT` if compression happened), and appends it to
`ilist` for transmission to the compute-node daemons.

### `setup_local_network` — consume the blob

Runs on each daemon during `PMIx_server_setup_local_support`, after
`register_nspace` so the node/proc maps are available. It searches the
info array for `PMIX_PNET_OPA_BLOB`, decompresses it if needed, and
unpacks a stream of `PMIX_ENVAR`s. Each envar is appended to the
namespace's `ns->envars` cache (so the base `setup_fork` will inject it
into every child). As a special case, when it sees the
`OMPI_MCA_orte_precondition_transports` envar it also stores the value as
a `PMIX_CREDENTIAL` job-level key (`PMIX_GDS_STORE_KV` on the wildcard
rank) so it is queryable as job info, not just an envar.

### `setup_fork`

Names this process's HFIs to the fabric software that will use them. It
asks `pmix_pnet_base_set_assigned_devices()` for the process's devices
filtered by the same PCI `(vendor, class)` pair `component_open` probed
for — `0x8086` / `0x208`, which matters because Intel also makes ethernet
controllers and those report class `0x200` — and writes the result to
**`PSM3_NIC`**, which selects by NIC name.

PSM2's `HFI_UNIT` and OPX's `FI_OPX_HFI_SELECT` are deliberately **not**
set. Both take a unit *ordinal*, and an ordinal means something only
against the enumeration it came from, which is the driver's rather than
one PMIx performed. The trailing digit of `hfi1_0` looks like that
ordinal and usually is, but "usually" is the wrong standard: a wrong
value in these variables does not fail, it quietly puts the process on
somebody else's HFI. If hwloc ever records the unit the driver reported —
as it does for Level Zero devices, see `pgpu/intel` — this is where it
would be used.

### `collect_inventory` / `deliver_inventory`

Both are effectively **stubs** in the current code: `collect_inventory`
carries a comment about searching the topology for OPA NICs but returns
`PMIX_SUCCESS` without adding anything, and `deliver_inventory` returns
`PMIX_SUCCESS`. Do not describe `opa` as providing real inventory — it
does not yet.

Note that `PMIX_SUCCESS` is also the right answer for "no OPA NIC here"
if these ever do real work. Unlike `allocate` and `setup_fork`, the base
has no decline convention for the inventory calls: it `PMIX_ERROR_LOG`s
any non-`SUCCESS` return and abandons the fan-out, so a component with
nothing to say has to say it quietly.

## Testing it

`test/unit/pnet_opa_seckey.c` drives the seckey path end to end —
`allocate` → `setup_local_network` → `setup_fork` — plus the PCI filter
in `setup_fork` and the malformed-directive cases below. It gets `opa`
selected on a machine with no HFI by handing the server
`test/topologies/opa-hfi.xml` through the documented
`pmix_hwloc_topo_file` hook. That fixture exists because `opa` had no
coverage at all otherwise: the two general-purpose fixtures present no
Omni-Path device, and they cannot simply be given one — `hwloc_devices`
asserts exact device counts against both. It deliberately carries an
Intel *ethernet* controller (class `0200`) beside the HFI (class
`0208`), because "same vendor, wrong class" is the mistake the PCI
filter exists to prevent.

## Gotchas

- **`PMIX_ALLOC_NETWORK` is deprecated, and that is exactly why its shape
  must be checked.** `opa`'s `allocate` is the only reader of it left in
  the library, so a host that gets the type wrong is corrected by nobody.
  It is documented `(pmix_data_array_t*)`; reading a `PMIX_STRING`'s bytes
  as one yields a wild `array` pointer and a garbage `size`, and the walk
  segfaults. The nested scan therefore checks the value type, the array
  pointer, the array's *element* type and its contents before touching
  them — the same discipline `pmix_pnet_base_get_assigned_devices` applies
  to `PMIX_DEVICE_ID`, and for the same reason.
- **`pmix_compress.decompress()` writes nothing when it fails, and the
  failure is not exotic.** A node that built no compression component
  still receives compressed blobs from a lead server that did, and the
  base's stub decompressor simply returns `false` — as does a real one fed
  a damaged blob. Its return value must be checked before the result is
  read *or freed*: it leaves the output pointer untouched.
- **`allocate` hands the payload over exactly once.** `PMIX_UNLOAD_BUFFER`
  transfers the packed bytes to a `pmix_byte_object_t` without freeing
  anything, so when compression succeeds — and the `pmix_kval_t` therefore
  owns the *compressed* copy — the uncompressed block has to be freed by
  hand.
- **`transports_print` type-puns a `uint64_t` through `unsigned int *`,
  and that is safe here only because the whole tree is built
  `-fno-strict-aliasing`** (`config/pmix_setup_cc.m4` adds it wherever the
  compiler takes it). Do not copy the idiom into new code, and do not
  "fix" it as a live defect — it is not one in this build. The arithmetic
  around it is deliberately width-independent, so it survives an
  `unsigned int` of any size; the resulting string is byte-order
  dependent, which does not matter because the key is generated once on
  the lead server and shipped as a string.
- **Built ≠ active.** `opa` links into `libpmix` on every build, but the
  `component_open` hwloc probe means it only participates on hosts with an
  Intel Omni-Path HFI — so on a developer's machine every entry point here
  is dead code unless something puts an HFI in the topology. That is what
  the fixture under [Testing it](#testing-it) is for; reach for it rather
  than for the hardware.
- **The seckey/envar names are Open MPI / ORTE-specific.**
  `OMPI_MCA_orte_precondition_transports` is the historical key the
  consumers expect; it is not a generic PMIx attribute. Keep it verbatim
  if you touch the allocate/setup pair, or you will silently break
  pre-conditioning.
- **The blob key is the contract.** `PMIX_PNET_OPA_BLOB`
  (`"pmix.pnet.opa.blob"`) links `allocate` to `setup_local_network`;
  both ends must agree, and the compressed-vs-plain byte-object handling
  must stay symmetric.
- **Blob format is append-only across versions.** The blob is a raw
  `PMIX_ENVAR` stream (no leading count), read until end-of-buffer; a
  `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` is treated as normal
  completion. Preserve that shape for interoperability.
