<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU `amd` Component

`amd` is the `pgpu` component intended to provide AMD-GPU support:
harvesting AMD-relevant environment variables for a job and (eventually)
reporting AMD GPU inventory. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `amd`. **It is not built
in any current configuration** (see the Availability section) and its GPU
logic beyond envar harvesting is stubbed.

## Files

| File | Contents |
|------|----------|
| `pgpu_amd.h` | Component struct `pmix_pgpu_amd_component_t`, module extern, and the blob/inventory key `#define`s. |
| `pgpu_amd_component.c` | Component struct + open/close/register/query. Priority **20**. |
| `pgpu_amd.c` | The module: `allocate`, `setup_local`, and the two inventory stubs. |
| `configure.m4` | Build gate — **hard-disabled** (`test "yes" = "no"`). |

## Availability (why it never activates)

Two independent gates keep `amd` inactive:

1. **Build gate.** `configure.m4` runs
   `AS_IF([test "yes" = "no"], [build], [do not build])`, always taking
   the not-built branch, so the component is compiled out and never
   appears in `base/static-components.h`. The configure summary reports
   `GPUs / AMD` as not-happy.
2. **Runtime gate (only relevant if built).** `component_open` returns
   `pmix_hwloc_check_vendor(&pmix_globals.topology, 0x1022, 0x302)`, which
   succeeds only if the node topology contains a PCI device with AMD's
   vendor ID `0x1022` and class `0x302`. Absent matching hardware (or a
   non-hwloc topology) the component declines to open.

`component_query` sets priority **20** and hands back
`pmix_pgpu_amd_module`.

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
  `info == NULL`. Otherwise, if the caller passed `PMIX_SETUP_APP_ENVARS`
  or `PMIX_SETUP_APP_ALL` (true), it harvests envars matching
  `component.include` (minus `component.exclude`) via
  `pmix_util_harvest_envars`, packs each as `PMIX_ENVAR` into a scratch
  `pmix_buffer_t`, compresses the buffer with `pmix_compress.compress`,
  and appends it to `ilist` as a `pmix_kval_t` keyed `PMIX_PGPU_AMD_BLOB`
  (`"pmix.pgpu.amd.blob"`), typed `PMIX_COMPRESSED_BYTE_OBJECT` if
  compression succeeded, else `PMIX_BYTE_OBJECT`.
- **`setup_local`** — called at `PMIx_server_setup_local_support` on the
  backend. Scans `info` for `PMIX_PGPU_AMD_BLOB`, decompresses it if it is
  a compressed byte object, loads it into a buffer, and unpacks
  `PMIX_ENVAR`s in a loop, appending each to `ns->envars`. Those cached
  envars are later injected into each local child by the base
  `setup_fork`. (Note the loop allocates one extra `pmix_envar_list_item_t`
  past the last successful unpack and `PMIX_RELEASE`s it — expected.)
- **`collect_inventory` / `deliver_inventory`** — **stubs**. Both
  `PMIX_HIDE_UNUSED_PARAMS(...)` and `return PMIX_SUCCESS`; the comments
  ("search the topology for AMD GPUs" / "look for our inventory blob")
  describe intent, not implemented behavior.

## Gotchas

- Do not describe `amd` as functional. It is not built, its default
  include list is empty, and its inventory functions do nothing.
- The blob key `PMIX_PGPU_AMD_BLOB` and inventory key
  `PMIX_PGPU_AMD_INVENTORY_KEY` (`pgpu_amd.h`) must stay unique across
  components — the `setup_local` search keys on the blob string to claim
  its own data.
- To bring the component to life you must fix the `configure.m4` gate to
  detect a real AMD/ROCm runtime, likely set a sensible default include
  list, and implement the inventory functions. Changing `configure.m4`
  requires the full `./autogen.pl && ./configure && make` regen.
