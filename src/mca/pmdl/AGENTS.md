<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PMDL Framework

This document orients AI agents and human contributors working in the
`pmdl` (**P**rogramming-**M**o**d**e**l**) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety model, and MCA concepts
described there all apply here and are not repeated. This file covers
what is specific to `pmdl`: what the framework is for, the (unusual)
two-struct module/API split, how a request is fanned across every active
model, the base helpers each component leans on, and the contract a
component must honor. The one component subdirectory (`ompi/`) carries
its own [`AGENTS.md`](ompi/AGENTS.md) with component-specific detail.

There is no `docs/how-things-work/` page for `pmdl`.

## What PMDL does

`pmdl` collects the **environment variables and MCA parameters that a
programming model needs**, and injects them into application processes
before they launch. When a launcher spawns an Open MPI job, each MPI
process expects to find a pile of `OMPI_*` variables in its environment
(`OMPI_COMM_WORLD_SIZE`, `OMPI_COMM_WORLD_RANK`, `OMPI_UNIVERSE_SIZE`,
…), plus any `OMPI_MCA_*` / `OPAL_*` tuning the user set in MCA parameter
files or in the launching environment. `pmdl` is the server-side machinery
that gathers all of that and hands it to the fork/exec path.

Concretely, it does four kinds of work:

1. **Harvest** — scan the launching environment and MCA parameter files
   for variables belonging to a programming model, and stage them (as
   `PMIX_SET_ENVAR` directives) to be forwarded to the spawned processes.
2. **Per-nspace bookkeeping** — when a namespace is registered, notice
   whether it contains apps of a given model and cache the job-level facts
   (universe/job/local/app sizes, app leaders) the model will need.
3. **Per-process fork setup** — just before a local child is exec'd,
   compute and `PMIx_Setenv` the model-specific variables that depend on
   *that* rank (its rank, local rank, node rank, cwd, argv, …).
4. **Teardown** — drop the per-nspace tracking when a namespace is
   deregistered.

Everything `pmdl` produces is *additive* environment for the child; it
never opens sockets, allocates resources, or talks to a peer. Compare
`pnet` (endpoints/"instant on") — a different framework with a similar
producer/consumer shape but an unrelated purpose.

## Where it runs

`pmdl` is opened and selected only inside a **server** and a **tool**:

- `src/server/pmix_server.c` opens `pmix_pmdl_base_framework` and calls
  `pmix_pmdl_base_select()` during server init (search for
  `pmix_pmdl_base_select`), then immediately drives
  `pmix_pmdl.parse_file_envars(...)` over the MCA file/override values.
- `src/tool/pmix_tool.c` does the same during tool init (a launcher is a
  tool, and must set up the environment for the children it spawns).

The public entry points are reached through the global `pmix_pmdl` from:
`pmix_server.c` (`register_nspace`, `deregister_nspace`, `setup_fork`,
`harvest_envars`), `pmix_client_spawn.c` (`harvest_envars` during a
`PMIx_Spawn`), and `gds/hash` (`setup_nspace`, `setup_nspace_kv`,
`setup_client` while unpacking job data). Because the global is
statically initialized to the base stubs (see below), those calls are
safe even in a role where the framework was never opened — the stubs gate
on the `initialized` flag and return `PMIX_ERR_INIT` rather than touch
any state.

## Multi-select: the base fans every request

`pmdl` is a **multi-select** framework. `pmix_pmdl_base_select()`
(`base/pmdl_base_select.c`) queries every component, calls each returned
module's `init` (dropping any that fail), wraps the survivors in a
`pmix_pmdl_base_active_module_t { pri, module, component }`, and inserts
them into `pmix_pmdl_globals.actives` in **descending priority order**.
Unlike a single-select framework there is no "winner": *every* active
module is offered *every* request.

The fan-out pattern in every base stub is uniform:

```c
PMIX_LIST_FOREACH (active, &pmix_pmdl_globals.actives, ...) {
    if (NULL != active->module-><fn>) {
        rc = active->module-><fn>(...);
        if (PMIX_SUCCESS != rc && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
            return rc;   // a real error stops the fan
        }
    }
}
```

So a module that does not care about a given request returns
**`PMIX_ERR_TAKE_NEXT_OPTION`** (treated as "not mine, keep going"); only
a genuine failure aborts the loop. Selection finding *zero* components is
**not** fatal — the base stubs still run their own model-agnostic work
(param-file harvesting, `PMIX_MCA_*` collection), so `pmdl` degrades to
"forward the generic MCA environment and nothing model-specific."

Today there is exactly one component, `ompi` (priority 55), so the
prioritized list has one entry — but the machinery is genuinely
multi-select, and a second model component would slot in by priority
with no base changes.

## The two-struct split (module vs. API) — read this first

This is the single most important structural fact about `pmdl`, and the
thing most likely to trip you up. [`pmdl.h`](pmdl.h) declares **two**
different structs:

- **`pmix_pmdl_module_t`** — what a *component* implements. Its function
  pointers take a resolved `pmix_namespace_t *` and, for the two harvest
  paths, an extra `char ***priors` argument.
- **`pmix_pmdl_API_module_t`** — the type of the exported global
  **`pmix_pmdl`** that the *rest of the library* calls. Its harvest and
  deregister entries take a `char *nspace` / `const char *` **name**
  instead of a `pmix_namespace_t *`, and its `setup_fork` /
  `harvest_envars` drop the `priors` argument.

`pmix_pmdl` is statically initialized in `base/pmdl_base_frame.c` to point
every slot at a `pmix_pmdl_base_*` stub. The **stubs are the adapter
layer** between the two structs: they take the caller's namespace *name*,
look up (or create) the matching `pmix_namespace_t`, own the transient
`priors` argv, and fan the resolved call across `actives`. A component
never sees a name string and never allocates `priors`; the base does that
for it.

### What `priors` is for

`harvest_envars` and `setup_fork` pass a `char ***priors` down to each
module. It is an argv of model names that have *already handled this
request*. A component appends its own name (e.g. `"ompi"`) the first time
it acts, and bails with `PMIX_ERR_TAKE_NEXT_OPTION` if it finds its name
already present. This dedups the (real) case where the same model would
otherwise be invoked twice for one job — for example when both an
"ompi5"-tagged and a bare "ompi"-tagged app map to the same component.
The base allocates `priors` on the stack of the stub, threads it through
the fan, and `PMIx_Argv_free`s it at the end. Do not persist a pointer to
it.

## Module interface (`pmix_pmdl_module_t`)

Defined in [`pmdl.h`](pmdl.h). A component may leave any pointer `NULL`;
the base skips it in the fan.

| Field | Signature (abbrev.) | Purpose |
|-------|---------------------|---------|
| `name` | `char *` | component name string, and the `priors` token |
| `init` / `finalize` | `(void)` | per-module lifecycle; `init` may veto selection by returning non-success |
| `harvest_envars` | `(nptr, info, ninfo, ilist, &priors)` | append model envars (as `PMIX_SET_ENVAR` kvals) to `ilist` for forwarding to children |
| `parse_file_envars` | `(ilist)` | pluck model-owned entries out of an already-parsed MCA-file value list and cache them |
| `setup_nspace` | `(nptr, info)` | note that this nspace hosts the model (single `pmix_info_t`) |
| `setup_nspace_kv` | `(nptr, kv)` | same, but from a single `pmix_kval_t` (with model-version parsing) |
| `register_nspace` | `(nptr)` | cache job-level facts for the nspace; add derived job-info to GDS |
| `setup_client` | `(nptr, rank, appnum)` | per-client hook (unused by `ompi`) |
| `setup_fork` | `(proc, &env, &priors)` | inject per-rank model envars into a child's `env` before exec |
| `deregister_nspace` | `(nptr)` | drop the per-nspace tracker |

The corresponding `pmix_pmdl_API_module_t` differs only in the three
signatures the stubs adapt: `harvest_envars(char *nspace, …)` (no
`priors`), `setup_fork(proc, &env)` (no `priors`), and
`deregister_nspace(const char *nptr)`.

## The base routing/helper layer (`base/pmdl_base_stubs.c`)

Every entry in `pmix_pmdl` points at a `pmix_pmdl_base_*` stub here. Most
are the plain fan described above; two carry additional base-level logic,
and two are shared helpers components call back into.

- **`pmix_pmdl_base_harvest_envars(nspace, info, ninfo, ilist)`** — does
  real work *before and after* the fan:
  1. Prepends every value already parsed from MCA param files
     (`pmix_mca_base_var_file_values`) and command-line overrides
     (`pmix_mca_base_var_override_values`) into `ilist` as
     `PMIX_MCA_<var>` `PMIX_SET_ENVAR` directives, so model components can
     override them afterward.
  2. Resolves/creates the `pmix_namespace_t` for `nspace` (it may not be
     registered yet) and fans `harvest_envars` across `actives`, owning
     the `priors` argv.
  3. Harvests the local `PMIX_MCA_*` environment via
     `pmix_util_harvest_envars`, then appends a `PMIX_PARAM_FILE_PASSED=1`
     marker so children know the params were already forwarded.
- **`pmix_pmdl_base_setup_fork(proc, &env)`** — resolves nothing extra;
  just owns `priors` and fans `setup_fork`.
- **`pmix_pmdl_base_deregister_nspace(name)`** — looks the namespace up by
  name and fans `deregister_nspace`; a miss is silently ignored.
- The remaining stubs (`parse_file_envars`, `setup_nspace`,
  `setup_nspace_kv`, `register_nspace`, `setup_client`) are straight fans.

### Two shared classification helpers

`pmdl_base_stubs.c` also exports two predicates that components use to
decide how to re-prefix a variable read from an MCA param file:

| Helper | Returns true when the param name… |
|--------|-----------------------------------|
| `pmix_pmdl_base_check_pmix_param(param)` | starts with `pmix` or names a PMIx framework (from the built-in `pmix_framework_names`) |
| `pmix_pmdl_base_check_prte_param(param)` | starts with `prte` or names a PRRTE framework |

The PRRTE framework list is a hard-coded `prte_frameworks_static_3_0_1[]`
table that can be **overridden at runtime** by the `PRTE_MCA_PREFIXES`
environment variable (comma-delimited). These predicates exist because a
single `openmpi-mca-params.conf` can hold PMIx-, PRRTE-, and OMPI-directed
values, and each must be re-prefixed (`PMIX_MCA_`, `PRTE_MCA_`,
`OMPI_MCA_`) before being forwarded. See the `ompi` component doc for how
they are used.

## Core data structures

### `pmdl.h` — the two module structs and the version

`pmdl.h` is the framework's public header, the contract every component
compiles against. It defines `pmix_pmdl_module_t`,
`pmix_pmdl_API_module_t`, the exported global `pmix_pmdl`, the component
typedef `pmix_pmdl_base_component_t` (a bare
`pmix_mca_base_component_t`), and **`PMIX_PMDL_BASE_VERSION_1_0_0`**, the
version macro every component struct must open with.

### `base/base.h` — the active-module wrapper and globals

- **`pmix_pmdl_base_active_module_t`** `{ super, pri, module, component }`
  — one selected module on the `actives` list (`PMIX_CLASS_INSTANCE` in
  `pmdl_base_frame.c`).
- **`pmix_pmdl_globals_t`** `{ lock, actives, initialized, selected }` —
  the framework-wide state, exported as `pmix_pmdl_globals`. `initialized`
  gates every stub (they return `PMIX_ERR_INIT` if the framework was never
  opened); `selected` makes `pmix_pmdl_base_select` idempotent.

The kvals a component appends to the harvest `ilist` are ordinary
`pmix_kval_t` carrying a `PMIX_SET_ENVAR` (or `PMIX_PREPEND_ENVAR`) key
and a `PMIX_ENVAR`-typed value — the standard "set this in the child's
environment" directive the fork path understands.

## Selection and lifecycle (`base/pmdl_base_frame.c`)

- `PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pmdl, …)` declares the framework
  with the "PMIx Programming Model Operations" description.
- `pmix_pmdl_open` sets `initialized`, constructs the lock and the
  `actives` list, and opens all components.
- `pmix_pmdl_close` finalizes and releases every active module, destructs
  the list and lock, and closes the components.
- `pmix_pmdl_register` registers **no MCA parameters of its own.** It
  exists solely to install two MCA *aliases* so the historical `ompi5` and
  `ompi4` component names (and their MCA vars) still resolve to today's
  unified `ompi` component:
  ```c
  pmix_mca_base_alias_register("pmix", "pmdl", "ompi", "ompi5", …);
  pmix_mca_base_alias_register("pmix", "pmdl", "ompi", "ompi4", …);
  ```
  This deliberately breaks MCA abstraction (naming a specific component in
  the base) because aliases must be registered before any component is —
  the in-code comment explains why. Keep it.
- `pmix_pmdl_base_select` is the multi-select routine described above.

## MCA parameters

The framework base registers none. The only `pmdl` MCA parameters are the
`ompi` component's `include_envars` / `exclude_envars` — see its
[`AGENTS.md`](ompi/AGENTS.md). Per the top-level guidance, prefer a new
attribute (honored inside `harvest_envars`/`setup_fork`) or a
component-scoped MCA parameter over a hard-coded constant.

## Threading

`pmdl` entry points are synchronous transforms invoked from code already
running on the appropriate progress thread (the server's namespace
registration, the fork/exec path, `gds/hash` unpack, `PMIx_Spawn`
processing). They read shared state (`pmix_globals.nspaces`, GDS via
`PMIX_GDS_FETCH_KV`) and a component's private nspace list directly; they
do **not** thread-shift, block, or use the caddy pattern. Do not invent a
path that calls a `pmix_pmdl.*` API from an arbitrary user thread without
thread-shifting first. The `pmix_pmdl_globals.lock` exists but the stubs
gate on the `initialized` flag rather than taking it on the hot path.

## Directory layout

```
src/mca/pmdl/
├── pmdl.h                  Framework API: module struct, API-module struct, version macro
├── base/
│   ├── base.h              Internal base API + globals + active-module wrapper
│   ├── pmdl_base_frame.c   open/close/register, framework decl, MCA aliases, class instance
│   ├── pmdl_base_select.c  multi-select: build the prioritized actives list
│   └── pmdl_base_stubs.c   the pmix_pmdl_base_* fan routers + the two param-classification helpers
└── ompi/                   Open MPI programming-model component (the only one)
```

## Building

The framework core (`base/`) is always built into `libpmix`. The `ompi`
component ships **no `configure.m4`**, so it is not conditionally
compiled out — it is wired in through the generated
`base/static-components.h` and is always present. `pmdl` ships **no
`show_help` text**, so the regenerate-the-help-content golden rule does
not bite here. Editing a `Makefile.am` only needs a plain `make`; adding
or removing a *component directory* changes the build wiring resolved by
`configure`, so re-run `./autogen.pl && ./configure … && make`.

Because `pmdl` is only meaningfully exercised inside a server/launcher,
test a real change by driving an actual launch (e.g. `prterun` under
PRRTE, or the `simptest`/spawn tests) and inspecting the environment the
children receive.

## When working in this framework

- **Return `PMIX_ERR_TAKE_NEXT_OPTION`, never an error, when a request is
  not yours.** A real error return aborts the fan and can strand other
  models' setup. Reserve non-success returns for genuine failures.
- **Respect the `priors` dedup contract.** In `harvest_envars` and
  `setup_fork`, check `*priors` for your name before acting and append it
  when you do. Do not free or retain the argv — the base owns it.
- **Never confuse the two structs.** Components fill a
  `pmix_pmdl_module_t` (namespace pointer, `priors`); callers use the
  `pmix_pmdl` `pmix_pmdl_API_module_t` (name string, no `priors`). If you
  add a function, add it to *both* and teach the base stub to adapt
  between them.
- **Environment you emit must be additive and idempotent.** Multiple
  overlapping models, repeated `register_nspace` calls, and re-launches
  all happen; guard with the "already given" marker envars (as `ompi`
  does with `OPAL_*_PARAMS_GIVEN`) rather than assuming single-shot.
