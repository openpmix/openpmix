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
- **Inject envars into a child** just before fork/exec (`setup_fork`) —
  the namespace-wide ones the base replays from its cache, and the
  per-rank ones each module contributes, notably naming the NIC a process
  was mapped against to the software that will use it.
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

Two components, [`opa`](opa/AGENTS.md) and [`nvd`](nvd/AGENTS.md), are
compiled in a default build, and each selects at runtime **only** when
hwloc reports its fabric hardware. (`nvd` was hardwired off in its
`configure.m4` until recently — see [Building](#building) — which meant a
cluster with Mellanox NICs got no support unless somebody had thought to
ask for a test build. Neither component links anything, so the build host
was answering a question only the run host can answer.)
[`tcp`](tcp/AGENTS.md) also compiles against the current interface but is
**opt-in** — its `configure.m4` is guarded by `--with-tcp`, so it is off
by default; when built it has no hardware gate and is selected on
**every** server (its entry points then self-gate on role / blob key).
[`simptest`](simptest/AGENTS.md) is a working test fabric that builds with
`--with-simptest` or `--enable-test-build`; it compiles against the
current interface and drives the assign → cache → `PMIx_Get` path
end-to-end (exercised by `test/simple/simpcoord.c`). See each component's
`AGENTS.md` for specifics, and do not assume any of them reflects a
supported, exercised code path.

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
| `setup_fork` | `(const pmix_proc_t *proc, char ***env)` | contribute **this one process's** network environment, after the base has replayed the namespace-wide envar cache |
| `child_finalized` | `(pmix_proc_t *peer)` | one client exited |
| `local_app_finalized` | `(pmix_namespace_t *nptr)` | all local clients of a job exited |
| `deregister_nspace` | `(pmix_namespace_t *nptr)` | release per-job resources (e.g. static ports) |
| `collect_inventory` | `(directives, ndirs, pmix_list_t *inventory)` | append local fabric inventory |
| `deliver_inventory` | `(info, ninfo, directives, ndirs)` | archive inventory from remote peers |
| `register_fabric` | `(pmix_fabric_t *fabric, directives, ndirs, cbfunc, cbdata)` | claim a fabric plane for cost/endpoint queries |
| `update_fabric` | `(pmix_fabric_t *fabric)` | refresh fabric data |
| `deregister_fabric` | `(pmix_fabric_t *fabric)` | release a fabric plane |

`setup_fork` is the per-*process* hook, and the split from the base's own
replay of the envar cache is the point: the cache is per *namespace*, so
it can only carry what every rank of the job shares, and a device
assignment is per *rank*. It is also the only pnet hook that runs on the
daemon that will fork the process, which is why per-node truth belongs
there — the head node's copy of a topology may belong to whichever node
reported it first.

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
2. **The API module's `setup_fork`** (`pmix_pnet_base_setup_fork`) does
   more than fan out: it replays the namespace envar cache into the
   child's environment first, and only then calls each active module's
   own `setup_fork`.

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
| `pmix_pnet_base_setup_fork` | looks up the namespace's envar cache and `PMIx_Setenv`s every cached `pmix_envar_list_item_t` into the child's `env`, **then** calls each active `module->setup_fork(proc, env)`, tolerating `NOT_AVAILABLE`/`TAKE_NEXT_OPTION` (the ordinary case: most processes are not mapped against a device). Any other error aborts the fork rather than launching a process that would use the wrong hardware. |
| `pmix_pnet_base_get_assigned_devices` | reads the proc's `PMIX_DEVICE_ID` out of the local datastore, resolves each device against the **local** topology, keeps the ones whose PCI `(vendor, class)` matches one of the caller's `pmix_pnet_pcimatch_t` entries, and joins their **selectors** with commas. Returns `PMIX_ERR_TAKE_NEXT_OPTION` when the process was given none of yours — and equally when the assignment is not an array of `pmix_device_t`, which it checks rather than assumes (see below) |
| `pmix_pnet_base_set_assigned_devices` | the one-line wrapper that writes that value into a named environment variable; this is all most components need |
| `pmix_pnet_base_child_finalized` | fans out to each `module->child_finalized` |
| `pmix_pnet_base_local_app_finalized` | fans out to each `module->local_app_finalized` |
| `pmix_pnet_base_deregister_nspace` | removes the namespace's envar cache **if it has one**, fans out to each `module->deregister_nspace(nptr)`, and only then releases the cache. A process that never ran `setup_local_network` — a scheduler, or a daemon this job put no procs on — has no cache, and the namespace object itself is used instead: a module that took resources in `allocate` still has to be told to give them back |
| `pmix_pnet_base_collect_inventory` | fans out to each `module->collect_inventory`; aborts on any error |
| `pmix_pnet_base_deliver_inventory` | fans out to each `module->deliver_inventory`; aborts on any error |
| `pmix_pnet_base_register_fabric` | initializes the `pmix_fabric_t`, walks actives until one returns `PMIX_OPERATION_SUCCEEDED`, and on success records a `pmix_pnet_fabric_t` on `pmix_pnet_globals.fabrics` |
| `pmix_pnet_base_update_fabric` | resolves the owning module (directly from `fabric->module`, or by matching `index`/`name` on the fabrics list for a remote request) and calls `module->update_fabric` |
| `pmix_pnet_base_deregister_fabric` | same resolution as update, then calls `module->deregister_fabric` |

Components that harvest envars call the utility
`pmix_util_harvest_envars` directly. (`base.h` used to declare a
`pmix_pnet_base_harvest_envars` that was never defined anywhere in the
tree; it has been removed, since a header promising a symbol no
translation unit provides can only mislead.)

### Two traps in these lookups

**`PMIX_CHECK_NSPACE` treats an empty nspace as a wildcard.** It returns
`true` if *either* argument is NULL or zero-length, so a `""` reaching
any of the list scans above matches whichever entry happens to be first.
That is why the entry points reject `PMIX_NSPACE_INVALID(nspace)` rather
than merely `NULL == nspace`: without it, a proc carrying an empty
nspace would be forked with another job's cached envars — its transport
security key included — and a `deregister_nspace("")` would release some
other job's ports. Note that `allocate`/`setup_local_network` scan
`pmix_globals.nspaces` with plain `strcmp`, which has no such wildcard;
closing the door on an invalid nspace is what keeps the two styles
agreeing.

**`PMIX_DEVICE_ID` is documented as a string**, and the library reads it
that way elsewhere (`pmix_hwloc.c` takes `info->value.data.string`). A
host with several devices to report can therefore quite reasonably hand
over a `PMIX_DATA_ARRAY` of `PMIX_STRING` rather than of `PMIX_DEVICE`.
`get_assigned_devices` checks `darray->type` for exactly that reason:
reading a `char *[]` as `pmix_device_t[]` both runs off the end of the
allocation and — because `osname` sits where the array holds its *second*
element — resolves the process to a device it was never given. Declining
an assignment we cannot read is the only safe answer.

### The envar cache owns a namespace reference

`setup_local_network` does `PMIX_RETAIN(nsp)` before parking the
namespace in a `pmix_nspace_env_cache_t`, and the class destructor
(`nsenvdes`, in `src/include/pmix_globals.c`) is what gives it back — the
release does **not** belong at the pnet call site, because
`pmix_pnet_close` also destructs the whole list. Two consequences worth
holding on to:

- `pmix_pnet_base_deregister_nspace` must release the cache **after** the
  module fan-out, not before: once the server drops its own reference,
  the cache's is the only thing keeping the `pmix_namespace_t` the
  modules are being handed alive.
- `nsenvcon` sets `ns` to NULL explicitly. `PMIX_NEW` mallocs rather than
  callocs, so without that the destructor would release a garbage
  pointer.

`pgpu` uses the same class in the same way; a change to either half of
this contract has to account for both frameworks.

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
| `nvd` | 10 | **Yes** | hwloc reports a Mellanox `0x15b3` / NVIDIA `0x10de` device of class `0x207` |
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
├── nvd/                    Mellanox/NVIDIA example (default-built; hwloc-gated at runtime)
├── opa/                    Omni-Path example (default-built; hwloc-gated at runtime)
├── simptest/               static-endpoint test example (working; --with-simptest / --enable-test-build)
└── tcp/                    static TCP/UDP port example (--with-tcp; no hardware gate)
```

## Threading

`pnet` has **no caddy/thread-shift machinery of its own**, and the base
functions are synchronous transforms over the actives list and the
namespace/fabric lists. Most entry points do arrive on the progress
thread — `allocate`, `setup_local_network`, `child_finalized`,
`local_app_finalized` and `deregister_nspace` are all reached from server
code that has already thread-shifted (`_setup_app`,
`_setup_local_support`, `_deregister_nspace`).

**Two do not, and it is not safe to assume otherwise.**

- **`setup_fork` runs on the host's thread.** `PMIx_server_setup_fork`
  does no thread-shift at all — it cannot, since it has to fill in the
  caller's `env` array before the caller forks — so
  `pmix_pnet_base_setup_fork` walks `pmix_pnet_globals.nspaces` and
  issues a `PMIX_GDS_FETCH_KV` from whatever thread the RM called it on.
  What makes that work today is the host's own sequencing: a job's
  `PMIx_server_setup_local_support` (which *is* shifted, and whose
  blocking form waits on a lock) completes before that job's processes
  are forked, and `deregister_nspace` comes after they exit. Nothing in
  the library enforces it. An RM that forks job A's children while job
  B's `setup_local_support` is still in flight has one thread appending
  to that list while another traverses it. The same is true of
  `pmix_ptl_base_setup_fork` and `pmix_pgpu.setup_fork` next door, so
  this is a property of the whole `PMIx_server_setup_fork` path rather
  than of `pnet` — do not "fix" it here alone.
- **The three fabric entry points run on the caller's thread.**
  `PMIx_Fabric_register_nb` / `_update_nb` / `_deregister_nb` call
  straight into `pmix_pnet.*` when the local peer is a server or
  scheduler, with no shift — the blocking wrapper even *rejects* being
  called from the progress thread. Meanwhile `pmix_server_fabric.c`
  services relayed `PMIX_FABRIC_REGISTER_CMD` / `UPDATE_CMD` requests
  from the switchyard, which is on the progress thread, and both reach
  `pmix_pnet_globals.fabrics`. A scheduler that registers a plane from
  its main thread while a remote update is in flight is racing on that
  list, and `deregister_fabric` now removes items from it. The fix
  belongs in `src/client/pmix_client_fabric.c`, which is where the
  missing shift is.

The module interface *does* allow an asynchronous path —
`collect_inventory` / `deliver_inventory` / `register_fabric` are
documented to return `PMIX_OPERATION_IN_PROGRESS` and run a callback
later if a component must query a fabric manager on its own thread — but
no shipped component exercises that path today. If you add one, follow
the top-level thread-safety rules: shift to your own thread, return
`PMIX_OPERATION_IN_PROGRESS`, and fire the callback when done.

## The fabric path is scaffolding

`register_fabric`, `update_fabric` and `deregister_fabric` are wired all
the way through the base, but **no shipped component implements any of
them** — grep the four component directories and you will find the slots
unset. So `pmix_pnet_globals.fabrics` is never populated in a stock
build, every base fabric call ends in `PMIX_ERR_NOT_SUPPORTED` /
`PMIX_ERR_NOT_FOUND` / `PMIX_ERR_BAD_PARAM`, and there is no automated
coverage of any of it. Three things to know before you build on it:

- **`fabric->module` is never set.** The public `pmix_fabric_t` reserves
  that slot for the library, and `update_fabric`/`deregister_fabric` each
  carry a branch that casts it back to a `pmix_pnet_fabric_t *` — but
  `register_fabric` only ever writes NULL there, so that branch is dead
  and every lookup goes through the index/name scan. It was left
  deliberately unwired: a tracker pointer parked in a caller's object
  would dangle the moment the fabric was deregistered, and a host that
  copied the struct would keep the dangling copy.
- **Only `PMIX_OPERATION_SUCCEEDED` records a tracker.** A module that
  claims a plane by returning plain `PMIX_SUCCESS` (the async shape) is
  *not* recorded, yet both callers — `pmix_server_fabric.c` and
  `PMIx_Fabric_register_nb` — read `PMIX_SUCCESS` as "claimed, wait for
  the callback". A later update or deregister on that plane then answers
  `PMIX_ERR_BAD_PARAM`. Settle the async contract before relying on it.
- **`deregister_fabric` removes the tracker.** It did not use to, which
  grew the list without bound across register/deregister cycles and left
  a stale entry that still resolved a later request to a module that had
  already torn the plane down. The scan is now first-match-wins for the
  same reason.

## Building

The framework core (`base/`) is always built into `libpmix`. The
components are **conditionally compiled**, and their `configure.m4` files
are unusually blunt about it:

- **`opa` and `nvd`** ship **no `configure.m4` at all** and therefore
  build unconditionally; the MCA machinery configures a component with no
  `configure.m4` by itself. Keep it that way. Neither links anything or
  needs an SDK — they read info attributes hwloc already recorded and set
  environment variables — and whether a component has work to do is a
  property of the machine the *daemon* runs on, which only
  `component_open` is in a position to know. `nvd` used to be hardwired
  off (`AS_IF([test "yes" = "no"], …)`) on the grounds that "no real
  NVIDIA-transport detection exists yet"; that asked the question in the
  wrong place, so a cluster with Mellanox NICs got no support at all.
  `opa`'s said `AS_IF([test "yes" = "yes"], …)` — always succeed — which
  was worth less than the file it lived in. The one visible consequence
  of dropping both is that `configure`'s summary no longer prints
  `Transports / NVIDIA|OmniPath` lines for components that always build.
- **`tcp`** — guarded by `--with-tcp` (same `AC_ARG_WITH` pattern as
  `simptest`): **not built by default**. When the flag is given it
  compiles and, having no runtime hardware gate, also always selects.
- **`simptest`** — built when `--with-simptest` is passed to `configure`,
  or force-built for compile coverage by `--enable-test-build`.

`tcp` and `simptest` still report their state through
`PMIX_SUMMARY_ADD([Transports], …)`, so `configure`'s summary shows
`Simptest` and `TCP` as yes/no.

`simptest` ships a `show_help` file, `help-pnet-simptest.txt`; per the
top-level golden rule, after any add/delete/modify of that text you must
`rm src/util/pmix_show_help_content.* && make`. The other components ship
no `show_help` text.

Per the top-level build rules: editing a `Makefile.am` needs only a plain
`make`; **adding or removing a component directory, or changing a
`configure.m4`, changes the build wiring and requires
`./autogen.pl && ./configure … && make`**. This one bites in practice:
`nvd`'s `configure.m4` was deleted so the component would build by
default, and every tree configured before that kept on omitting it —
`static-components.h` still listed only `opa`, so `nvd` was silently
absent and `test/unit/pnet_assigned_devices` had nothing to exercise.
A plain `make` will not tell you; check `base/static-components.h`.

## When working in this framework

- **`opa`, `nvd`, `tcp`, and `simptest` are the current references.** All
  compile against today's interface. `opa` and `nvd` are built by default
  but only *run* on Omni-Path and Mellanox hardware respectively; `tcp` is
  opt-in (`--with-tcp`) and, once built, has no hardware gate so it always
  selects; `simptest` is opt-in (`--with-simptest` /
  `--enable-test-build`) and drives the endpoint/coordinate assignment
  path without real hardware. Read `opa` or `tcp` first if you need a
  template, or `simptest` for the static assign → cache → `Get` flow.
  `nvd` and `opa` are the reference for the per-rank `setup_fork` path,
  which `test/unit/pnet_assigned_devices.c` covers end to end.
- **`simptest` shows the per-proc vs. per-node split.** Fabric endpoints
  (`PMIX_FABRIC_ENDPT`) are per-process data, fetched by rank, so they go
  in a `PMIX_PROC_INFO_ARRAY` array; fabric coordinates
  (`PMIX_FABRIC_COORDINATES`) are per-node, fetched at the node level
  (rank `PMIX_RANK_UNDEF`), so they must be delivered in a
  `PMIX_NODE_INFO_ARRAY` rather than proc data or the client's `PMIx_Get`
  will not find them. This mirrors how a real fabric component must split
  the two.
- **Put per-job envars in the cache and per-rank envars in `setup_fork`.**
  An envar every rank of the job shares belongs in the namespace's envar
  cache, appended as a `pmix_envar_list_item_t` to `ns->envars` during
  `setup_local_network`; the base replays the cache into every child. An
  envar whose value differs per rank — a device assignment — has nowhere
  to be set but the component's own `setup_fork`, which the base calls
  afterwards. Do not reach for the topology there yourself: use
  `pmix_pnet_base_set_assigned_devices()` with the same PCI
  `(vendor, class)` pairs your `component_open` probed for, so a node
  carrying two fabrics does not have you naming somebody else's NIC.
- **Never invent a selector the topology did not supply.** A NIC's
  selector is the OS device name, which is always there. Where a
  variable takes a *unit ordinal* instead — PSM2's `HFI_UNIT`, OPX's
  `FI_OPX_HFI_SELECT` — leave it alone unless hwloc actually recorded
  that ordinal: an ordinal means something only against the enumeration
  it came from, and a wrong value in these variables does not fail, it
  quietly puts the process on somebody else's hardware.
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
