<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The MCA tree

This document orients AI agents and human contributors working anywhere
under `src/mca`. Read the top-level [`AGENTS.md`](../../AGENTS.md) first
— the golden rules, prefix conventions, thread-safety model and the MCA
vocabulary (project → framework → component → module) all come from
there and are not repeated here. This file covers what is true of the
*tree*: the two files that live at this level, the directory shape every
framework must follow, and where to look next.

Nearly every framework subdirectory carries its own `AGENTS.md`, and so
do most component subdirectories. Those are the authoritative documents
for the thing they describe; this one only gets you to them.

## What lives at this level

Only three things:

| Path | Role |
|------|------|
| [`mca.h`](mca.h) | the component and module ABI — what every component struct starts with |
| [`Makefile.include`](Makefile.include) | one line, included from `src/Makefile.am`, that installs `mca.h` |
| [`base/`](base/) | **not a framework** — the MCA implementation itself. See [`base/AGENTS.md`](base/AGENTS.md) |

Everything else is a framework directory.

### `mca.h` — the component ABI

`pmix_mca_base_component_t` (versioned as
`pmix_mca_base_component_2_1_0_t`) is the struct every component's public
symbol begins with. It carries three sets of names and versions — MCA,
project, framework, component — and four optional function pointers:

| Field | When it runs | Notes |
|-------|--------------|-------|
| `pmix_mca_register_component_params` | during framework *register* | the right place for `pmix_mca_base_component_var_register()` |
| `pmix_mca_open_component` | during framework *open* | allocate resources; **not** the place to register variables |
| `pmix_mca_query_component` | during *select* | return a module and a priority |
| `pmix_mca_close_component` | during framework *close* | release resources |

All four may be `NULL`, which the base treats as "called it, it returned
`PMIX_SUCCESS`".

**`PMIX_ERR_NOT_AVAILABLE` from register or open means "silently ignore
me".** It is how a component says "not on this machine" without
producing a user-visible error. Any *other* failure is reported (subject
to `mca_base_component_show_load_errors`) and the component is dropped.
**`PMIX_ERR_FATAL` from a query stops selection entirely** — a component
uses it to say "the user asked for something I cannot provide, do not
quietly pick someone else instead."

The struct ends with `char reserved[32]`. That is the ABI escape hatch:
new fields come out of `reserved`, never by inserting into the middle,
because host environments and dynamically-loaded components built
against an older release must keep working. The same reasoning as the
top-level backward-compatibility rules — treat this layout as
append-only.

Three name-length limits are fixed here and used to size fields all over
the tree: project 15, framework 31, component 63 (each `+1` for the
terminator).

A component struct is opened with its framework's version macro, which
expands through `PMIX_MCA_BASE_VERSION_1_0_0()` in this header:

```c
pmix_gds_base_component_t pmix_mca_gds_hash_component = {
    PMIX_GDS_BASE_VERSION_1_0_0,
    .pmix_mca_component_name = "hash",
    ...
};
```

The public symbol must be named
`<project>_mca_<framework>_<component>_component`, because that is the
string
[`base/pmix_mca_base_component_repository.c`](base/pmix_mca_base_component_repository.c)
builds and looks up with `dlsym` when the component is a DSO. Get it
wrong and the component works statically and silently disappears from a
`--enable-mca-dso` build.

## Directory shape

The layout is strict — nothing under `src/mca` may deviate:

```
src/mca/<framework>/
├── <framework>.h            the framework's public API: module struct + version macro
├── base/                    framework infrastructure, NOT a component
│   ├── base.h
│   ├── <framework>_base_frame.c     PMIX_MCA_BASE_FRAMEWORK_DECLARE
│   ├── <framework>_base_select.c
│   └── ...
└── <component>/             one directory per component
    ├── Makefile.am
    ├── configure.m4         only if the component has build prerequisites
    └── <framework>_<component>_*.c
```

`base/` is the one name that is not a component. Everything else at that
level is.

Naming follows from the prefix rule: files are
`<framework>_<component>_*.c`, framework-scoped symbols are
`pmix_<framework>_*`, component-scoped symbols are
`pmix_<framework>_<component>_*`.

## Frameworks in this tree

| Framework | Select | Purpose |
|-----------|--------|---------|
| [`bfrops`](bfrops/) | multi | pack/unpack/copy/print/compare — one component per wire-format version |
| [`gds`](gds/) | multi (assign-on-demand per peer) | the datastore behind `PMIx_Put`/`PMIx_Get` |
| [`pcompress`](pcompress/) | single | compression of large data objects |
| [`pdl`](pdl/) | single | `dlopen` abstraction — used *by* `base/` itself |
| [`pgpu`](pgpu/) | multi | GPU resource discovery |
| [`pif`](pif/) | multi | network interface discovery |
| [`pinstalldirs`](pinstalldirs/) | multi | installation directory resolution |
| [`plog`](plog/) | multi | logging of user-provided alerts |
| [`pmdl`](pmdl/) | multi | programming-model (MPI, OpenMP, …) env and parameter setup |
| [`pnet`](pnet/) | multi | network support and "instant on" endpoint computation |
| [`preg`](preg/) | multi | node/proc-map regular expression generation and parsing |
| [`psec`](psec/) | multi | connection security handshakes |
| [`psensor`](psensor/) | multi | process monitoring and health checks |
| [`pstat`](pstat/) | single | process/node/disk statistics |
| [`ptl`](ptl/) | single | client/server and tool/server transport |

Note that the top-level [`AGENTS.md`](../../AGENTS.md) also lists `prm`
and `psquash`. Neither directory exists in this tree any more; the table
above is what is actually here. Trust `ls` over either document, and
each framework's own `AGENTS.md` over this table's one-word
select column.

**`pdl` is special: it is opened by the MCA base itself**, in
`pmix_mca_base_component_repository_init()`, before any other framework
exists. It therefore cannot depend on anything the rest of the MCA
provides.

## Adding a framework or component

The full procedure is in the top-level guide; the parts specific to this
tree:

1. Create `src/mca/<framework>/<component>/` with a `Makefile.am`, and a
   `configure.m4` **only** if the component can fail to be buildable.
2. Give the component struct the exactly-correct public symbol name (see
   `mca.h` above) and open it with the framework's version macro.
3. Register MCA variables from the component's *register* function, not
   its open function.
4. **Regenerate the build system.** Adding or removing a component
   directory, or touching a `configure.m4`, changes wiring that a plain
   `make` cannot pick up:

   ```sh
   ./autogen.pl
   ./configure <same options as before>   # ./config.status --config
   make -j
   ```

   Until you do, the component is simply not built — and it will look
   like your code is being ignored.
5. Add the build products to the appropriate `.gitignore`, and check
   `git status` after a clean build.

Editing a `Makefile.am` alone needs only a plain `make`.

## Two things that bite

- **A renamed or deleted component directory wedges existing build
  trees.** Automake records each component's `configure.m4` as a
  prerequisite of `aclocal.m4`; when one disappears, `make` decides
  `aclocal.m4` is stale and shells out to `aclocal`. The error names
  `aclocal`, never the component. Delete `config.status` and re-run
  `configure`. See
  [`contrib/dockerswarm/AGENTS.md`](../../contrib/dockerswarm/AGENTS.md),
  where this is a recurring problem.
- **A component that only builds when some hardware or third-party
  library is present gets no compiler coverage on a machine that lacks
  it.** Gate its `configure.m4` with `|| test "$pmix_testbuild" = "1"`
  and guard its third-party include with `#if PMIX_TESTBUILD` against a
  shim header, so `--enable-test-build` can compile-check it. A
  test-build tree is for compiling only — never run the functional tests
  against one.

## Where to go next

- [`base/AGENTS.md`](base/AGENTS.md) — the MCA implementation: variables,
  component discovery and loading, the framework lifecycle.
- `<framework>/AGENTS.md` — one per framework, and the authoritative
  document for it.
- [`docs/developers/terminology.rst`](../../docs/developers/terminology.rst)
  — the project's own MCA vocabulary.
