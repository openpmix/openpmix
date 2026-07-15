<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PGPU Framework

This document orients AI agents and human contributors working in the
`pgpu` (**P**MIx **GPU**) framework. It assumes you have already read the
top-level [`AGENTS.md`](../../../AGENTS.md) — the golden rules, prefix
conventions, thread-safety model, and MCA concepts described there all
apply here and are not repeated. This file covers what is specific to
`pgpu`: what the framework does, how its two module structs relate, the
base routing layer every component leans on, and the honest state of the
code. Each vendor component subdirectory (`amd/`, `intel/`, `nvd/`)
carries its own `AGENTS.md`; a build-on-request `test/` component (see
[Building](#building)) exercises the launch path on a host that has no
real GPU.

There is no `docs/how-things-work/` page for `pgpu`. Its closest sibling
is [`pnet`](../pnet/AGENTS.md), whose structure `pgpu` mirrors almost
exactly (same allocate / setup_local / setup_fork / inventory shape). The
server now drives `pgpu` the same way it drives `pnet`, so when in doubt
about intended behavior, read how the server calls `pnet`.

## Read this first: the current state of `pgpu`

Before you invest in this framework, understand what is and is not live:

1. **The server drives the full launch path.**
   `PMIx_server_setup_application` calls `pmix_pgpu.allocate` and
   `PMIx_server_setup_local_support` calls `pmix_pgpu.setup_local` (in
   `src/server/pmix_server.c`), immediately alongside the corresponding
   `pnet` calls, and `pmix_pgpu.setup_fork` runs from the fork path.
   Together with the long-standing `pmix_pgpu.collect_inventory` /
   `pmix_pgpu.deliver_inventory` calls, **every `pgpu` API entry now has
   an in-tree caller** — the envar harvest → ship → inject pipeline is
   wired exactly as `pnet`'s is.

2. **No vendor component is built by default.** The three vendor
   components (`amd`, `intel`, `nvd`) ship a `configure.m4` whose gate is
   literally `AS_IF([test "yes" = "no"], [build], [do not build])` — a
   condition that is always false — so they are compiled out, the
   generated [`base/static-components.h`](base/static-components.h)
   contains no vendor component, and `pmix_pgpu_globals.actives` is empty
   on a stock build. They now *compile and link* cleanly when their gate
   is enabled, but shipping them on by default still needs real
   vendor-runtime detection. To exercise the framework end-to-end today,
   build the `test` component with `--with-pgpu-test` (see
   [Building](#building)).

3. **The vendor GPU logic is still stubbed.** The envar-harvesting
   `allocate`/`setup_local` are fully implemented, but `collect_inventory`
   and `deliver_inventory` are empty stubs that just `return
   PMIX_SUCCESS`.

So the framework *is* wired into launch, but no vendor component runs in a
stock build — enable `--with-pgpu-test` (or a vendor gate) to see it work.
Document and extend it faithfully, and do not describe the stubs as doing
work the code does not yet do.

## What PGPU is for

The header comment in [`pgpu.h`](pgpu.h) states the intent: this interface
is *"for use by PMIx servers to obtain GPU-related info and to setup GPU
support for applications prior to launch."* Concretely, a vendor component
is meant to:

- **Harvest GPU-relevant environment variables** (e.g. `CUDA_*`,
  `NCCL_*`) on the node running the launcher, pack them into a blob at
  `PMIx_server_setup_application` time (`allocate`), ship that blob to the
  backend compute-node daemons, and replay the envars into each local
  client's environment at fork time (`setup_local` caches them,
  `setup_fork` injects them).
- **Report and archive GPU inventory** — discover the GPUs present in the
  node topology (`collect_inventory`) and store inventory delivered by
  remote peers (`deliver_inventory`).

The envar path is the only substantially-implemented behavior, and it
lives in the components, not the base.

## Single-select or multi-select?

`pgpu` is **multi-select**: several components may be active at once, and
each base entry point loops over *all* active modules. But its selection
is **hand-rolled**, not the standard `pmix_mca_base_select()` used by
single-select frameworks — see [Selection](#selection-pgpu_base_selectc)
below. A node with both AMD and NVIDIA GPUs is expected to activate both
the `amd` and `nvd` modules simultaneously; each contributes its own
vendor blob.

## The two module structs (`pgpu.h`)

`pgpu.h` is unusual: it defines **two** different function-pointer
structs. Understanding why is the key to the framework.

### `pmix_pgpu_module_t` — what a component fills in

This is the per-component module. Its function-pointer fields:

| Field | Signature (typedef) | Purpose |
|-------|---------------------|---------|
| `name` | `char *` | component name string (used in verbose output) |
| `plane` | `void *` | pointer to plane-specific metadata (unused by current components) |
| `init` | `pmix_pgpu_base_module_init_fn_t` — `(void)` | one-time setup; return error to decline selection |
| `finalize` | `pmix_pgpu_base_module_fini_fn_t` — `(void)` | teardown |
| `allocate` | `..._allocate_fn_t` — `(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo, pmix_list_t *ilist)` | harvest/pack data for `PMIx_server_setup_application` |
| `setup_local` | `..._setup_local_fn_t` — `(pmix_nspace_env_cache_t *ns, pmix_info_t info[], size_t ninfo)` | consume the blob at `PMIx_server_setup_local_support` |
| `child_finalized` | `..._child_finalized_fn_t` — `(pmix_proc_t *peer)` | cleanup when a local client exits |
| `local_app_finalized` | `..._local_app_finalized_fn_t` — `(pmix_namespace_t *nptr)` | cleanup when all local clients of an app exit |
| `deregister_nspace` | `..._dregister_nspace_fn_t` — `(pmix_namespace_t *nptr)` | release per-nspace resources |
| `collect_inventory` | `..._collect_inventory_fn_t` — `(pmix_info_t directives[], size_t ndirs, pmix_list_t *inventory)` | add local GPU inventory as `pmix_kval_t`s |
| `deliver_inventory` | `..._deliver_inventory_fn_t` — `(pmix_info_t info[], size_t ninfo, pmix_info_t directives[], size_t ndirs)` | archive inventory from remote peers |

Note there is **no `setup_fork` in the component module** — components do
not touch the fork directly; the base does (see below). The three vendor
components fill in only `name`, `allocate`, `setup_local`,
`collect_inventory`, and `deliver_inventory`; the `test` component fills
`name`, `allocate`, and `setup_local`. All other slots are left `NULL` and
the base skips them.

### `pmix_pgpu_API_module_t` and the global `pmix_pgpu`

The rest of the library never calls a component module directly. It calls
through the exported global **`pmix_pgpu`**, whose type is the *second*
struct, `pmix_pgpu_API_module_t`. This one differs deliberately:

- Its `allocate`, `setup_local`, and `deregister_nspace` take a **`char
  *nspace` string** instead of a `pmix_namespace_t *` pointer. The header
  comment explains why: it is an optimization so that *every* component
  does not have to look up the namespace-object pointer — the base does
  the lookup once and hands the resolved `pmix_namespace_t *` /
  `pmix_nspace_env_cache_t *` to the components.
- It adds a **`setup_fork`** entry
  (`pmix_pgpu_base_API_setup_fork_fn_t` — `(const pmix_proc_t *peer, char
  ***env)`) that has no per-component counterpart.

Every slot of the global `pmix_pgpu` points at a `pmix_pgpu_base_*`
routing function (see below). It is instantiated in `pgpu_base_frame.c`
and wires up `allocate`, `setup_local`, `setup_fork`, `child_finalized`,
`local_app_finalized`, `deregister_nspace`, `collect_inventory`, and
`deliver_inventory`. Note `init` and `finalize` are **not** set on the
global module.

### Version macro and component type

- **`pmix_pgpu_base_component_t`** is a plain typedef of
  `pmix_mca_base_component_t`; components wrap it in a larger struct to
  hold their MCA-param state (see any component's `*_component_t`).
- **`PMIX_PGPU_BASE_VERSION_1_0_0`** is the version macro every component
  struct must open with.

## Directory layout

```
src/mca/pgpu/
├── pgpu.h                  Framework public API: the two module structs + version macro
├── base/                   Framework infrastructure
│   ├── base.h              Internal base API: globals, active-module class, base fn decls
│   ├── pgpu_base_frame.c   open/close, framework decl, global pmix_pgpu, active-module class
│   ├── pgpu_base_select.c  hand-rolled multi-select (query + priority insert)
│   └── pgpu_base_fns.c     the pmix_pgpu_base_* routing functions
├── amd/                    AMD vendor component  (compiles; build-gated off by default)
├── intel/                  Intel vendor component (compiles; build-gated off by default)
├── nvd/                    NVIDIA vendor component (compiles; build-gated off by default)
└── test/                   Build-on-request test component (--with-pgpu-test)
```

## Framework globals (`base/base.h`)

`pmix_pgpu_globals` (`pmix_pgpu_globals_t`) holds:

| Field | Type | Purpose |
|-------|------|---------|
| `actives` | `pmix_list_t` of `pmix_pgpu_base_active_module_t` | selected modules in descending priority order |
| `nspaces` | `pmix_list_t` of `pmix_nspace_env_cache_t` | per-nspace cache of harvested envars awaiting fork |
| `selected` | `bool` | guards against selecting twice |

`pmix_pgpu_base_active_module_t` `{ super, pri, module, component }` wraps
each selected module with its priority and owning component; its class is
instantiated in `pgpu_base_frame.c`.

The `nspaces` cache is the heart of the envar mechanism:
`pmix_nspace_env_cache_t` (defined in `src/include/pmix_globals.h`) is
`{ ns, envars }` — a namespace pointer plus a list of
`pmix_envar_list_item_t`. `setup_local` fills `ns->envars`; `setup_fork`
drains it into the child's environment.

## The base routing layer (`pgpu_base_fns.c`)

Every global `pmix_pgpu` entry is a thin `pmix_pgpu_base_*` dispatcher.
Their common shape: bail out early if `actives` is empty, then loop the
active modules and call each one that implements the corresponding
function pointer. Specifics that matter:

- **`pmix_pgpu_base_allocate(char *nspace, ...)`** — finds (or creates and
  appends) the `pmix_namespace_t` in `pmix_globals.nspaces`, then — **only
  if this peer is a server** (`PMIX_PEER_IS_SERVER`) — calls each active
  `module->allocate(nptr, ...)`. A module return of `PMIX_SUCCESS`,
  `PMIX_ERR_NOT_AVAILABLE`, or `PMIX_ERR_TAKE_NEXT_OPTION` is tolerated
  (keep going); any other error aborts and is returned. The header note
  says a **tool** (e.g. `prun`) may also call this to harvest local
  envars for a `PMIx_Spawn`.
- **`pmix_pgpu_base_setup_local(char *nspace, ...)`** — resolves or
  creates the `pmix_nspace_env_cache_t` in `pmix_pgpu_globals.nspaces`
  (retaining the underlying `pmix_namespace_t`), then calls each active
  `module->setup_local(ns, ...)` with the same tolerated-return contract.
  "Can only be called by a server from within an event."
- **`pmix_pgpu_base_setup_fork(const pmix_proc_t *proc, char ***env)`** —
  does **not** call any component. It looks up the proc's nspace in the
  envar cache and, for each cached `pmix_envar_list_item_t`, calls
  `PMIx_Setenv(...)` to inject it into `*env`. This is why the component
  module has no `setup_fork`: the fork step just replays what
  `setup_local` cached.
- **`pmix_pgpu_base_child_finalized` / `_local_app_finalized`** — loop and
  forward to any module implementing the hook (all current modules leave
  these `NULL`).
- **`pmix_pgpu_base_deregister_nspace(char *nspace)`** — removes and
  `PMIX_RELEASE`s the matching `pmix_nspace_env_cache_t`, forwarding to
  each module's `deregister_nspace` first.
- **`pmix_pgpu_base_collect_inventory` / `_deliver_inventory`** — loop and
  forward; here a **non-`SUCCESS` return stops the loop and is returned**
  (stricter than the allocate/setup path). These run from the server's
  inventory-collection paths, as `allocate`/`setup_local`/`setup_fork` run
  from the launch paths.

### A dead declaration to be aware of

`base.h` declares `pmix_pgpu_base_harvest_envars(char **incvars, char
**excvars, pmix_list_t *ilist)`, but **no definition exists anywhere in
the tree**. Components do their own harvesting by calling the util routine
`pmix_util_harvest_envars` (in `src/util/pmix_environ.c`) directly. Do not
assume the base helper works; if you need it, you must implement it.

## Selection (`pgpu_base_select.c`)

`pmix_pgpu_base_select()` is a **custom multi-select**, not
`pmix_mca_base_select`. It walks
`pmix_pgpu_base_framework.framework_components`, and for each component
with a `pmix_mca_query_component`:

1. calls `query(&module, &priority)`; skips the component if it returns
   non-success or a `NULL` module;
2. calls the module's `init()` (if present) and skips the component if
   `init` fails;
3. wraps the survivor in a `pmix_pgpu_base_active_module_t` and inserts it
   into `pmix_pgpu_globals.actives` in **descending priority order**
   (strict `priority > mod->pri`, so equal priorities keep component
   iteration order).

Finding zero components is **not** an error — the function returns
`PMIX_SUCCESS` with an empty `actives` list, which is the case in a
default build (no vendor component compiled, `test` compiled only with
`--with-pgpu-test`). At verbosity >4 it prints the resolved priority list.
`selected` guards against a second pass.

Default component priorities (from each `component_query`):

| Component | Priority | `component_open` gate (only if built) |
|-----------|----------|----------------------------------------|
| `amd`   | 20 | `pmix_hwloc_check_vendor(topo, 0x1022, 0x302)` |
| `intel` | 20 | `pmix_hwloc_check_vendor(topo, 0x8086, 0x0380)` |
| `nvd`   | 10 | `pmix_hwloc_check_vendor(topo, 0x10de, 0x302)` |
| `test`  | 10 | server role **and** `pgpu=test` named in the MCA selection string |

`pmix_hwloc_check_vendor` (in `src/hwloc/pmix_hwloc.c`) walks the PCI
devices in the node topology and returns `PMIX_SUCCESS` only if a device
matches the given (vendor-ID, PCI-class) pair; it returns
`PMIX_ERR_NOT_AVAILABLE` when no such device is present and
`PMIX_ERR_TAKE_NEXT_OPTION` when the topology is not hwloc-sourced. A
non-success `component_open` prevents the component from being queried, so
even if the components were built, each would activate **only on a host
that actually has that vendor's GPU**.

## Lifecycle (`pgpu_base_frame.c`)

- **`pmix_pgpu_open`** constructs `actives` and `nspaces`, then opens all
  built components (none in a stock build).
- **`pmix_pgpu_close`** clears `selected`, finalizes each active module,
  destructs both lists, and closes the components.
- The framework is declared with
  `PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pgpu, "PMIx GPU Operations",
  NULL, pmix_pgpu_open, pmix_pgpu_close, ...
  PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT)`. The **register argument is
  `NULL`**: `pgpu` defines **no framework-level MCA parameters**. (The
  only MCA params in this framework are the per-component
  `include_envars` / `exclude_envars` pair.)

The framework is opened and selected during **server** startup in
`src/server/pmix_server.c` — `pmix_mca_base_framework_open(...)` followed
by `pmix_pgpu_base_select()`, immediately after `pnet` selection and
before `pstat` is opened — and closed at server teardown. `pmix_pgpu.allocate`
is invoked from `_setup_app` (next to `pmix_pnet.allocate`),
`pmix_pgpu.setup_local` from `_setup_local_support` (next to
`pmix_pnet.setup_local_network`), `pmix_pgpu.setup_fork` from the local
fork path, and the inventory entries from the server's
inventory-collection paths.

## MCA parameters

The framework has none. Each component registers two string params (shown
here for `nvd`; `amd`, `intel`, and `test` are identical apart from the
prefix and defaults):

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pgpu_nvd_include_envars` | `CUDA_*,NCCL_*` (nvd); `amd`/`intel` default `NULL`; `test` defaults `PMIX_TEST_GPU_*` | comma-delimited glob list of envars to harvest (`*`/`?` supported) |
| `pgpu_nvd_exclude_envars` | `NULL` | comma-delimited glob list of envars to exclude |

At register time each list is split with `PMIx_Argv_split` into the
component's `include` / `exclude` argv, which `allocate` passes to
`pmix_util_harvest_envars`. With a `NULL` include list (the `amd`/`intel`
default) the harvest is skipped entirely.

## Threading

There is no caddy pattern inside `pgpu`. The base routing functions and
component module functions are **synchronous** and are reached only on the
server's progress thread — `setup_local`/`setup_fork` are explicitly
documented "server, from within an event," and `collect_inventory` /
`deliver_inventory` are called from already-thread-shifted server-op caddy
handlers. The `pgpu.h` header does describe an *optional* asynchronous
contract for the inventory hooks (shift to an internal thread and return
`PMIX_OPERATION_IN_PROGRESS`), but no current component uses it. If you
implement it, follow the top-level thread-shifting rules.

## Building

The framework **base** is always compiled into `libpmix` (via
`base/Makefile.include`); the top-level `Makefile.am` builds
`libmca_pgpu.la` with `sources =` empty apart from the base. Each
component ships a `configure.m4` and a `Makefile.am`:

- The three **vendor** components (`amd`, `intel`, `nvd`) hard-disable
  themselves (`AS_IF([test "yes" = "no"], ...)`), so they are not compiled
  and `static-components.h` carries no vendor component. To enable one,
  replace the placeholder gate with real vendor-runtime detection, then
  regenerate: `./autogen.pl && ./configure ... && make`.
- The **`test`** component builds only when `--with-pgpu-test` is passed
  to `configure`, and never ships in a normal build. It is the way to
  exercise the launch path on a host with no GPU (select it at runtime
  with `PMIX_MCA_pgpu=test`).

Merely editing a `Makefile.am` needs only `make`, but changing a
`configure.m4` or adding/removing a component directory changes the wiring
`configure` resolves, so the full `./autogen.pl && ./configure && make`
regen is required. `pgpu` ships **no `show_help` text**, so the
regenerate-the-help-content golden rule does not apply here.

## When adding or modifying a component

- Open the component struct with `PMIX_PGPU_BASE_VERSION_1_0_0` and set
  `.pmix_mca_component_name` to your directory name. Emit the component-
  init symbol with `PMIX_MCA_BASE_COMPONENT_INIT(pmix, pgpu, <name>)` —
  the third argument **must** match the struct's component name
  (`pmix_mca_pgpu_<name>_component`), or the static-component pointer will
  reference an undefined symbol (this was the `nvd` bug, since fixed).
- Provide a `component_query` that hands back your `pmix_pgpu_module_t` and
  a priority, and a `component_open` that declines (returns non-success)
  when the vendor hardware is absent — reuse `pmix_hwloc_check_vendor`.
- Fill only the module slots you implement; leave the rest `NULL` — the
  base checks each pointer before calling.
- If you harvest envars, mirror the existing components: gate on
  `PMIX_SETUP_APP_ENVARS` / `PMIX_SETUP_APP_ALL`, harvest with
  `pmix_util_harvest_envars`, pack as `PMIX_ENVAR`, compress via
  `pmix_compress`, and stash under a unique per-component blob key so
  `setup_local` can find and unpack it.
- Give the component a real `configure.m4` gate (detect the vendor
  runtime/library) instead of the placeholder `test "yes" = "no"`, and add
  the `PMIX_SUMMARY_ADD` line so the configure summary reports it. The
  `test` component's `configure.m4` (an `--with-pgpu-test` `AC_ARG_WITH`)
  is the template for a real opt-in gate.
- The launch API calls (`pmix_pgpu.allocate` / `.setup_local` /
  `.setup_fork`) are already wired into the server the way `pnet` is; a
  new component inherits them for free. The `test` component is the
  working reference for a component that harvests and replays envars
  through that path.
