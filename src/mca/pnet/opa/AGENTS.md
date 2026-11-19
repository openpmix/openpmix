<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `opa` Component

`opa` is the `pnet` component for **Omni-Path** fabric support, a
product of Cornelis Networks (formerly of Intel): OPA100, CN5000, and
later generations. Every one of them is driven by the `hfi1` kernel driver
and, in user space, by **OPX** — libfabric's `prov/opx` provider — which
is what this component writes for. (PSM2 also drives OPA100, and the
transport key Open MPI's PSM2 MTL reads is still provided; see below.
PSM3 is **not** an Omni-Path library — it is Intel's, for Intel's
ethernet adapters — and nothing here sets anything for it.) It and
[`nvd`](../nvd/AGENTS.md) are the two `pnet` components compiled into a
default build; check `base/static-components.h` rather than trusting any
prose about it. Read the framework [`AGENTS.md`](../AGENTS.md) first;
this file covers only what is specific to `opa`. Compared with the other
components it is the closest thing to a live reference — it compiles
against the current module interface — but note it only becomes *active*
when Omni-Path hardware is present, so on most machines it is built but
never selected. `nvd` is a near-copy of it, so a defect found in one is
worth looking for in the other.

## Files

| File | Contents |
|------|----------|
| `pnet_opa.h` | Component struct type, blob/inventory key `#define`s, the match table's declaration. |
| `pnet_opa_component.c` | Component struct, `component_register`, `component_open` (topology gate), `component_query` (priority **10**). |
| `pnet_opa.c` | The match table and the module: `allocate` / `setup_local_network` / `setup_fork` / `deregister_nspace` / `finalize` / `collect_inventory` / `deliver_inventory`. |

There is deliberately **no `configure.m4`**: it used to hold
`AS_IF([test "yes" = "yes"], …)` — always succeed, and add an `OmniPath`
summary line — which is worth less than the file it lived in. The
component links nothing and always builds; whether it has work to do is
settled at run time by `component_open`.

## When it is selected

Selection is a two-step gate:

1. **`component_open`** asks `pmix_pnet_base_check_devices()` about
   `pmix_pnet_opa_match[]`, the same table `setup_fork` filters a
   process's assignment with:

   | Vendor | Class | What it is |
   |--------|-------|------------|
   | `0x434e` | `0x208` | Cornelis CN5000 and later (`434e:0001` on CN5000) |
   | `0x8086` | `0x208` | OPA100 (`8086:24f0`) |

   **Both are needed.** CN5000 reports Cornelis's own vendor id —
   `test/topologies/cn5k-hfi.xml`, a real CN5000 node, shows it — and the
   component used to look for Intel's alone, so it never opened on one.
   OPA100 carries the id Intel gave it; that is in the silicon, so the
   cards in service still report it although Intel no longer has anything
   to do with the product. Do not drop either entry. The class is part of
   the question: Intel's ethernet controllers report `0x200` and are not
   ours. On a host with neither HFI, `open` returns
   `PMIX_ERR_NOT_AVAILABLE` and the component is silently left closed.
2. **`component_query`** (reached only if open succeeded) unconditionally
   hands back `pmix_pnet_opa_module` at priority **10**.

Because `pnet` is multi-select, priority 10 only determines `opa`'s
position in the fan-out order relative to any other active component; it
does not exclude anyone.

## The module

```c
pmix_pnet_module_t pmix_pnet_opa_module = {
    .name = "opa",
    .finalize = finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .deregister_nspace = deregister_nspace,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};
```

`.finalize` and `.deregister_nspace` release the per-job OPX keys
described below. `init`, the other cleanup hooks, and the fabric slots are
`NULL`.
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

  The fallback path is reached whenever `/dev/urandom` cannot be opened
  or short-reads — a chroot or container with a restricted `/dev`, or a
  process out of descriptors. So `transports_use_rand` **seeds once and
  keeps drawing from that stream**: it used to re-seed from `time(NULL)`
  on every call, which handed two jobs allocated inside the same second
  the same key. It also fills both
  halves of each 64-bit word, because `pmix_rand` returns 32 bits at a
  time and the caller is asking for 128. `pnet/tcp`'s `generate_key` is
  the same code and carries the same fix.
  The same request also packs **`FI_OPX_UUID`**, OPX's job key, right
  after it. OPX names its intra-node shared-memory segments with that key
  and hands it to the driver as the context uuid, so two processes reach
  each other through shared memory only when their keys agree; unset, OPX
  takes `SLURM_JOB_ID`, and failing that a fixed built-in default
  (`OPX_DEFAULT_JOB_KEY_STR`). Open MPI's OFI path never sets it. The key belongs
  to **a job and the jobs it spawns**, and to nothing else
  (`opx_job_uuid()`): a job whose directives name the process that
  spawned it (`PMIX_PARENT_ID`) takes the key this server recorded for
  that process's job, and any other job gets 128 fresh random bits. The
  key is recorded under the job's namespace on this server — the same
  `pnet_opa_uuid_t` list the daemons use, below — so a job spawned from a
  spawned job inherits it in turn; a parent is running when it spawns, so
  its job is still registered. Setting a job up again gives it the same
  key. Not session scope, deliberately: a PRRTE session is an allocation
  that may run many unrelated jobs, and sharing the key shares OPX's
  shared-memory namespace and the driver's context grouping. **Jobs
  launched apart do not share a key even when they later connect.** OPX
  takes one key per domain, reads it from the environment when the domain
  opens, and uses it for every peer — it accepts nothing through
  libfabric's `auth_key` interface and its peer address carries no key — so
  a key delivered at connect time has nowhere to go. A user who needs
  shared memory between such jobs gives both the same `FI_OPX_UUID`, which
  `setup_fork` keeps. The key is printed as 32 hex digits, the most OPX
  accepts. A daemon that predates this simply caches it as one more
  envar.
- **envars** → `pmix_util_harvest_envars` using the component's
  `include`/`exclude` globs (default include `"HFI_*,PSM2_*,FI_OPX_*"`),
  packing each matched `PMIX_ENVAR` into the buffer. These come after the
  keys, so an `FI_OPX_UUID` the user set where the job was launched
  arrives later in the blob than the generated one.

If nothing was packed, it returns `PMIX_SUCCESS` with no blob. Otherwise
it wraps the buffer in a `pmix_kval_t` keyed `PMIX_PNET_OPA_BLOB`,
compresses it with `pmix_compress.compress` (marking the type
`PMIX_COMPRESSED_BYTE_OBJECT` if compression happened), and appends it to
`ilist` for transmission to the compute-node daemons.

### `setup_local_network` — consume the blob

Runs on each daemon during `PMIx_server_setup_local_support`, after
`register_nspace` so the node/proc maps are available. It searches the
info array for `PMIX_PNET_OPA_BLOB`, decompresses it if needed, and
unpacks a stream of `PMIX_ENVAR`s. Each envar but one is appended to the
namespace's `ns->envars` cache (so the base `setup_fork` will inject it
into every child). The exception is `FI_OPX_UUID`: the base sets that
cache **with overwrite**, and a key the child's environment already
carries — one the user gave the app — must win, so it is kept instead on
the component's own per-namespace list (`pnet_opa_uuid_t`), a later one
replacing an earlier, and `setup_fork` sets it without overwriting. The
list entry goes at `deregister_nspace` — on the server that set the job
up as well, where `allocate` recorded it — and the whole list at
`finalize`.
The cache does carry the transport key,
`OMPI_MCA_orte_precondition_transports`, and it goes nowhere else: it is
**not** stored as job data (it once was, as a `PMIX_CREDENTIAL`
job-level key). Open MPI's PSM2 MTL asks for that key first and falls
back to the environment variable, so it still finds the key.
`test/unit/pnet_opa_seckey.c` checks that the job holds no
`PMIX_CREDENTIAL` after setup.

### `setup_fork`

First, for every process of a job that has one, it sets `FI_OPX_UUID`
without overwriting (`PMIX_ERR_EXISTS` is not an error). OPX needs the key
whether or not the process was mapped against an HFI.

Then it names this process's HFI. It asks
`pmix_pnet_base_get_assigned_devices()` for the process's devices filtered
by `pmix_pnet_opa_match[]` — which matters because Intel also makes
ethernet controllers, and they report class `0x200` — and, **for a process
given exactly one**, writes **`FI_OPX_HFI_SELECT`** over any value already
set.

The variable takes a unit *number*, and that is not an ordinal here:
every path by which OPX opens a unit builds the device's name from the
number — `/dev/hfi1_<N>`, `/sys/class/infiniband/hfi1_<N>`, the verbs
device `hfi1_<N>` (`fi_opx_service.c`, `fi_opx_sysfs.c`,
`opx_hfi1_rdma_core.c`) — so the number in the name is the unit by
construction, with no enumeration of OPX's own behind it. `opx_unit_of()`
accepts exactly `hfi1_` followed by decimal digits with no leading zero.
Two cases are left unset:

- **another device's name begins with this one's.** OPX's verbs path
  finds the unit's device by `strncmp` over the length of `hfi1_<N>` and
  takes the first in the device list, so unit 1 also matches
  `hfi1_10`–`hfi1_15`, depending on list order. `opx_prefix_collides()`
  refuses then — the same shape, and the same answer, as `nvd`'s
  `verbs_prefix_collides()`.
- **several HFIs.** OPX opens the first fixed unit it reads and stops, so
  a list says "the first of these", not "these".

On CN6000 OPX may open a second, send-only context on an HFI of the other
plane, which it discovers itself whatever `FI_OPX_HFI_SELECT` says; that
is OPX's choice and not one this variable can narrow.

A device the process was mapped against under its **network interface**
name — CN5000's HFI function carries `ibp161s0` and `ibp161s0d1` beside
`hfi1_0` — reaches this code as `hfi1_0`: the base names a PCI function by
its preferred OS device. `test/unit/pnet_opa_cn5k.c` covers it.

PSM2's `HFI_UNIT` is deliberately **not** set. PSM2 numbers units by its
own enumeration, which nothing here can see, and a wrong value there does
not fail — it quietly puts the process on somebody else's HFI.

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

Two programs, one per fixture, because the topology is read once per
process:

- `test/unit/pnet_opa_seckey.c` drives the seckey path end to end —
  `allocate` → `setup_local_network` → `setup_fork` — plus the OPX key's
  scoping (inherited by a spawned job and by one that job spawns, not
  shared by jobs launched apart or by one naming a parent the server set
  up no job for, the same on a second setup, never written over a value
  already set, a launcher's own value preferred, forgotten at
  deregistration), the PCI filter in
  `setup_fork`, both selection variables, and the malformed-directive
  cases below. It gets `opa` selected on a machine with no HFI by handing
  the server `test/topologies/opa-hfi.xml` through the documented
  `pmix_hwloc_topo_file` hook. That fixture carries three OPA100 HFIs —
  `hfi1_0`, `hfi1_1` and `hfi1_10`, the last so that unit 1 collides —
  and an Intel *ethernet* controller (class `0200`), because "same
  vendor, wrong class" is the mistake the PCI filter exists to prevent.
  The general-purpose fixtures cannot simply be given an HFI:
  `hwloc_devices` asserts exact device counts against both.
- `test/unit/pnet_opa_cn5k.c` runs against `test/topologies/cn5k-hfi.xml`,
  a real CN5000 node with its identifying values replaced: that `opa`
  opens for Cornelis's vendor id at all (it fails, not skips, if not — the
  fixture has the HFI), that the HFI is named to OPX whether the process
  was given it as `hfi1_0` or as `ibp161s0`, that `PSM3_NIC` is not set,
  and that the node's Mellanox ports are left to `nvd`.

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
- **`setup_fork` runs on the host's thread, and reads the OPX key list.**
  That list is written by `setup_local_network` and `deregister_nspace` on
  the progress thread. It is safe for the same reason the base's envar
  cache is — the host forks a job's processes after that job's
  `setup_local_support` completes and deregisters it after they exit —
  and unsafe in the same way if a host does not; see the framework
  `AGENTS.md`'s threading section.
- **Built ≠ active.** `opa` links into `libpmix` on every build, but the
  `component_open` probe means it only participates on hosts with an
  Omni-Path HFI — so on a developer's machine every entry point here
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
