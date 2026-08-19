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
   `pmix_pgpu.child_finalized`, `.local_app_finalized` and
   `.deregister_nspace` run from `pmix_server_registration.c`, each
   immediately beside its `pnet` twin. Together with the long-standing
   `pmix_pgpu.collect_inventory` / `pmix_pgpu.deliver_inventory` calls,
   **every `pgpu` API entry now has an in-tree caller** — the envar
   harvest → ship → inject pipeline is wired exactly as `pnet`'s is.

   The teardown three were the last to be wired, and their absence was
   not cosmetic: `setup_local` caches one `pmix_nspace_env_cache_t` per
   namespace and *retains the namespace object* while it does, and
   `deregister_nspace` is the only thing that gives either back. Until
   the call existed a persistent server accumulated one of each per job
   it had ever run. If you add a hook to this framework, wire its caller
   in the same commit — an entry point nothing calls looks identical to
   one that works.

2. **The vendor components build everywhere now.** `amd`, `intel` and
   `nvd` used to gate themselves off unless `--enable-test-build` was
   passed, on the grounds that no vendor-runtime detection existed. That
   was the wrong place to ask the question: none of them links anything
   or needs a vendor SDK — they read info attributes hwloc already
   recorded and set environment variables — and the machine that builds
   PMIx is routinely not the machine that runs it, so a cluster with GPUs
   got no support unless somebody had thought to configure a test build.
   Detection happens at **run** time instead, in each component's
   `component_open`, which asks the local topology whether that vendor's
   hardware is present and declines if not.

3. **All three vendor components set their visible-devices variable.**
   `setup_fork` names the GPUs a process was mapped against in
   `CUDA_VISIBLE_DEVICES` / `ROCR_VISIBLE_DEVICES` / `ZE_AFFINITY_MASK`.
   What goes in the variable is the device's **selector**, which the
   topology layer produces: for NVIDIA and AMD that is the vendor's own
   device identifier, because their variables accept one; for Intel it is
   the Level Zero device ordinal the driver reported, because
   `ZE_AFFINITY_MASK` accepts nothing else. `intel` additionally states
   `ZE_FLAT_DEVICE_HIERARCHY`, since an ordinal only means something
   against a device hierarchy model - see
   [`intel/AGENTS.md`](intel/AGENTS.md).

4. **The inventory hooks are still stubs.** `collect_inventory` and
   `deliver_inventory` return `PMIX_SUCCESS` having done nothing in every
   component.

So the framework is wired into launch and the vendor components run in a
stock build, on any host whose topology shows that vendor's hardware.
Document and extend it faithfully, and do not describe the remaining
stubs as doing work the code does not yet do.

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
| `setup_fork` | `..._setup_fork_fn_t` — `(const pmix_proc_t *proc, char ***env)` | contribute this one process's GPU environment |

`setup_fork` is the per-*process* hook and is called by the base's own
`setup_fork` after it has replayed the namespace-wide envar cache — the
split is the point, since a GPU assignment differs per rank and the cache
can only carry what every rank shares.

The three vendor components fill in `name`, `allocate`, `setup_local`,
`setup_fork`, `collect_inventory`, and `deliver_inventory`; the `test`
component fills `name`, `allocate`, and `setup_local`. All other slots are
left `NULL` and the base skips them.

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
- The three **`PMIX_MCA_pgpu_*_VERSION`** macros state the framework's
  interface version — the numbers `PMIX_MCA_BASE_VERSION(pgpu)` stamps
  into every component struct.

## Directory layout

```
src/mca/pgpu/
├── pgpu.h                  Framework public API: the two module structs + version macro
├── base/                   Framework infrastructure
│   ├── base.h              Internal base API: globals, active-module class, base fn decls
│   ├── pgpu_base_frame.c   open/close, framework decl, global pmix_pgpu, active-module class
│   ├── pgpu_base_select.c  hand-rolled multi-select (query + priority insert)
│   └── pgpu_base_fns.c     the pmix_pgpu_base_* routing functions
├── amd/                    AMD vendor component  (always built; declines at run time)
├── intel/                  Intel vendor component (always built; declines at run time)
├── nvd/                    NVIDIA vendor component (always built; declines at run time)
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
  first looks up the proc's nspace in the envar cache and, for each
  cached `pmix_envar_list_item_t`, calls `PMIx_Setenv(...)` to inject it
  into `*env`; **then** calls each active module's `setup_fork`. The
  split is the point: the cache is per *namespace*, so it can only carry
  what every rank of the job shares, and a GPU assignment is per *rank*.
  A module return of `PMIX_ERR_NOT_AVAILABLE` or
  `PMIX_ERR_TAKE_NEXT_OPTION` means "nothing to say about this process",
  which is the ordinary case; any other error aborts the fork rather than
  launching a process that would use the wrong hardware.

  Two things about this hook are worth knowing before touching it. It was
  **unreachable until recently** — `PMIx_server_setup_fork` called
  `pmix_pnet.setup_fork` and `pmix_pmdl.setup_fork` but never
  `pmix_pgpu.setup_fork`, so `allocate` harvested envars, `setup_local`
  cached them, and nothing ever replayed them into a child. And it is the
  only pgpu hook that runs on the **daemon that will fork the process**,
  which is why per-node truth belongs here: a device's vendor identity
  differs from node to node, and the head node's copy of a topology may
  belong to whichever node reported it first.

  `pmix_pgpu_base_get_visible_devices()` is the shared implementation the
  vendor modules build on — it reads the proc's `PMIX_DEVICE_ID` out of
  the local datastore, resolves each device against the local topology,
  keeps the ones belonging to the caller's vendor, and joins their
  **selectors** (`pmix_hwloc_device_t.selector`) with commas. It checks
  the data array's **element type**, not just that the value is one:
  `PMIX_DEVICE_ID` is a `char*` everywhere except in a process's own proc
  info, where it is an array of `pmix_device_t`, and the host stored it —
  so taking an array of anything else as `pmix_device_t` would read
  `osname` out of whatever happened to sit at that offset.
  `pmix_pgpu_base_set_visible_devices()` is the one-line wrapper that
  writes that value into the named variable; `nvd` and `amd` call it
  directly. A component that has to inspect the environment *before*
  writing - `intel` does - calls the getter instead, because a check that
  runs after the variable is set is no check at all.
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

### The decompress return is not optional

`setup_local` in every component reads a blob that may arrive as a
`PMIX_COMPRESSED_BYTE_OBJECT`, and `pmix_compress.decompress` returns a
**bool**, not a status. When it returns false it leaves `*outbytes`
**untouched** — the `pcompress` base's own default implementation writes
neither output, and `zlib`'s writes `*outlen = 0` and returns without
writing the pointer. So a caller that ignores the return goes on to load
a buffer from an indeterminate stack pointer and then `free()` it.

This is not a corrupted-input-only path. Compression is an optional
component, the blob's wire type is chosen by whichever node ran
`allocate`, and the two need not have been built alike — a node with no
`pcompress` component receiving a compressed blob from one that had it is
an ordinary mixed-installation outcome, and it aborts on the `free`.
Check the return and decline the blob; `test/unit/pgpu_envar_blob.c`
pins the behavior, and `pnet/nvd` carries the same guard.

### Setting an envar is the whole job

`pmix_pgpu_base_setup_fork`, `pmix_pgpu_base_set_visible_devices` and
`intel`'s `setup_fork` all check what `PMIx_Setenv` returned. Returning
`PMIX_SUCCESS` when the variable was not written launches a process that
sees every device on the node instead of the ones it was assigned — a
silently wrong job rather than a failed one, which is the worse of the
two outcomes and the harder to diagnose. The same reasoning applies to
the argv the device list is accumulated in: a dropped entry reads as a
smaller assignment, not as an error.

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
`PMIX_SUCCESS` with an empty `actives` list, which is the case on any
host whose topology shows none of the three vendors' hardware (`test` is
compiled only with `--with-pgpu-test`). At verbosity >4 it prints the
resolved priority list.
`selected` guards against a second pass.

Default component priorities (from each `component_query`):

| Component | Priority | `component_open` gate (only if built) |
|-----------|----------|----------------------------------------|
| `amd`   | 20 | `pmix_hwloc_check_vendor_baseclass(topo, 0x1002, 0x03)` |
| `intel` | 20 | `pmix_hwloc_check_vendor(topo, 0x8086, 0x0380)` |
| `nvd`   | 10 | `pmix_hwloc_check_vendor_baseclass(topo, 0x10de, 0x03)` |
| `test`  | 10 | server role **and** `pgpu=test` named in the MCA selection string |

`pmix_hwloc_check_vendor_baseclass` (in `src/hwloc/pmix_hwloc.c`) walks
the PCI devices in the node topology and returns `PMIX_SUCCESS` only if
one of them is from the given vendor and in the given PCI **base** class
— `0x03`, display controller, for every GPU. The subclass is deliberately
not part of the question: one vendor's GPUs report `0x0300`, `0x0302` and
`0x0380` depending on the part, so matching one exactly means declining
on hardware that is plainly there. (`intel` still uses the exact-class
`pmix_hwloc_check_vendor` because Intel's *integrated* graphics are
`0x0300` and are not what this component is for.) Either form returns
`PMIX_ERR_NOT_AVAILABLE` when no such device is present and
`PMIX_ERR_TAKE_NEXT_OPTION` when the topology is not hwloc-sourced. A
non-success `component_open` prevents the component from being queried, so
each activates **only on a host that actually has that vendor's GPU**.

**Return `PMIX_ERR_NOT_AVAILABLE`, and nothing else, to decline.** That
is the MCA's "silently ignore me" cue (see
[`src/mca/AGENTS.md`](../AGENTS.md)); every other status is reported as a
component that *failed to open*. The vendor check's own
`PMIX_ERR_TAKE_NEXT_OPTION` is therefore normalized rather than passed
through — these components open on every server now, so handing that
status back would announce a failure on every host without a GPU.

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
`libmca_pgpu.la` with `sources =` empty apart from the base:

- The three **vendor** components (`amd`, `intel`, `nvd`) ship a
  `Makefile.am` and **no `configure.m4`** — they build unconditionally,
  and the MCA machinery configures a component with no `configure.m4`
  by itself. Keep it that way: there is nothing to look for (no library,
  no SDK, no header), and whether a component has work to do is a
  property of the machine the *daemon* runs on, which only
  `component_open` is in a position to know. They previously carried a
  `configure.m4` that did nothing but always succeed; the one visible
  consequence of dropping it is that the configure summary no longer
  prints a `GPUs / AMD|Intel|NVIDIA: yes` line for a component that
  always builds.
- The **`test`** component does ship a `configure.m4`, and that one
  earns its place: it adds the `--with-pgpu-test` `AC_ARG_WITH` that
  keeps the component out of a normal build. It is the way to exercise
  the launch path on a host with no GPU (select it at runtime with
  `PMIX_MCA_pgpu=test`).

Merely editing a `Makefile.am` needs only `make`, but adding or removing
a `configure.m4` — or a component directory — changes the wiring
`configure` resolves, so the full `./autogen.pl && ./configure && make`
regen is required. The `intel` component ships `help-pgpu-intel.txt`, so
the regenerate-the-help-content golden rule **does** apply when you touch
it: `rm src/util/pmix_show_help_content.*` and rebuild, or the library
keeps emitting the old text. No other component here has help text.

## When adding or modifying a component

- Open the component struct with `PMIX_MCA_BASE_VERSION(pgpu)` and set
  `.pmix_mca_component_name` to your directory name. Emit the component-
  init symbol with `PMIX_MCA_BASE_COMPONENT_INIT(pmix, pgpu, <name>)` —
  the third argument **must** match the struct's component name
  (`pmix_mca_pgpu_<name>_component`), or the static-component pointer will
  reference an undefined symbol (this was the `nvd` bug, since fixed).
- Provide a `component_query` that hands back your `pmix_pgpu_module_t` and
  a priority, and a `component_open` that declines (returns non-success)
  when the vendor hardware is absent — reuse
  `pmix_hwloc_check_vendor_baseclass`, not the exact-class
  `pmix_hwloc_check_vendor`, unless a subclass really is the question.
- Fill only the module slots you implement; leave the rest `NULL` — the
  base checks each pointer before calling.
- If you harvest envars, mirror the existing components: gate on
  `PMIX_SETUP_APP_ENVARS` / `PMIX_SETUP_APP_ALL`, harvest with
  `pmix_util_harvest_envars`, pack as `PMIX_ENVAR`, compress via
  `pmix_compress`, and stash under a unique per-component blob key so
  `setup_local` can find and unpack it.
- **Ship no `configure.m4` at all** unless the component genuinely needs
  something at build time (a header, a library to link, or a deliberate
  opt-in). A component that only reads the topology and sets envars should
  build everywhere and decline at run time in `component_open` — the build
  host is not the run host, so a build-time gate answers the wrong
  question — and a `configure.m4` whose whole body is "succeed, and add a
  summary line saying so" is worth less than the file it lives in. The
  `test` component's `--with-pgpu-test` `AC_ARG_WITH` is the template for
  the case that does earn one: it exists so a component meant only for
  testing does not load on a real system.
- **Put the run-time check in `component_open`, not `component_query`.**
  The topology is loaded (`pmix_hwloc_setup_topology`) before the server
  opens this framework, so `component_open` can see it; and a component
  that declines to open is never queried, so the same check in
  `component_query` would be dead code. `component_query` should only hand
  back the module and a priority.
- The launch API calls (`pmix_pgpu.allocate` / `.setup_local` /
  `.setup_fork`) are already wired into the server the way `pnet` is; a
  new component inherits them for free. The `test` component is the
  working reference for a component that harvests and replays envars
  through that path.
