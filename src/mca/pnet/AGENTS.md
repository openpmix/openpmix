<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET Framework

This document orients AI agents and human contributors working in the
`pnet` (**P**MIx **Net**work) framework. It assumes you have already read
the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden rules, prefix
conventions, thread-safety model, and MCA concepts described there all
apply here and are not repeated. This file covers what is specific to
`pnet`: what the framework is for, how a request fans out across the
active components, the two module structs, the fabric bookkeeping, and —
importantly — how much of the shipped component code is dormant or stale
scaffolding rather than live production code. Each component subdirectory
(`nvd/`, `opa/`, `simptest/`, `tcp/`) carries its own `AGENTS.md`.

There is no `docs/how-things-work/` page for `pnet`.

## What PNET does

`pnet` is the **server-side** hook by which PMIx participates in setting
up an application's network/fabric before and during launch. Per the
header comment in [`pnet.h`](pnet.h), it exists so a PMIx server can
"obtain network-related info such as security keys that need to be shared
across applications, and to setup network support for applications prior
to launch." In concrete terms the framework gives components an
opportunity to:

- **Allocate** network resources for a job — inject a shared transport
  security key, harvest login-node environment variables for forwarding
  to compute nodes, or assign static endpoints/ports (the `allocate`
  entry point, driven by `PMIx_server_setup_application` /
  `PMIx_Allocate_resources`).
- **Set up the local network** on each compute-node daemon after the job
  is registered — unpack whatever blob `allocate` produced and cache it
  as job-level info / envars (`setup_local_network`, driven by
  `PMIx_server_setup_local_support`).
- **Inject envars into a child** just before fork/exec (`setup_fork`).
- **Clean up** as clients and jobs terminate (`child_finalized`,
  `local_app_finalized`, `deregister_nspace`).
- **Collect and deliver fabric inventory** for the RM/scheduler
  (`collect_inventory`, `deliver_inventory`).
- **Register / update / deregister a fabric plane** for cost-matrix and
  endpoint queries (`register_fabric`, `update_fabric`,
  `deregister_fabric`).

Everything `pnet` does runs inside a **PMIx server** (usually the
scheduler or a compute-node daemon). The framework is opened and selected
only during server startup (`src/server/pmix_server.c`); the public entry
points are reached through the global `pmix_pnet` API module from the
server code (e.g. `pmix_pnet.allocate`, `pmix_pnet.setup_fork`).

### Read this before trusting the component code

Only **one** component, [`opa`](opa/AGENTS.md), is compiled in a default
build, and it selects at runtime **only** when hwloc reports matching
fabric hardware. [`tcp`](tcp/AGENTS.md) also compiles against the current
interface but is **opt-in** — its `configure.m4` is guarded by
`--with-tcp`, so it is off by default; when built it has no hardware gate
and is selected on **every** server (its entry points then self-gate on
role / blob key). [`nvd`](nvd/AGENTS.md) is hardwired **off** in its
`configure.m4` (see [Building](#building)), and
[`simptest`](simptest/AGENTS.md) builds only with `--with-simptest` and is
**stale** — its module functions no longer match the current
`pmix_pnet_module_t` interface and it references base symbols that no
longer exist, so it would not compile against today's tree (this is
invisible in CI precisely because it is not built). Treat the shipped
components as two current examples (`opa` built by default, `tcp` opt-in
via `--with-tcp`), one current-but-disabled example (`nvd`), and one stale
example (`simptest`). See each component's `AGENTS.md` for specifics, and
do not assume any of them reflects a supported, exercised code path.

## Single-select vs. multi-select

`pnet` is a **multi-select** framework. `base/pnet_base_select.c`
(`pmix_pnet_base_select`) queries every component, and for each that
returns a module it wraps the module in a `pmix_pnet_base_active_module_t`
and inserts it into `pmix_pnet_globals.actives` in **descending priority
order**. Every base entry point then **fans out across all active
modules** in that order. It is *fabric-manager style*: each active
component is expected to service the fabric it recognizes and to decline
the rest.

The decline conventions the base honors:

- For `allocate` and `setup_local_network`, the base calls every module
  and only aborts the fan-out on a **hard** error — `PMIX_SUCCESS`,
  `PMIX_ERR_NOT_AVAILABLE`, and `PMIX_ERR_TAKE_NEXT_OPTION` all mean
  "fine, keep going to the next module."
- For `collect_inventory` / `deliver_inventory`, any non-`PMIX_SUCCESS`
  return aborts and is propagated.
- For `register_fabric`, the base walks actives until one returns
  `PMIX_OPERATION_SUCCEEDED` (claiming the fabric), skipping modules that
  return `PMIX_ERR_TAKE_NEXT_OPTION`.

Unlike a first-success framework, there is no built-in fallback: if
`pmix_pnet_globals.actives` is empty (the common case on a server with no
matching hardware and no test component), the base functions simply
short-circuit — `allocate`/`setup_local_network` return `PMIX_SUCCESS`
with nothing done, and `register_fabric` returns `PMIX_ERR_NOT_SUPPORTED`.

## Two module structs

`pnet.h` defines **two** parallel structs. Getting them straight is the
key to reading the framework.

### `pmix_pnet_module_t` — the component-facing module

This is what every component fills in and returns from its
`component_query`. Fields (all function pointers may be left `NULL`; the
base skips a `NULL` slot):

| Field | Signature (abridged) | Purpose |
|-------|----------------------|---------|
| `name` | `char *` | component name string |
| `plane` | `void *` | optional pointer to plane-specific metadata |
| `init` / `finalize` | `(void)` | per-module lifecycle; `init` may veto selection by returning non-success |
| `allocate` | `(pmix_namespace_t *nptr, info, ninfo, pmix_list_t *ilist)` | build the launch blob (seckeys/envars/endpoints) appended as `pmix_kval_t`s to `ilist` |
| `setup_local_network` | `(pmix_nspace_env_cache_t *nptr, info, ninfo)` | on the compute node, unpack that blob and cache it |
| `child_finalized` | `(pmix_proc_t *peer)` | one client exited |
| `local_app_finalized` | `(pmix_namespace_t *nptr)` | all local clients of a job exited |
| `deregister_nspace` | `(pmix_namespace_t *nptr)` | release per-job resources (e.g. static ports) |
| `collect_inventory` | `(directives, ndirs, pmix_list_t *inventory)` | append local fabric inventory |
| `deliver_inventory` | `(info, ninfo, directives, ndirs)` | archive inventory from remote peers |
| `register_fabric` | `(pmix_fabric_t *fabric, directives, ndirs, cbfunc, cbdata)` | claim a fabric plane for cost/endpoint queries |
| `update_fabric` | `(pmix_fabric_t *fabric)` | refresh fabric data |
| `deregister_fabric` | `(pmix_fabric_t *fabric)` | release a fabric plane |

Note there is **no `setup_fork` in the component module** — envar
injection at fork time is handled entirely by the base (see below).

### `pmix_pnet_API_module_t` — the exported global `pmix_pnet`

The single global `pmix_pnet` (declared `extern` in `pnet.h`,
instantiated in `pnet_base_frame.c`) is an instance of the *API* module,
whose slots all point at the `pmix_pnet_base_*` fan-out functions. It
mirrors the component module with two differences, both deliberate:

1. **`allocate`, `setup_local_network`, and `deregister_nspace` take a
   `char *nspace`** instead of a `pmix_namespace_t *` /
   `pmix_nspace_env_cache_t *`. The header comment explains this is an
   optimization so every caller doesn't have to look up the namespace
   pointer — the base does that lookup once and passes the resolved
   object down to the components.
2. **The API module adds `setup_fork`** (`pmix_pnet_base_setup_fork`),
   which has no component-module counterpart.

All server back-end code calls through `pmix_pnet.<fn>(...)`; components
never call each other.

## Core data structures (`base/base.h`)

- **`pmix_pnet_globals_t pmix_pnet_globals`** — framework state:
  - `actives` — the priority-ordered list of
    `pmix_pnet_base_active_module_t { pri, module, component }`.
  - `fabrics` — registered `pmix_pnet_fabric_t` objects (see below).
  - `nspaces` — per-namespace `pmix_nspace_env_cache_t` objects holding
    the harvested envars to inject at fork time.
  - `selected` — guard so `pmix_pnet_base_select` runs once.
- **`pmix_pnet_fabric_t`** `{ super, char *name, size_t index,
  pmix_pnet_module_t *module, void *payload }` — one registered fabric
  plane, recording which module owns it so a later *remote* request
  (which arrives with `fabric->module == NULL`) can be routed back to the
  right module by matching `index` or `name`. Constructed/destructed in
  `pnet_base_frame.c`.
- **`pmix_nspace_env_cache_t`** (defined in
  [`src/include/pmix_globals.h`](../../../src/include/pmix_globals.h)) —
  `{ super, pmix_namespace_t *ns, pmix_list_t envars }`. The
  per-namespace envar cache that `setup_local_network` fills and
  `setup_fork` drains.

## The base fan-out functions (`base/pnet_base_fns.c`)

Every slot of the global `pmix_pnet` API module points here. These are
the framework — the components are leaf handlers.

| Base function | What it does |
|---------------|--------------|
| `pmix_pnet_base_allocate` | resolves `nspace` → `pmix_namespace_t` (creating it if new), and **only if the local peer is a server** calls each active `module->allocate`, tolerating `NOT_AVAILABLE`/`TAKE_NEXT_OPTION` |
| `pmix_pnet_base_setup_local_network` | resolves `nspace` → a `pmix_nspace_env_cache_t` on `pmix_pnet_globals.nspaces` (creating it), then calls each active `module->setup_local_network(ns, …)` |
| `pmix_pnet_base_setup_fork` | looks up the namespace's envar cache and `PMIx_Setenv`s every cached `pmix_envar_list_item_t` into the child's `env`. **This is the whole of `setup_fork` — it is a base operation, not a per-module call.** |
| `pmix_pnet_base_child_finalized` | fans out to each `module->child_finalized` |
| `pmix_pnet_base_local_app_finalized` | fans out to each `module->local_app_finalized` |
| `pmix_pnet_base_deregister_nspace` | removes & releases the namespace's envar cache, then fans out to each `module->deregister_nspace(ns->ns)` |
| `pmix_pnet_base_collect_inventory` | fans out to each `module->collect_inventory`; aborts on any error |
| `pmix_pnet_base_deliver_inventory` | fans out to each `module->deliver_inventory`; aborts on any error |
| `pmix_pnet_base_register_fabric` | initializes the `pmix_fabric_t`, walks actives until one returns `PMIX_OPERATION_SUCCEEDED`, and on success records a `pmix_pnet_fabric_t` on `pmix_pnet_globals.fabrics` |
| `pmix_pnet_base_update_fabric` | resolves the owning module (directly from `fabric->module`, or by matching `index`/`name` on the fabrics list for a remote request) and calls `module->update_fabric` |
| `pmix_pnet_base_deregister_fabric` | same resolution as update, then calls `module->deregister_fabric` |

**A dead declaration to be aware of:** `base.h` declares
`pmix_pnet_base_harvest_envars`, but **no definition exists** anywhere in
the tree. Components that harvest envars call the utility
`pmix_util_harvest_envars` directly, not this base symbol. Do not build on
the base declaration; either implement it or ignore it.

## Selection and lifecycle

- **`base/pnet_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pnet, "PMIx Network
  Operations", …)`), instantiates `pmix_pnet_globals` and the global
  `pmix_pnet` API module (every slot wired to a `pmix_pnet_base_*`
  function), and defines the `PMIX_CLASS_INSTANCE`s for
  `pmix_pnet_base_active_module_t` and `pmix_pnet_fabric_t`.
  `pmix_pnet_open` constructs the three globals lists and opens all
  components; `pmix_pnet_close` finalizes and releases every active
  module and destructs the lists. The frame registers **no framework-level
  MCA parameters** (there is no `pmix_pnet_register`).
- **`base/pnet_base_select.c`** (`pmix_pnet_base_select`) is the
  multi-select builder described above: query each component, run its
  `init` (skip it if `init` fails), and insert its module into `actives`
  in descending priority order. It always returns `PMIX_SUCCESS` — an
  empty active list is **not** an error for `pnet`, unlike single-select
  frameworks. At verbosity >4 it prints the resolved priority list.

Selection is driven from server startup: `src/server/pmix_server.c` opens
`pmix_pnet_base_framework` and calls `pmix_pnet_base_select()`.

Default priorities and runtime gates (from each component's
`component_query` / `component_open`):

| Component | Priority | Built by default? | Becomes active when… |
|-----------|----------|-------------------|----------------------|
| `nvd` | 10 | **No** (hardwired off) | hwloc reports a Mellanox `0x15b3` / NVIDIA `0x10de` device of class `0x207` |
| `opa` | 10 | **Yes** | hwloc reports an Intel `0x8086` device of class `0x208` |
| `simptest` | 0 | Only with `--with-simptest` | server role **and** `pnet=simptest` MCA selection **and** a config file given |
| `tcp` | 5 | **No** (`--with-tcp`) | always returns a module (no hardware gate); its entry points self-gate on `PMIX_PEER_IS_GATEWAY` / their own blob key, but `collect_inventory` runs on every server |

Because `nvd`/`opa` gate selection on a hwloc hardware probe in
`component_open`, they only end up in `actives` on hosts with the matching
fabric hardware. On a default build with no matching hardware, **no**
`pnet` component ends up in `actives` and the base functions quietly do
nothing. If `tcp` is built (`--with-tcp`) it has no such gate, so it then
lands in `actives` on every server; its entry points self-gate, so the
only work done on a non-gateway server without a matching blob is
`collect_inventory` reporting the local TCP interfaces.

## MCA parameters

The framework registers none. Each component registers its own via
`pmix_mca_base_component_var_register` (prefix `pmix_pnet_<comp>_`):

| Component | Parameter | Meaning |
|-----------|-----------|---------|
| `opa`, `nvd`, `tcp` | `include_envars` / `exclude_envars` | comma-delimited envar globs to harvest / omit (`*`,`?` supported) |
| `tcp` | `static_ports` | static port ranges, `type:plane:ranges;…` |
| `tcp` | `default_network_allocation` | default port allocation spec |
| `simptest` | `config_file` | path to the fabric-topology description file |

`opa` and `nvd` seed `include_envars` with fabric-specific defaults
(`opa`: `"HFI_*,PSM2_*"`; `nvd`: `"UCX_*,HCOLL_*,UCC_*,SHARP_*,NCCL_*"`).

## Directory layout

```
src/mca/pnet/
├── pnet.h                  Framework API: both module structs, the API global, version macro
├── base/
│   ├── base.h              Internal base API + pmix_pnet_globals + fabric/active-module types
│   ├── pnet_base_frame.c   open/close, framework decl, API-module instance, class instances
│   ├── pnet_base_select.c  multi-select: build the priority-ordered actives list
│   └── pnet_base_fns.c     the pmix_pnet_base_* fan-out functions
├── nvd/                    Mellanox/NVIDIA example (current interface, disabled in build)
├── opa/                    Omni-Path example (default-built; hwloc-gated at runtime)
├── simptest/               static-endpoint test example (stale; --with-simptest only)
└── tcp/                    static TCP/UDP port example (--with-tcp; no hardware gate)
```

## Threading

`pnet` has **no caddy/thread-shift machinery of its own**. Its entry
points are called by the server from within progress-thread events (the
server code that invokes `pmix_pnet.*` has already thread-shifted), and
the base functions are synchronous transforms over the actives list and
the namespace/fabric lists. The module interface *does* allow an
asynchronous path — `collect_inventory` / `deliver_inventory` /
`register_fabric` are documented to return `PMIX_OPERATION_IN_PROGRESS`
and run a callback later if a component must query a fabric manager on its
own thread — but no shipped component exercises that path today. If you
add one, follow the top-level thread-safety rules: shift to your own
thread, return `PMIX_OPERATION_IN_PROGRESS`, and fire the callback when
done.

## Building

The framework core (`base/`) is always built into `libpmix`. The
components are **conditionally compiled**, and their `configure.m4` files
are unusually blunt about it:

- **`opa`** — `AS_IF([test "yes" = "yes"], …)`, i.e. **always builds**.
  It is the only component listed in the generated
  `base/static-components.h` in a default configure. (Whether it then
  *selects* still depends on the hwloc probe at runtime.)
- **`tcp`** — guarded by `--with-tcp` (same `AC_ARG_WITH` pattern as
  `simptest`): **not built by default**. When the flag is given it
  compiles and, having no runtime hardware gate, also always selects.
- **`nvd`** — `AS_IF([test "yes" = "no"], …)`, i.e. the "can-compile"
  branch is never taken: **never built**. Its `Makefile` is still
  generated, but the source is not compiled into the library.
- **`simptest`** — built only when `--with-simptest` is passed to
  `configure`.

Each component reports its state through `PMIX_SUMMARY_ADD([Transports],
…)`, so `configure`'s summary shows `NVIDIA`, `OmniPath`, `Simptest`,
`TCP` as yes/no.

`simptest` ships a `show_help` file, `help-pnet-simptest.txt`; per the
top-level golden rule, after any add/delete/modify of that text you must
`rm src/util/pmix_show_help_content.* && make`. The other components ship
no `show_help` text.

Per the top-level build rules: editing a `Makefile.am` needs only a plain
`make`; **adding or removing a component directory, or changing a
`configure.m4`, changes the build wiring and requires
`./autogen.pl && ./configure … && make`** (this is exactly the mechanism
that keeps `nvd` out of the library today).

## When working in this framework

- **`opa` and `tcp` are the current references.** Both compile against
  today's interface. `opa` is built by default but only *runs* on
  Omni-Path hardware; `tcp` is opt-in (`--with-tcp`) and, once built, has
  no hardware gate so it always selects. `nvd` matches the current
  interface but is not built. `simptest` is stale (see below) and builds
  only with `--with-simptest`. Read `opa` or `tcp` first if you need a
  template.
- **Know why `simptest` doesn't compile against HEAD.** Its module
  functions use signatures from an older `pmix_pnet_module_t` (e.g. a
  `setup_fork` module slot that no longer exists, inventory functions that
  still take `pmix_inventory_cbfunc_t`/`pmix_op_cbfunc_t` callbacks, a
  `setup_local_network` taking `pmix_namespace_t *`). `tcp` used to share
  these problems and additionally referenced base symbols
  (`pmix_pnet_globals.nodes`, `pmix_pnet_node_t`, `pmix_pnet_resource_t`)
  that no longer exist; it has since been ported to the current interface
  (its inventory archive now lives in a component-local tree). If you
  resurrect `simptest`, port it the same way — do not "fix" the framework
  header to match a stale component.
- **`setup_fork` is base-only.** If a component needs an envar in the
  child's environment, it must add it to the namespace's envar cache
  during `setup_local_network` (append a `pmix_envar_list_item_t` to
  `ns->envars`); the base's `setup_fork` will inject it. There is no
  component `setup_fork` hook to override.
- **Respect the decline convention.** A module that does not recognize a
  request must return `PMIX_ERR_TAKE_NEXT_OPTION` (or
  `PMIX_ERR_NOT_AVAILABLE`) so the base continues the fan-out — never a
  hard error, which would abort the whole chain.
- **Blobs are opaque and must be self-packing.** As the comments in every
  `allocate` note, if a component transfers binary data it must pack it
  into a `pmix_buffer_t` itself (optionally compressing via
  `pmix_compress`) because the host RM does not know the format; the
  matching `setup_local_network` unpacks it under the same key. Keep the
  pack/unpack pair symmetric, and honor the top-level wire-format
  interoperability rules if the blob can cross versions.
