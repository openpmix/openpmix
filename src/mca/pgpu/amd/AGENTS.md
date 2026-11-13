<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU `amd` Component

`amd` is the `pgpu` component for AMD GPUs: it names a process's
assigned GPUs in `ROCR_VISIBLE_DEVICES`, harvests AMD-relevant
environment variables for a job, and (eventually) will report AMD GPU
inventory. Read the framework [`AGENTS.md`](../AGENTS.md) first; this
file covers only what is specific to `amd`. It is built in every
configuration and opens only on a node with AMD GPUs (see
Availability); its inventory logic is stubbed.

## Files

| File | Contents |
|------|----------|
| `pgpu_amd.h` | Component struct `pmix_pgpu_amd_component_t`, module extern, and the blob/inventory key `#define`s. |
| `pgpu_amd_component.c` | Component struct + open/close/register/query. Priority **20**. |
| `pgpu_amd.c` | The module: `allocate`, `setup_local`, `setup_fork`, and the two inventory stubs. |

## Availability

There is **no `configure.m4` and no build gate**: the component links
nothing and needs no vendor SDK, so it builds everywhere and there is
nothing for `configure` to look for.

The question is settled at **run** time. `component_open` asks
`pmix_hwloc_check_vendor_baseclass()` whether the node topology contains a
PCI device with AMD's GPU vendor ID `0x1002` in the display base class
`0x03`, and failing that in the processing-accelerator base class `0x12`.
Absent matching hardware (or a non-hwloc topology) the component declines
to open and is never queried.

Three details of that question each cost the component every node it
should have opened on:

- **`0x1002`, not `0x1022`.** The latter is this vendor's id on its CPU
  and chipset functions; the GPUs carry the id ATI brought with it.
- **The base class, not an exact subclass.** An older Instinct part
  reports `0x0380` and a Radeon `0x0300`, and both are this vendor's
  GPUs, so there is no one subclass to match.
- **Not only the display class.** An MI350 reports `0x1200`, a processing
  accelerator, and has no display-class function at all.
  `test/topologies/mi350-bcm57608.xml` is such a node, and
  `test/unit/pgpu_accelerator_class.c` checks that its GPUs are named.

Opening is all the class decides. Which PCI functions are GPUs is the
device enumeration's call (`pmix_hwloc_get_devices()`, which counts a
function by its vendor compute backend), and `setup_fork` names only
devices whose vendor identity is AMD's, so a broad open names nothing
that is not an AMD GPU.

`component_query` sets priority **20** and hands back
`pmix_pgpu_amd_module`; it does no checking of its own, because a
component that declined to open is never queried.

## Component struct and MCA params

`pmix_pgpu_amd_component_t` extends the base component with
`incparms`/`excparms` (the raw param strings) and `include`/`exclude`
(their `PMIx_Argv_split` argv forms). `component_register` registers:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_amd_include_envars` | `NULL` | comma-delimited glob list of envars to harvest |
| `pgpu_amd_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

With the default `NULL` include list, `allocate` harvests nothing —
`amd` would have to be given an explicit include list to do any work.

## The module functions

`pmix_pgpu_amd_module` fills only `name`, `allocate`, `setup_local`,
`collect_inventory`, and `deliver_inventory`; all other slots are `NULL`.

- **`allocate`** — called (via `pmix_pgpu_base_allocate`) at
  `PMIx_server_setup_application`. Returns `PMIX_ERR_TAKE_NEXT_OPTION` if
  `info == NULL`, if neither `PMIX_SETUP_APP_ENVARS` nor
  `PMIX_SETUP_APP_ALL` was requested, or if `component.include` is empty
  — which, given this component's `NULL` default include list, is the
  usual outcome unless someone set `pgpu_amd_include_envars`. Declining
  is deliberate: appending an empty blob would ship one to every daemon
  in the job to say the same thing. Otherwise it harvests envars matching
  `component.include` (minus `component.exclude`) via
  `pmix_util_harvest_envars`, packs each as `PMIX_ENVAR` into a scratch
  `pmix_buffer_t`, compresses the buffer with `pmix_compress.compress`,
  and appends it to `ilist` as a `pmix_kval_t` keyed `PMIX_PGPU_AMD_BLOB`
  (`"pmix.pgpu.amd.blob"`), typed `PMIX_COMPRESSED_BYTE_OBJECT` if
  compression succeeded, else `PMIX_BYTE_OBJECT`.
- **`setup_local`** — called at `PMIx_server_setup_local_support` on the
  backend. Scans `info` for `PMIX_PGPU_AMD_BLOB`, decompresses it if it is
  a compressed byte object — checking the **bool**
  `pmix_compress.decompress` returned before touching its output, see the
  framework [`AGENTS.md`](../AGENTS.md#the-decompress-return-is-not-optional)
  — loads it into a buffer, and unpacks
  `PMIX_ENVAR`s in a loop, appending each to `ns->envars`. Those cached
  envars are later injected into each local child by the base
  `setup_fork`. (Note the loop allocates one extra `pmix_envar_list_item_t`
  past the last successful unpack and `PMIX_RELEASE`s it — expected.)
- **`collect_inventory` / `deliver_inventory`** — **stubs**. Both
  `PMIX_HIDE_UNUSED_PARAMS(...)` and `return PMIX_SUCCESS`; the comments
  ("search the topology for AMD GPUs" / "look for our inventory blob")
  describe intent, not implemented behavior.

## Gotchas

- Its device assignment (`setup_fork` → `ROCR_VISIBLE_DEVICES`) is live,
  but its default include list is empty and its inventory functions do
  nothing. Do not describe those as working.
- **The value is not hwloc's `AMDUUID`.** ROCm's filter (`RvdFilter` in
  ROCR's `amd_filter_device.cpp`) takes an entry beginning with `G` as a
  prefix of `GPU-%016llX` of the device's unique id and parses anything
  else as an index, and the first entry matching nothing ends the list.
  hwloc writes the id as bare hex (its RSMI path) or inside an AMD SMI
  UUID (its AMD SMI path, hwloc 2.15 and later), and either one as it
  stands makes the process see no GPU. `amd_selector()` in
  `src/hwloc/pmix_hwloc.c` re-spells the id; `test_amd_selector` in
  `test/unit/hwloc_devices.c` checks each form against ROCm's rule.
- **A partitioned GPU is not named.** Each partition is a KFD node with a
  unique id of its own, taken from its first XCD, while the id hwloc read
  is the whole device's. An identity recording a partition other than the
  first, or an id more than one GPU on the node reports, is therefore
  declined, which leaves every GPU visible rather than none. How rsmi and
  AMD SMI report a partition has not been checked on hardware.
- The blob key `PMIX_PGPU_AMD_BLOB` and inventory key
  `PMIX_PGPU_AMD_INVENTORY_KEY` (`pgpu_amd.h`) must stay unique across
  components — the `setup_local` search keys on the blob string to claim
  its own data.
- What is left to do here is a sensible default include list and the
  inventory functions. Do **not** answer either with a `configure.m4`
  gate: whether this component has work to do is a property of the node
  the daemon runs on, and the build host is routinely not that node.
