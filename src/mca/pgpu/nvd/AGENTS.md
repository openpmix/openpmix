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
to `nvd`. **It is not built in any current configuration** (see
Availability) and its GPU logic beyond envar harvesting is stubbed. It is
the one component with a non-empty default include list — and it carries a
latent component-name bug.

## Files

| File | Contents |
|------|----------|
| `pgpu_nvd.h` | Component struct `pmix_pgpu_nvd_component_t`, module extern, and the blob/inventory key `#define`s. |
| `pgpu_nvd_component.c` | Component struct + open/close/register/query. Priority **10**. |
| `pgpu_nvd.c` | The module: `allocate`, `setup_local`, and the two inventory stubs. |
| `configure.m4` | Build gate — **hard-disabled** (`test "yes" = "no"`). |

## Availability (why it never activates)

Two independent gates keep `nvd` inactive:

1. **Build gate.** `configure.m4` runs
   `AS_IF([test "yes" = "no"], [build], [do not build])`, always taking
   the not-built branch, so the component is compiled out and never
   appears in `base/static-components.h`. The configure summary reports
   `GPUs / NVIDIA` as not-happy.
2. **Runtime gate (only relevant if built).** `component_open` returns
   `pmix_hwloc_check_vendor(&pmix_globals.topology, 0x10de, 0x302)`, which
   succeeds only if the node topology contains a PCI device with NVIDIA's
   vendor ID `0x10de` and class `0x302`. Absent matching hardware (or a
   non-hwloc topology) the component declines to open.

`component_query` sets priority **10** — the lowest of the three vendor
components — and hands back `pmix_pgpu_nvd_module`.

## Latent bug: component-name mismatch

`pgpu_nvd_component.c` emits its static-component pointer with
`PMIX_MCA_BASE_COMPONENT_INIT(pmix, pgpu, nvidia)` — note **`nvidia`**,
not `nvd`. That macro expands to a pointer named
`pmix_mca_pgpu_nvidia_component_ptr` referencing
`pmix_mca_pgpu_nvidia_component`, but the actual struct is
`pmix_mca_pgpu_nvd_component`. If `nvd` were ever enabled and linked into
`static-components.h`, this would fail to resolve. The third macro
argument must be `nvd`. Fix this together with the `configure.m4` gate
before attempting to build the component.

## Component struct and MCA params

`pmix_pgpu_nvd_component_t` extends the base component with
`incparms`/`excparms` and their split `include`/`exclude` argvs.
`component_register` registers:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_nvd_include_envars` | `CUDA_*,NCCL_*` | comma-delimited glob list of envars to harvest |
| `pgpu_nvd_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

Unlike `amd` and `intel`, `nvd`'s include default is **non-empty**
(`CUDA_*,NCCL_*`), so — if it were built and selected — its `allocate`
would harvest CUDA and NCCL envars by default.

## The module functions

`pmix_pgpu_nvd_module` fills only `name`, `allocate`, `setup_local`,
`collect_inventory`, and `deliver_inventory`; all other slots are `NULL`.
The bodies match the other vendor components:

- **`allocate`** — returns `PMIX_ERR_TAKE_NEXT_OPTION` if `info == NULL`;
  otherwise, when `PMIX_SETUP_APP_ENVARS` / `PMIX_SETUP_APP_ALL` is set,
  harvests envars via `pmix_util_harvest_envars` (default `CUDA_*,NCCL_*`),
  packs them as `PMIX_ENVAR`, compresses, and appends the result to
  `ilist` under `PMIX_PGPU_NVD_BLOB` (`"pmix.pgpu.nvd.blob"`) as a
  `PMIX_COMPRESSED_BYTE_OBJECT` (or `PMIX_BYTE_OBJECT` if uncompressible).
- **`setup_local`** — finds `PMIX_PGPU_NVD_BLOB` in `info`, decompresses
  if needed, and unpacks the `PMIX_ENVAR`s into `ns->envars` for later
  replay into local children by the base `setup_fork`.
- **`collect_inventory` / `deliver_inventory`** — **stubs** that
  `PMIX_HIDE_UNUSED_PARAMS(...)` and `return PMIX_SUCCESS`.

## Gotchas

- Do not describe `nvd` as functional: not built (and would not even link
  as written due to the `nvidia` name bug), and its inventory functions
  are no-ops.
- Keep `PMIX_PGPU_NVD_BLOB` / `PMIX_PGPU_NVD_INVENTORY_KEY` unique across
  components; `setup_local` claims its data by matching the blob key.
- Bringing it to life means fixing the `PMIX_MCA_BASE_COMPONENT_INIT`
  name, replacing the `configure.m4` placeholder with real CUDA-runtime
  detection, and implementing the inventory functions. `configure.m4`
  changes require the full `./autogen.pl && ./configure && make` regen.
