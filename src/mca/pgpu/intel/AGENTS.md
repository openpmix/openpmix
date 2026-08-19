<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU `intel` Component

`intel` is the `pgpu` component that provides Intel-GPU support: telling a
process which Intel GPUs it was assigned, harvesting Intel-relevant
environment variables for a job, and (eventually) reporting Intel GPU
inventory. Read the framework [`AGENTS.md`](../AGENTS.md) first; this file
covers only what is specific to `intel`.

Its device-assignment path is the substantial one, and it is the reason
this component is not a copy of `nvd` with the names changed: Intel is the
vendor whose selection variable does not accept a device identity.

## Files

| File | Contents |
|------|----------|
| `pgpu_intel.h` | Component struct `pmix_pgpu_intel_component_t`, module extern, and the blob/inventory key `#define`s. |
| `pgpu_intel_component.c` | Component struct + open/close/register/query. Priority **20**. |
| `pgpu_intel.c` | The module: `allocate`, `setup_local`, `setup_fork`, and the two inventory stubs. |
| `help-pgpu-intel.txt` | The one user-facing message: `hierarchy-mismatch`. |

There is no `configure.m4`, deliberately — see Availability.

## Availability

The component **builds unconditionally, and ships no `configure.m4`**. It
links nothing and needs no vendor SDK; it reads info attributes hwloc
already recorded and sets environment variables, so there is nothing for
`configure` to look for, and the machine that builds PMIx is routinely
not the machine that runs it. The MCA machinery configures a component
with no `configure.m4` on its own.

The question is settled at **run** time: `component_open` returns
`pmix_hwloc_check_vendor(&pmix_globals.topology, 0x8086, 0x0380)`, which
succeeds only if the node topology contains a PCI device with Intel's
vendor ID `0x8086` and display class `0x0380`. The topology is loaded
before the server opens this framework, so `component_open` really can
see it; absent matching hardware (or a non-hwloc topology) the component
declines to open and is never queried. `component_query` therefore does
no checking of its own — it sets priority **20** and hands back the
module.

## Component struct and MCA params

`pmix_pgpu_intel_component_t` extends the base component with
`incparms`/`excparms` and their split `include`/`exclude` argvs.
`component_register` registers:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_intel_include_envars` | `NULL` | comma-delimited glob list of envars to harvest |
| `pgpu_intel_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

With the default `NULL` include list, `allocate` harvests nothing. Note
that this is not an oversight to be fixed by adding `ZE_*` to the default:
the harvest happens on the node running the *launcher* and is replayed
into every rank of the job, so a launcher whose environment happened to
carry a `ZE_AFFINITY_MASK` would broadcast it to the whole job. Anything
added here has to be safe to say to every rank on every node.

## Device assignment: `setup_fork`

This is the part worth understanding before changing anything.

**What the variable takes.** `ZE_AFFINITY_MASK` is a comma-separated list
of Level Zero device ordinals, each optionally `device.subdevice`. There
is no identity form of the kind `CUDA_VISIBLE_DEVICES` and
`ROCR_VISIBLE_DEVICES` accept. The [Level Zero
specification](https://oneapi-src.github.io/level-zero-spec/level-zero/latest/core/PROG.html#affinity-mask)
is the authority for the grammar.

**Why an ordinal is legitimate here.** The component used to set nothing,
reasoning that an ordinal depends on an ordering PMIx could not
reproduce. It does not have to reproduce it: hwloc's Level Zero backend
records `LevelZeroDriverDeviceIndex` — the index `zeDeviceGet` handed back
— on every root device. So the ordinal is *read* from the enumeration,
not predicted. And it is read at fork time, on the node that will run the
process, from that node's own topology.

**Why the hierarchy model has to be stated with it.** The spec is explicit
that a driver reads `ZE_FLAT_DEVICE_HIERARCHY` first and then interprets
`ZE_AFFINITY_MASK` against the devices that model exposes. Under
`COMPOSITE` a root device is a whole card (its tiles are sub-devices);
under `FLAT` the tiles themselves are the root devices. The same ordinal
therefore names different hardware under the two, so the ordinal alone is
half a statement. `setup_fork`:

- writes `ZE_FLAT_DEVICE_HIERARCHY` when the process has not been given
  one, naming the model this node enumerated under. Without it the
  process takes its own runtime's default, which need not match the
  daemon's — different builds, and in a container different installs.
- **writes nothing at all** when the process has been given a model that
  contradicts it, and says so through `help-pgpu-intel.txt`. Overriding
  an explicit choice would change how many devices the program sees;
  writing the mask anyway would silently hand it half the hardware it was
  assigned. Dropping the assignment leaves the process seeing every
  device it could see before, which is the pre-existing behavior and the
  only harmless one.

`COMBINED` counts as agreeing with `FLAT`: it differs only in whether a
tile can be navigated back to its card, and `zeDeviceGet` reports the same
devices in the same order under both. That is what `hierarchy_agrees()`
encodes — the question is only ever "composite or not".

**Where the pieces live.** The component itself is short, because the
work is split by what knows what:

- `pmix_hwloc_get_devices()` fills each device's `selector` — for Intel,
  the ordinals of every Level Zero root device on that card's PCI
  function. *Every* one, which is what makes the value mean "this card"
  under either model: one ordinal under `COMPOSITE`, two under `FLAT`
  where the tiles are the root devices. It yields NULL rather than a
  partial answer for a device behind a second Level Zero driver, since
  `ZE_AFFINITY_MASK` has no way to name a driver.
- `pmix_hwloc_levelzero_hierarchy()` reports the model, or declines when
  the topology cannot tell — which happens exactly when the models do not
  differ (no card has more than one tile).
- `pmix_pgpu_base_get_visible_devices()` does the shared work: read the
  proc's `PMIX_DEVICE_ID`, resolve each device locally, keep this
  vendor's, join their selectors. The component uses the *getter* rather
  than `pmix_pgpu_base_set_visible_devices()` precisely because it must
  check the environment before writing.

The mask itself is overwritten if already set, like the other vendor
components' variables: a process asking to be mapped against a device has
made the more specific request, and the ordinals come from the topology
as seen through any narrowing already in force.

## The other module functions

- **`allocate`** — returns `PMIX_ERR_TAKE_NEXT_OPTION` if `info == NULL`,
  if neither `PMIX_SETUP_APP_ENVARS` nor `PMIX_SETUP_APP_ALL` was
  requested, or if the include list is empty — which, given this
  component's `NULL` default, is the usual outcome unless someone set
  `pgpu_intel_include_envars`. Declining is deliberate: appending an
  empty blob would ship one to every daemon in the job to say the same
  thing. Otherwise it harvests envars via `pmix_util_harvest_envars`,
  packs them as `PMIX_ENVAR`, compresses, and appends the result to
  `ilist` under `PMIX_PGPU_INTEL_BLOB` (`"pmix.pgpu.intel.blob"`) as a
  `PMIX_COMPRESSED_BYTE_OBJECT` (or `PMIX_BYTE_OBJECT` if uncompressible).
- **`setup_local`** — finds `PMIX_PGPU_INTEL_BLOB` in `info`, decompresses
  if needed — checking the **bool** `pmix_compress.decompress` returned
  before touching its output, see the framework
  [`AGENTS.md`](../AGENTS.md#the-decompress-return-is-not-optional) — and
  unpacks the `PMIX_ENVAR`s into `ns->envars` for later replay into local
  children by the base `setup_fork`.
- **`collect_inventory` / `deliver_inventory`** — **stubs** that
  `PMIX_HIDE_UNUSED_PARAMS(...)` and `return PMIX_SUCCESS`.

## Tests

`test/unit/pgpu_affinity_mask` drives the whole assignment path against
`test/topologies/intel-4gpu.xml` — an Aurora node cut to four cards, with
both an OpenCL and a Level Zero view of each, so a value only appears if
the ordinals were found across the PCI function rather than on the device
that names it. `test/unit/hwloc_devices` covers the selector and the
hierarchy report directly, against both `intel-4gpu.xml` (COMPOSITE) and
`intel-flat-4gpu.xml` (FLAT) — the same machine under the two models,
which is the pair that catches an ordinal computed for the wrong one.

## Gotchas

- Keep `PMIX_PGPU_INTEL_BLOB` / `PMIX_PGPU_INTEL_INVENTORY_KEY` unique
  across components; `setup_local` claims its data by matching the blob
  key string.
- Touching `help-pgpu-intel.txt` — including adding a topic — requires
  `rm src/util/pmix_show_help_content.*` and a rebuild, or the library
  keeps emitting the old text.
- The inventory functions really are no-ops. Do not describe them as
  doing work.
- An ordinal is not an identity. Do not let one leak into anything that
  reports *which* device a process has (`PMIX_DEVICE_ID`, the uuid, the
  distance arrays); it is only good for saying which devices a Level Zero
  driver should expose, to a process, under a stated hierarchy model.
