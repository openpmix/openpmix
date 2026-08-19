<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PMDL `ompi` Component

`ompi` is the `pmdl` component that sets up the environment an **Open MPI**
(and OpenSHMEM / OPAL) application expects. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `ompi`. It is the only `pmdl` component today, and it fills in every
slot of the `pmix_pmdl_module_t` except `setup_client`, which it leaves
`NULL`.

## Files

| File | Contents |
|------|----------|
| `pmdl_ompi.h` | Declares the component struct (`pmix_pmdl_ompi_component_t`) and the module/component symbols. |
| `pmdl_ompi_component.c` | Component struct + `component_register` (the two MCA vars) + `component_query` (priority **55**, always). |
| `pmdl_ompi.c` | The module: `harvest_envars`, `parse_file_envars`, `setup_nspace`/`_kv`, `register_nspace`, `setup_fork`, `deregister_nspace`, and their helpers. |

## When it is selected

`component_query` unconditionally returns the module with **priority 55**
— there is no environment gate and no `configure.m4`, so `ompi` is always
built and always active. Whether it actually *does* anything for a given
job is decided per-request by `checkus()` (below), not at selection time.

The component was historically split into `ompi4` / `ompi5`; those names
are preserved as MCA aliases registered by the framework base (see the
framework doc), so an `--mca pmdl ompi5` still resolves here.

## MCA parameters (`component_register`)

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `pmix_pmdl_ompi_include_envars` | `"OMPI_*,OPAL_*"` | comma-list of environment-variable globs (`*`/`?`) to harvest from the launch environment |
| `pmix_pmdl_ompi_exclude_envars` | *(none)* | comma-list of globs to exclude from that harvest |

Both are split into argv (`include` / `exclude`) stored on the component
struct and handed to `pmix_util_harvest_envars` during `harvest_envars`.

## The component struct

```c
typedef struct {
    pmix_pmdl_base_component_t super;
    char *incparms; char *excparms;   // the raw MCA-var strings
    char **include;  char **exclude;  // split into globs
} pmix_pmdl_ompi_component_t;
```

## "Is this ours?" — `checkus()`

`harvest_envars` and `setup_nspace` gate on `checkus(info, ninfo)`, which
returns true only if some `pmix_info_t` in the array carries
`PMIX_PROGRAMMING_MODEL` or `PMIX_PERSONALITY` whose string value
*contains* `"ompi"`. `setup_nspace_kv` does the same test on a single
`pmix_kval_t` but adds **version parsing**: a bare `"ompi"` is taken
(service it just in case), while `"ompiN"` is taken only if `N >= 5` (this
component represents the Open MPI v5 framework layout). A request that is
not ours yields `PMIX_ERR_TAKE_NEXT_OPTION`.

**Screen the value before reading it as a string.** These keys reach this
component two ways it does not control: from the `pmix_info_t` array a
host handed `PMIx_server_register_nspace`, and from job-level data
`gds/hash` unpacked from a peer (`process_arrays.c` and `gds_hash.c` both
call `setup_nspace`/`setup_nspace_kv` the moment they see one of these
keys). Neither validates the type first, so `data.string` on a value that
is not `PMIX_STRING` is whatever bit pattern the union happened to hold —
`model_string()` is the one place that check lives, and every read of one
of these values goes through it. Remember too that `PMIx_Argv_split()`
answers **NULL** for a string with no tokens, so an empty personality has
no `tmp[0]` to look at.

## Per-nspace tracking

The module keeps a private `mynspaces` list of `pmdl_nspace_t`
`{ nspace, univ_size, job_size, local_size, num_apps }`, each field
initialized to `UINT32_MAX` ("not yet known"). `setup_nspace` /
`setup_nspace_kv` create the tracker when they recognize an ompi job;
`register_nspace` fills the size fields from GDS; `setup_fork` reads them;
`deregister_nspace` removes and releases the tracker. `ompi_init` /
`ompi_finalize` construct and destruct `mynspaces` (and `myenvars`, below).

## What each entry point does

### `harvest_envars`

Gated on `checkus` and on the `priors` dedup. It only actually harvests if
some info key is `PMIX_SETUP_APP_ENVARS` — otherwise it returns
`PMIX_ERR_TAKE_NEXT_OPTION`. When it does run, it:

- processes the Open MPI **MCA param files** it can find —
  `$OMPIHOME/etc/openmpi-mca-params.conf` and
  `~/.openmpi/mca-params.conf` (home resolved from `PMIX_USERID` or
  `geteuid`) — via `process_param_file`, and appends
  `OPAL_SYS_PARAMS_GIVEN=1` / `OPAL_USER_PARAMS_GIVEN=1` markers so the MPI
  process does not re-read them;
- harvests the launch environment through the component's
  `include`/`exclude` globs (`OMPI_*,OPAL_*` by default);
- special-cases **`OPAL_PREFIX`**: if harvested, it also emits a
  `PMIX_PREPEND_ENVAR` for `LD_LIBRARY_PATH` pointing at
  `$OPAL_PREFIX/<libdir-basename>` (the basename taken from
  `pmix_pinstall_dirs.libdir`), so the child can find the matching
  libraries;
- appends anything previously stashed in `myenvars` (see
  `parse_file_envars`).

### `process_param_file` (helper)

Parses one MCA param file and, for each variable, decides how to
re-prefix it before forwarding, using the framework's shared classifiers:
`pmix_pmdl_base_check_pmix_param` → `PMIX_MCA_<var>`,
`pmix_pmdl_base_check_prte_param` → `PRTE_MCA_<var>`, otherwise assume it
is an Open MPI param → `OMPI_MCA_<var>`. Each becomes a `PMIX_SET_ENVAR`
kval on the harvest `ilist`, through `add_envar()`/`add_prefixed_envar()`
— the one place that knows a file value may be `NULL` (see the framework
guide's "A param-file value can be `NULL`").

The precedence is deliberate and the first test wins, which decides the
one name the two framework tables share: `mca_base_*` in an
`openmpi-mca-params.conf` goes out as `PMIX_MCA_mca_base_*`, because
`"mca"` is in `pmix_framework_names`. That is arguably the wrong owner for
a file Open MPI wrote, but changing it would silently move a live setting
between libraries; it is left as-is and recorded here rather than
"fixed" in passing.

### `parse_file_envars`

Called once at server/tool startup over the already-parsed MCA file
values. It moves every entry naming a known **Open MPI framework** out of
the shared list and into the module's private `myenvars`, renaming it to
`OMPI_MCA_<var>`. The framework list is the hard-coded
`ompi_frameworks_static_5_0_0[]` table (btl, coll, pml, osc, … plus
`mca`/`opal`/`ompi` and the OSHMEM frameworks), overridable at runtime by
the `OMPI_MCA_PREFIXES` environment variable. Those stashed vars are
re-emitted later by `harvest_envars` and `setup_fork`.

Two things about the match are load-bearing, and both were wrong:

- It matches on the **framework-name boundary** (`in_framework()`): the
  name must be the framework itself (`btl = self,tcp` is a real param
  file line) or be followed by `_`. A bare prefix comparison also claims
  `iface_name` for `if` and `opal_x` for `op`.
- A parameter **PMIx claims is left alone.** The list this is handed is
  what PMIx read from *its own* param files, and two names appear in both
  framework tables — `mca`, and `pmix` (OPAL carried a `pmix` framework).
  Taking those meant a value the user set for PMIx, such as
  `pmix_hwloc_topo_file`, was renamed `OMPI_MCA_*` and so reached neither
  library: it was gone from the list `harvest_envars` prefixes, and Open
  MPI has no such parameter.

### `register_nspace`

For a tracked nspace, fetches `PMIX_UNIV_SIZE`, `PMIX_JOB_SIZE`,
`PMIX_JOB_NUM_APPS`, and (best-effort) `PMIX_LOCAL_SIZE` from GDS into the
tracker. If the job has more than one application, it also builds
space-separated `OMPI_APP_SIZES` (per-app `PMIX_APP_SIZE`) and
`OMPI_FIRST_RANKS` (per-app `PMIX_APPLDR`) strings and caches them back
into GDS as job info via `PMIX_GDS_CACHE_JOB_INFO`. A single-app job
returns early. An unknown nspace returns `PMIX_ERR_TAKE_NEXT_OPTION`.

### `setup_fork`

The per-rank workhorse, gated on the `priors` dedup and on the nspace
being tracked. It `PMIx_Setenv`s the model variables the process needs,
drawing sizes from the tracker and per-rank facts from GDS:

- sizes: `OMPI_UNIVERSE_SIZE`; `OMPI_COMM_WORLD_SIZE`, `OMPI_WORLD_SIZE`,
  `OMPI_MCA_num_procs` (all = job size); `OMPI_COMM_WORLD_LOCAL_SIZE`,
  `OMPI_WORLD_LOCAL_SIZE`; `OMPI_NUM_APP_CTX` (= num apps);
- location/command: `OMPI_FILE_LOCATION` (`PMIX_PROCDIR`),
  `OMPI_MCA_initial_wdir` (`PMIX_WDIR`), `OMPI_COMMAND` + `OMPI_ARGV`
  (split from `PMIX_APP_ARGV`), `OMPI_MCA_cpu_type` (`uname().machine`);
- per-rank: `OMPI_COMM_WORLD_RANK` (`proc->rank`),
  `OMPI_COMM_WORLD_LOCAL_RANK` (`PMIX_LOCAL_RANK`),
  `OMPI_COMM_WORLD_NODE_RANK` (`PMIX_NODE_RANK`);
- multi-app only: `OMPI_APP_CTX_NUM_PROCS` (per-app sizes),
  `OMPI_FIRST_RANKS` (per-app leaders);
- `OMPI_MCA_num_restarts` (`PMIX_REINCARNATION`), **if the host provided
  one** — nothing in PMIx sets that key, and a host that has never
  restarted the proc has nothing to say. Its absence is not a failure;
  returning the fetch error from here fails `PMIx_server_setup_fork` and
  with it the child's launch;
- finally, every `myenvars` value collected by `parse_file_envars`.

Three rules govern the whole function:

- **The per-app values belong to the child's app, not to ours.**
  `PMIX_WDIR` and `PMIX_APP_ARGV` are app-level, so they are fetched with
  the appnum of `proc` — read from the datastore, defaulting to 0 the way
  `gds/hash` does when a host names none. `pmix_globals.appnum` is *my*
  appnum, which in a server is the server's; using it handed every rank in
  an MPMD job the first app's command line and working directory.
- **Only the per-app breakdown is multi-app-only.** Everything after it —
  the restart count and the `myenvars` values — applies to any job. Both
  used to sit behind an early `return` taken whenever `num_apps == 1`,
  which is nearly every job.
- **`UINT32_MAX` means "the host never told us"**, which is the state the
  tracker starts in and the state `PMIX_LOCAL_SIZE` legitimately stays in.
  A size in that state is not emitted, rather than telling the child its
  universe holds four billion procs.

### `deregister_nspace`

Finds the tracker for the nspace, removes it from `mynspaces`, and
releases it.

## Gotchas

- **`setup_fork` and `harvest_envars` re-emit `myenvars`.** MCA-file
  values pulled aside by `parse_file_envars` are injected in *both* paths;
  that is intentional (harvest feeds forwarded env; fork feeds the local
  child directly). Don't "dedupe" one away.
- **The `OMPIHOME`/home param-file logic runs only under
  `PMIX_SETUP_APP_ENVARS`.** Without that directive `harvest_envars`
  no-ops with `TAKE_NEXT_OPTION` — a request to set up a nspace is not the
  same as a request to harvest the environment.
- **`OPAL_PREFIX` implies an `LD_LIBRARY_PATH` prepend.** The code assumes
  the target install used the same libdir basename as this PMIx build. If
  you touch the prefix handling, preserve that library-path fixup or MPI
  processes may not find their libraries.
- **Version gating lives in `setup_nspace_kv` only.** `"ompiN"` is
  serviced iff `N >= 5`. If Open MPI's framework layout changes again,
  that threshold (and `ompi_frameworks_static_5_0_0[]`) is what to revisit
  — but prefer the `OMPI_MCA_PREFIXES` runtime override to editing the
  table.
- **This file legitimately `#include "include/pmix.h"`** and calls public
  `PMIx_*` helpers (`PMIx_Setenv`, `PMIx_Argv_*`, `PMIx_Value_get_number`)
  — those are the sanctioned utility wrappers, not client API entry
  points, so the "back-end never calls `PMIx_*`" rule is not violated
  here.
