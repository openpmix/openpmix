<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU `nvd` Component

`nvd` is the `pgpu` component intended to provide NVIDIA-GPU support:
harvesting NVIDIA-relevant environment variables (CUDA/NCCL) for a job and
(eventually) reporting NVIDIA GPU inventory. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `nvd`. It is **built everywhere** and declines at run time on a host
with no NVIDIA GPU (see Availability); its inventory logic is stubbed, but
its envar-harvesting and device-assignment paths are live and run through
the shared launch wiring. It is the one component with a non-empty default
include list (`CUDA_*,NCCL_*`).

## Files

| File | Contents |
|------|----------|
| `pgpu_nvd.h` | Component struct `pmix_pgpu_nvd_component_t`, module extern, and the blob/inventory key `#define`s. |
| `pgpu_nvd_component.c` | Component struct + open/close/register/query. Priority **10**. |
| `pgpu_nvd.c` | The module: `allocate`, `setup_local`, `setup_fork`, and the two inventory stubs. |

## Availability

There is **no `configure.m4` and no build gate**: the component links
nothing and needs no vendor SDK, so it builds everywhere and there is
nothing for `configure` to look for.

The question is settled at **run** time. `component_open` returns
`pmix_hwloc_check_vendor_baseclass(&pmix_globals.topology, 0x10de, 0x03)`,
which succeeds only if the node topology contains a PCI device with
NVIDIA's vendor ID `0x10de` in the display base class `0x03`. Absent
matching hardware (or a non-hwloc topology) the component declines to
open and is never queried.

The **base** class is deliberate. A datacenter part reports subclass
`0x0302` ("3D controller") while a card with a display wired to it
reports `0x0300` ("VGA compatible"), and both are NVIDIA GPUs; matching
`0x0302` exactly left the component silent on the second kind, which is
indistinguishable from being correct that no GPU is there.

`component_query` sets priority **10** — the lowest of the three vendor
components — and hands back `pmix_pgpu_nvd_module`; it does no checking of
its own, because a component that declined to open is never queried.

## Component-name requirement

`pgpu_nvd_component.c` emits its static-component pointer with
`PMIX_MCA_BASE_COMPONENT_INIT(pmix, pgpu, nvd)`. The third argument must
match the struct name `pmix_mca_pgpu_nvd_component`. An earlier version
used `nvidia` here, which referenced an undefined
`pmix_mca_pgpu_nvidia_component` symbol and would have failed to link had
the component been enabled; it is now correct. Keep the name in sync if
you ever rename the component.

## Component struct and MCA params

`pmix_pgpu_nvd_component_t` extends the base component with
`incparms`/`excparms` and their split `include`/`exclude` argvs.
`component_register` registers:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_nvd_include_envars` | `CUDA_*,NCCL_*` | comma-delimited glob list of envars to harvest |
| `pgpu_nvd_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

Unlike `amd` and `intel`, `nvd`'s include default is **non-empty**
(`CUDA_*,NCCL_*`), so on a host with NVIDIA GPUs its `allocate`
harvests CUDA and NCCL envars by default.

## The module functions

`pmix_pgpu_nvd_module` fills `name`, `allocate`, `setup_local`,
`setup_fork`, `collect_inventory`, and `deliver_inventory`; all other
slots are `NULL`. The bodies match the other vendor components:

- **`allocate`** — returns `PMIX_ERR_TAKE_NEXT_OPTION` if `info == NULL`,
  if neither `PMIX_SETUP_APP_ENVARS` nor `PMIX_SETUP_APP_ALL` was
  requested, or if the include list is empty. Otherwise it harvests
  envars via `pmix_util_harvest_envars` (default `CUDA_*,NCCL_*`), packs
  them as `PMIX_ENVAR`, compresses, and appends the result to `ilist`
  under `PMIX_PGPU_NVD_BLOB` (`"pmix.pgpu.nvd.blob"`) as a
  `PMIX_COMPRESSED_BYTE_OBJECT` (or `PMIX_BYTE_OBJECT` if
  uncompressible). It declines rather than appending an empty blob for
  every daemon in the job to carry.
- **`setup_local`** — finds `PMIX_PGPU_NVD_BLOB` in `info`, decompresses
  if needed — checking the **bool** `pmix_compress.decompress` returned
  before touching its output, see the framework
  [`AGENTS.md`](../AGENTS.md#the-decompress-return-is-not-optional) —
  and unpacks the `PMIX_ENVAR`s into `ns->envars` for later replay into
  local children by the base `setup_fork`.
- **`setup_fork`** — one line: `pmix_pgpu_base_set_visible_devices(proc,
  "NVIDIA", "CUDA_VISIBLE_DEVICES", env)`.
- **`collect_inventory` / `deliver_inventory`** — **stubs** that
  `PMIX_HIDE_UNUSED_PARAMS(...)` and `return PMIX_SUCCESS`.

## Gotchas

- Its device assignment (`setup_fork` → `CUDA_VISIBLE_DEVICES`) and its
  envar harvest are live; its inventory functions are no-ops. Do not
  describe those as working.
- Keep `PMIX_PGPU_NVD_BLOB` / `PMIX_PGPU_NVD_INVENTORY_KEY` unique across
  components; `setup_local` claims its data by matching the blob key.
- What is left to do here is the inventory functions. Do **not** answer
  that with a `configure.m4` gate detecting a CUDA runtime: whether this
  component has work to do is a property of the node the daemon runs on,
  and the build host is routinely not that node.
