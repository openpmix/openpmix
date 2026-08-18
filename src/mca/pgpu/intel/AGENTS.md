<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU `intel` Component

`intel` is the `pgpu` component intended to provide Intel-GPU support:
harvesting Intel-relevant environment variables for a job and (eventually)
reporting Intel GPU inventory. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `intel`. **It is not built
in a default configuration** (see Availability) and its inventory logic is
stubbed, but its envar-harvesting path compiles cleanly and runs through
the shared launch wiring when the component is enabled.

## Files

| File | Contents |
|------|----------|
| `pgpu_intel.h` | Component struct `pmix_pgpu_intel_component_t`, module extern, and the blob/inventory key `#define`s. |
| `pgpu_intel_component.c` | Component struct + open/close/register/query. Priority **20**. |
| `pgpu_intel.c` | The module: `allocate`, `setup_local`, and the two inventory stubs. |

## Availability

There is **no `configure.m4` and no build gate**: the component links
nothing and needs no vendor SDK, so it builds everywhere and the MCA
machinery configures a component with no `configure.m4` by itself. It
previously carried one whose whole body was `AS_IF([test "yes" = "no"],
...)` — it never built at all.

The question is settled at **run** time. `component_open` returns
   `pmix_hwloc_check_vendor(&pmix_globals.topology, 0x8086, 0x0380)`,
   which succeeds only if the node topology contains a PCI device with
   Intel's vendor ID `0x8086` and class `0x0380`. Absent matching
   hardware (or a non-hwloc topology) the component declines to open.

`component_query` sets priority **20** and hands back
`pmix_pgpu_intel_module`.

## Component struct and MCA params

`pmix_pgpu_intel_component_t` extends the base component with
`incparms`/`excparms` and their split `include`/`exclude` argvs.
`component_register` registers:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_intel_include_envars` | `NULL` | comma-delimited glob list of envars to harvest |
| `pgpu_intel_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

With the default `NULL` include list, `allocate` harvests nothing.

## The module functions

`pmix_pgpu_intel_module` fills only `name`, `allocate`, `setup_local`,
`collect_inventory`, and `deliver_inventory`; all other slots are `NULL`.
The bodies are identical in shape to the other vendor components:

- **`allocate`** — returns `PMIX_ERR_TAKE_NEXT_OPTION` if `info == NULL`;
  otherwise, when `PMIX_SETUP_APP_ENVARS` / `PMIX_SETUP_APP_ALL` is set,
  harvests envars via `pmix_util_harvest_envars`, packs them as
  `PMIX_ENVAR`, compresses, and appends the result to `ilist` under
  `PMIX_PGPU_INTEL_BLOB` (`"pmix.pgpu.intel.blob"`) as a
  `PMIX_COMPRESSED_BYTE_OBJECT` (or `PMIX_BYTE_OBJECT` if uncompressible).
- **`setup_local`** — finds `PMIX_PGPU_INTEL_BLOB` in `info`, decompresses
  if needed, and unpacks the `PMIX_ENVAR`s into `ns->envars` for later
  replay into local children by the base `setup_fork`.
- **`collect_inventory` / `deliver_inventory`** — **stubs** that
  `PMIX_HIDE_UNUSED_PARAMS(...)` and `return PMIX_SUCCESS`.

## Gotchas

- Its default include list is empty and its inventory functions do
  nothing. Do not describe those as working.
- Keep `PMIX_PGPU_INTEL_BLOB` / `PMIX_PGPU_INTEL_INVENTORY_KEY` unique
  across components; `setup_local` claims its data by matching the blob
  key string.
- What is left to do here is a sensible default include list and the
  inventory functions. Do **not** answer either with a `configure.m4`
  gate detecting the Level Zero library: whether this component has work
  to do is a property of the node the daemon runs on, and the build host
  is routinely not that node.
