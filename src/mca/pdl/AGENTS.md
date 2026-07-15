<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PDL Framework

This document orients AI agents and human contributors working in the
`pdl` (**P**MIx **D**ynamic **L**oader) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety model, and MCA concepts
described there all apply here and are not repeated. This file covers
what is specific to `pdl`: what the framework is for, the unusual way its
one component is chosen (at *build* time, not run time), the small module
interface every component fills in, the base wrapper layer, and the
gotchas. Each component subdirectory (`pdlopen/`, `plibltdl/`) carries
its own `AGENTS.md`.

There is no `docs/how-things-work/` page for `pdl`; the header comment in
[`pdl.h`](pdl.h) is the closest thing to a design narrative and is worth
reading in full.

## What PDL does

`pdl` is PMIx's portable abstraction over the platform's dynamic-loading
primitives — `dlopen(3)` / `dlsym(3)` / `dlclose(3)` and their
equivalents — "very similar to Libtool's libltdl" (per `pdl.h`). It
exists so the rest of the library can load a DSO, look up a symbol in it,
close it, and walk a directory of loadable files without embedding any
platform `#ifdef`s of its own.

Its one and only consumer is the **MCA component repository**
([`src/mca/base/pmix_mca_base_component_repository.c`](../../base/pmix_mca_base_component_repository.c)):
that is the code that loads *other* frameworks' components when they are
built as DSOs rather than linked statically. It calls
`pmix_pdl_foreachfile` to discover component libraries on the search
path, `pmix_pdl_open` to load one, `pmix_pdl_lookup` to find its
`pmix_mca_<framework>_<component>_component` struct, and `pmix_pdl_close`
to unload it. In other words: `pdl` is the bottom turtle of the plugin
system — the framework that makes all the *other* dynamic frameworks
loadable. That is why it is opened before any DSO-based framework and is
itself marked **`NO_DSO`** (it can never be a DSO, or nothing could load
it).

If PMIx is configured `--disable-dlopen` (a fully static build where
every component is linked in and nothing is ever loaded at run time),
`pdl` builds **no** component at all and every wrapper returns
`PMIX_ERR_NOT_SUPPORTED` — which is fine, because in that configuration
nobody asks it to load anything.

## The single most important structural fact

`pdl` is a **single-select** framework, but not the run-time kind.
Selection happens at **configure/build time**. Its
[`configure.m4`](configure.m4) sets

```m4
m4_define(MCA_pmix_pdl_CONFIGURE_MODE, STOP_AT_FIRST)
```

so `configure` walks the components in descending priority, keeps the
**first** one that can compile on this platform, and *does not even
build the losers*. The header comment in `pdl.h` states the consequence
directly:

> This is a compile-time framework: a single component will be selected
> by the priority that its configure.m4 provides. All other components
> will be ignored (i.e., not built/not part of the installation).
> Meaning: the static_components of the pdl framework will always contain
> 0 or 1 components.

You can confirm this in the generated
[`base/static-components.h`](base/static-components.h): in a normal build
it lists exactly one entry (`pdlopen`). The run-time
`pmix_pdl_base_select()` therefore never arbitrates between candidates —
there is at most one, and it just installs it. **Component priority in
`pdl` is resolved by `configure`, not by the select function.**

Practical corollaries:

- To force a particular component you influence *configure*, not a
  run-time MCA parameter: `--disable-pmix-dlopen` makes `pdlopen` fail
  its configure test, letting `plibltdl` win instead;
  `--with-libltdl=DIR` points the libltdl probe at a specific install.
- Because only one component is ever compiled in, all of `pdl` is
  effectively "the base wrappers plus whichever single module was
  chosen." There is no multi-component interplay to reason about.

## Module interface (`pmix_pdl_base_module_t`)

Defined in [`pdl.h`](pdl.h) as `pmix_pdl_base_module_1_0_0_t`. A component
fills in all four function pointers (both current components do; none is
left `NULL`).

| Field | Signature | Purpose |
|-------|-----------|---------|
| `super` | `pmix_mca_base_module_2_0_0_t` | standard MCA module header |
| `open` | `(const char *fname, bool use_ext, bool private_namespace, pmix_pdl_handle_t **handle, char **err_msg)` | load a DSO; allocate and return a handle |
| `close` | `(pmix_pdl_handle_t *handle)` | unload the DSO and free the handle |
| `lookup` | `(pmix_pdl_handle_t *handle, const char *symbol, void **ptr, char **err_msg)` | resolve `symbol` in an open DSO |
| `foreachfile` | `(const char *search_path, int (*cb)(const char *filename, void *ctx), void *ctx)` | walk a `PMIX_ENV_SEP`-delimited path, invoking `cb` once per unique loadable basename |

Semantics that matter (all documented in `pdl.h`):

- **`open`** — `fname == NULL` means "open this process itself" (the main
  program's symbol table). `use_ext == true` asks the module to try the
  platform's library suffixes (`.so`, `.dylib`, `.dll`, `.sl`) rather
  than the bare name. `private_namespace == true` loads into a private
  (local) namespace, otherwise a global one. **The module allocates the
  handle**; the caller frees it only by calling `close`.
- **`foreachfile`** — deliberately collapses a Libtool `.la` file and its
  underlying `.so` to a single callback keyed on the shared basename
  (e.g. `foo.la` and `foo.so` both yield `foo` exactly once). This is
  what lets the component repository enumerate installable components
  without tripping over Libtool wrapper files.
- **`err_msg`** — on failure a module *may* point this at a human-readable
  string. That string is owned by the module/loader (it can be the
  `dlerror()` static buffer); **the caller must not free or alter it**,
  and its contents may change on the next `pdl` call.

The `pmix_pdl_handle_t` type is **opaque** in `pdl.h` (only a forward
declaration). Each component defines the real `struct pmix_pdl_handle_t`
privately in its own header, wrapping whatever the underlying loader
returns (a `void *` from `dlopen`, an `lt_dlhandle` from libltdl).

The selected module is stored in the exported global **`pmix_pdl`**
(declared in [`base/base.h`](base/base.h), defined in
`base/pdl_base_open.c`); back-end code never touches `pmix_pdl` directly
— it calls through the base wrappers below.

## The base wrapper layer (`base/pdl_base_fns.c`)

Unlike most frameworks, back-end callers do **not** invoke
`pmix_pdl->open(...)` directly. They call four thin public wrappers, each
of which null-checks both `pmix_pdl` and the specific function pointer
and returns `PMIX_ERR_NOT_SUPPORTED` if either is absent (the
`--disable-dlopen` / no-component case):

| Wrapper | Delegates to | On no component |
|---------|--------------|-----------------|
| `pmix_pdl_open(fname, use_ext, priv, &handle, &err_msg)` | `pmix_pdl->open` | sets `*handle = NULL`, returns `PMIX_ERR_NOT_SUPPORTED` |
| `pmix_pdl_lookup(handle, symbol, &ptr, &err_msg)` | `pmix_pdl->lookup` | `PMIX_ERR_NOT_SUPPORTED` |
| `pmix_pdl_close(handle)` | `pmix_pdl->close` | `PMIX_ERR_NOT_SUPPORTED` |
| `pmix_pdl_foreachfile(path, cb, ctx)` | `pmix_pdl->foreachfile` | `PMIX_ERR_NOT_SUPPORTED` |

This wrapper indirection is why a `--disable-dlopen` build "just works":
the component repository calls the same four functions, they all cleanly
report unsupported, and the repository falls back to its statically
linked components.

## Selection and lifecycle (`base/`)

Note there is **no `pdl_base_frame.c`** here; the framework declaration
and globals live in `pdl_base_open.c`, and the base is split across four
small files:

- **`base/pdl_base_open.c`** defines the globals (`pmix_pdl`,
  `pmix_pdl_base_selected_component`), the open function
  `pmix_pdl_base_open` (which just calls
  `pmix_mca_base_framework_components_open`), and declares the framework
  itself:
  ```c
  PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pdl, "Dynamic loader framework",
      NULL /* register */, pmix_pdl_base_open /* open */, NULL /* close */,
      pmix_mca_pdl_base_static_components,
      PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO);
  ```
  The **`NO_DSO`** flag is essential: it tells the MCA base never to try
  to *dynamically load* a `pdl` component (which would be circular). The
  comment in the file explains that the file also exists just so the
  linker keeps this `.o` (the "cough cough OS X" note).
- **`base/pdl_base_select.c`** (`pmix_pdl_base_select`) calls the standard
  `pmix_mca_base_select("pdl", ...)`, stores the winner in
  `pmix_pdl_base_selected_component` and `pmix_pdl`, and returns
  `PMIX_ERROR` if nothing was selected. Because at most one static
  component exists, this is a formality that installs it (or leaves
  `pmix_pdl == NULL`).
- **`base/pdl_base_close.c`** (`pmix_pdl_base_close`) closes the
  framework's components.
- **`base/pdl_base_fns.c`** — the four public wrappers described above.

The framework has **no register function and no framework-level MCA
parameters** (the `NULL` in the declare). The only MCA parameters in
`pdl` are component-level (see below).

### Where it is driven from

`pdl` is unusual in *who* opens and selects it. It is not opened during
the normal framework-open sweep in library init; instead the component
repository opens and selects it lazily the first time a DSO must be
loaded, in
[`pmix_mca_base_component_repository.c`](../../base/pmix_mca_base_component_repository.c)
(`pmix_mca_base_framework_open(&pmix_pdl_base_framework, ...)` then
`pmix_pdl_base_select()`), and closes it again when the repository is
finalized. The framework is registered in the master list in
[`src/include/pmix_frameworks.c`](../../../src/include/pmix_frameworks.c).

## Component selection criteria

Priorities are set in each component's `configure.m4`
(`MCA_pmix_pdl_<name>_PRIORITY`) and applied by `STOP_AT_FIRST` at
configure time:

| Component | Priority | Built (wins) when… |
|-----------|----------|--------------------|
| `pdlopen` | 80 | `dlopen`/`dlfcn.h`/`-ldl` are available **and** `--disable-pmix-dlopen` was not given (the normal case on Linux, macOS, and most POSIX systems) |
| `plibltdl` | 50 | GNU libltdl (`ltdl.h` + `-lltdl`) is available **and** `pdlopen` did not win — e.g. under `--disable-pmix-dlopen`, or where native `dlopen` is absent but libltdl is present |

If neither can compile and `--disable-dlopen` was not specified,
`configure` **errors out** (see `configure.m4`: "Did not find a suitable
static pmix pdl component"), because dynamic components were requested
but nothing can load them. Under `--disable-dlopen`, `want_pdl=0` forces
all components to fail deliberately and `PMIX_HAVE_PDL_SUPPORT` is defined
to `0`.

## MCA parameters

There are no framework-level parameters. Each component registers its
own:

| Parameter | Component | Meaning |
|-----------|-----------|---------|
| `pdl_pdlopen_filename_suffixes` | `pdlopen` | comma-delimited suffix list to try when `use_ext` is set; default `.so,.dylib,.dll,.sl` |
| `pdl_plibltdl_have_lt_dladvise` | `plibltdl` | read-only/constant info parameter reporting whether the linked libltdl provides `lt_dladvise` |

## Threading

The `pdl` functions are simple synchronous wrappers around the platform
loader; they do not thread-shift, block on a lock, or touch shared
library state, and there is no caddy pattern here. They run in the
context of whoever is loading a component (the MCA component repository),
which happens during framework open/close — not on a steady-state hot
path. Dynamic loading is inherently process-global (the dynamic linker's
own state), so callers serialize component loading themselves; `pdl` adds
no locking of its own.

## Directory layout

```
src/mca/pdl/
├── pdl.h                    Framework API: handle type, module & component structs, version macro
├── base/
│   ├── base.h               Internal base API + the pmix_pdl / selected-component globals
│   ├── pdl_base_open.c      framework DECLARE (NO_DSO), globals, open
│   ├── pdl_base_select.c    installs the single configure-time winner into pmix_pdl
│   ├── pdl_base_close.c     framework close
│   ├── pdl_base_fns.c       the four public wrappers (open/lookup/close/foreachfile)
│   └── static-components.h  generated: the 0-or-1 statically-built component
├── pdlopen/                 native dlopen(3) component (priority 80, the usual winner)
└── plibltdl/                GNU libltdl component (priority 50, fallback)
```

## Building

`pdl` ships a framework `configure.m4` (`STOP_AT_FIRST`, the
`--disable-dlopen` wiring, and the `PMIX_HAVE_PDL_SUPPORT` define), and
**each component ships its own `configure.m4`** that runs the actual
availability probe (`OAC_CHECK_PACKAGE([dlopen], ...)` for `pdlopen`,
`OAC_CHECK_PACKAGE([libltdl], ...)` plus an `lt_dladvise` check for
`plibltdl`). Both components force **static-only** compilation
(`MCA_pmix_pdl_<name>_COMPILE_MODE` → `static`); a `pdl` component can
never be a DSO. `pdl` ships **no `show_help` files**, so the
regenerate-the-help-content golden rule does not apply here.

Because component selection and the `PMIX_HAVE_*` defines are decided by
`configure`, any change to a `configure.m4`, or adding/removing a
component directory, requires the full
`./autogen.pl && ./configure … && make` — a plain `make` cannot re-run
the availability probes. Editing only a `Makefile.am` needs just `make`.

## When working in this framework

- **Remember selection is a build-time decision.** Do not add run-time
  "try `pdlopen`, fall back to `plibltdl`" logic — only one of them is
  ever compiled in. If you need to change which loader is used, that is a
  `configure.m4` priority/probe question.
- **Never let a `pdl` component become loadable as a DSO.** The `NO_DSO`
  framework flag and the per-component static-only `COMPILE_MODE` are
  load-bearing: `pdl` is what loads DSOs, so it must itself be linked in.
- **Honor the handle-ownership contract.** The module allocates the
  handle in `open` and frees it in `close`; callers must pair every
  successful `open` with a `close` and must not free the handle
  themselves.
- **Do not free or cache `err_msg`.** It may be a pointer into a static
  loader buffer (`dlerror()`) that the next call overwrites.
- **Keep `foreachfile`'s dedup behavior.** The component repository
  relies on `.la`/`.so`/`.lo`/`.o` being filtered so each component is
  offered exactly once by basename; changing that would make it try to
  load Libtool wrappers or object files.
- **A `--disable-dlopen` build has no component.** New code that calls a
  `pmix_pdl_*` wrapper must handle `PMIX_ERR_NOT_SUPPORTED` gracefully
  rather than assuming a loader is present.
