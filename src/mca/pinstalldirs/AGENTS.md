<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PINSTALLDIRS Framework

This document orients AI agents and human contributors working in the
`pinstalldirs` (**P**MIx **Install Dir**ectorie**s**) framework. It
assumes you have already read the top-level
[`AGENTS.md`](../../../AGENTS.md) — the golden rules, prefix conventions,
thread-safety model, and MCA concepts described there all apply here and
are not repeated. This file covers what is specific to `pinstalldirs`:
what the framework is for, how the two components combine to produce one
resolved set of paths, the variable-expansion machinery, and the base
functions that drive it all. Each component subdirectory (`config/`,
`env/`) carries its own `AGENTS.md` with component-specific detail.

There is no `docs/how-things-work/` page for this framework; this file is
the primary reference.

## What PINSTALLDIRS does

`pinstalldirs` answers one question, very early in startup: **where is
PMIx installed?** It resolves the full GNU-standard set of installation
paths — `prefix`, `libdir`, `bindir`, `sysconfdir`, `pkgdatadir`, and the
rest — and publishes them in a single global structure,
[`pmix_pinstall_dirs`](pinstalldirs_types.h), that the rest of the
library reads directly. It also exports a string-expansion helper,
`pmix_pinstall_dirs_expand()`, that substitutes `${prefix}`-style tokens
in an arbitrary string against those resolved values (used, for example,
by the wrapper compiler `pmixcc` to expand paths in its data file).

The paths come from two sources, applied in priority order:

- **Environment overrides** (`env` component) — `PMIX_PREFIX`,
  `PMIX_LIBDIR`, etc., read from the environment at runtime, plus the
  `PMIX_PREFIX` *attribute* if one is passed to `PMIx_Init`.
- **Build-time defaults** (`config` component) — the `--prefix` and
  related values baked in by `configure`, compiled into the object file.

The environment source has higher priority, so a runtime `PMIX_*`
override wins; any field it does *not* set falls through to the
compiled-in default. This is what makes a relocated or `DESTDIR`-staged
installation work without recompilation.

`pinstalldirs` is unusual among PMIx frameworks in three ways, all of
which follow from how early and how universally it runs:

1. It is opened and initialized **before almost anything else** — in
   [`src/runtime/pmix_init.c`](../../runtime/pmix_init.c) it comes right
   after `pmix_output_init()` and *before* the help system, the keyval
   parser, and every other framework. Each user-facing tool
   (`pmix_info`, `pattrs`, `pps`, `pevent`, `plookup`, `pmixcc`) opens it
   the same way at the top of `main`. So its code cannot depend on any
   other library subsystem being up yet.
2. It performs **no MCA-parameter registration** and is **never built as
   a DSO** — the framework is declared with
   `PMIX_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER |
   PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO`. Both components are always
   statically linked in. (The MCA parameter machinery is not yet
   available this early, and the paths are needed *to find* any DSOs.)
3. It does **not select** a single winning component. Both components
   are always active; the base **merges** their contributions field by
   field. See below.

## The merge model: no selection, first-to-set wins

Most MCA frameworks pick one component (single-select) or offer a
request to each in turn (multi-select). `pinstalldirs` does neither and
ships **no `select.c`**. Instead, `pmix_pinstall_dirs_base_init()` walks
*every* opened component in the framework-components list and copies each
path field into the global `pmix_pinstall_dirs` **only if that field is
still unset** (`NULL`). The copy is the `CONDITIONAL_COPY` macro in
[`base/pinstalldirs_base_components.c`](base/pinstalldirs_base_components.c):

```c
#define CONDITIONAL_COPY(target, origin, field)             \
    do {                                                    \
        if (origin.field != NULL && target.field == NULL) { \
            target.field = origin.field;                    \
        }                                                   \
    } while (0)
```

So the **first component to set a given field wins**, and later
components fill only the gaps. The winner is decided entirely by the
order components appear in the list, which is **descending MCA
priority**:

| Component | Priority | Role |
|-----------|----------|------|
| `env`    | 10 | runtime `PMIX_*` env-var / attribute overrides |
| `config` | 0  | build-time defaults from `configure` |

`env` is first, so any path it resolves from the environment takes
precedence; `config` then supplies the compiled-in default for every
field `env` left `NULL`. Because `env` deliberately leaves a field
`NULL` when its environment variable is absent or empty (see the `env`
component doc), the common case is "no overrides present, `config`
supplies everything." The framework's `CONFIGURE_MODE` is `PRIORITY`
(from the top-level [`configure.m4`](configure.m4)), which is what causes
the generated [`base/static-components.h`](base/static-components.h) to
list `env` ahead of `config`.

There is no fallback module and no failure path for "nothing selected":
if neither component set a field it simply stays `NULL`, and the expand
step below turns a `NULL` into `NULL` harmlessly.

## Core data structures

### `pmix_pinstall_dirs_t` (`pinstalldirs_types.h`)

The resolved path set. Every field is a `char *`; the global instance
`pmix_pinstall_dirs` is the framework's real product. The fields, in
struct order:

| Field | Meaning |
|-------|---------|
| `prefix` | installation prefix (`--prefix`) |
| `exec_prefix` | prefix for architecture-dependent files |
| `bindir` | user executables |
| `sbindir` | system-admin executables |
| `libexecdir` | executables run by other programs |
| `datarootdir` | root of read-only arch-independent data |
| `datadir` | read-only arch-independent data |
| `sysconfdir` | read-only single-machine config |
| `sharedstatedir` | arch-independent modifiable data |
| `localstatedir` | single-machine modifiable data |
| `libdir` | object code libraries |
| `includedir` | C header files |
| `infodir` | GNU info files |
| `mandir` | man pages |
| `pmixdatadir` | `$(datadir)/pmix` — PMIx's own data (help files, wrapper data) |
| `pmixlibdir` | `$(libdir)/pmix` — where components live |
| `pmixincludedir` | `$(includedir)/pmix` — devel headers |

The three `pmix*` fields are PMIx's package-scoped versions of the
`pkg{data,lib,include}dir` GNU variables; the field-name comment in
`pinstalldirs_types.h` notes they intentionally match the
`configure`-set Makefile macros (e.g. `$(pmixdatadir)`). This naming
matters during expansion (below): the token spelled `${pkgdatadir}`
expands from the `pmixdatadir` field, via the `EXPAND_STRING2` alias.

`pmix_pinstall_dirs` is only valid **after** the framework has been
opened and `pmix_pinstall_dirs_base_init()` has run.

### `pmix_pinstalldirs_base_component_t` (`pinstalldirs.h`)

A component is more than an MCA header here — it carries its data inline:

```c
struct pmix_pinstalldirs_base_component_2_0_0_t {
    pmix_mca_base_component_t component;      // standard MCA header
    pmix_pinstall_dirs_t      install_dirs_data;  // this component's paths
    pmix_install_dirs_init_fn_t init;         // optional, may be NULL
};
```

- `install_dirs_data` is the component's own candidate path set, which
  the base merges into the global.
- `init` (`pmix_install_dirs_init_fn_t`,
  `void (*)(pmix_info_t info[], size_t ninfo)`) is an **optional**
  pre-merge hook. If non-`NULL`, the base calls it — passing through the
  `info`/`ninfo` array handed to `PMIx_Init` — *before* copying that
  component's `install_dirs_data`, giving the component a chance to
  populate its fields at runtime. `config` leaves `init` `NULL` (its
  data is a compile-time constant); `env` uses it to read the
  environment and the `PMIX_PREFIX` attribute.

`PMIX_PINSTALLDIRS_BASE_VERSION_1_0_0` is the version macro every
component's `component` sub-struct must open with. Note the struct is
versioned `2_0_0` in its type name while the MCA version macro is
`1_0_0`; match the existing components rather than "fixing" this.

## Directory layout

```
src/mca/pinstalldirs/
├── pinstalldirs.h                  Framework API: component struct, expand
│                                   decl, version macro
├── pinstalldirs_types.h            pmix_pinstall_dirs_t + the global
├── configure.m4                    sets CONFIGURE_MODE = PRIORITY
├── base/                           Framework infrastructure
│   ├── base.h                      base API: framework decl, expand_setup,
│   │                               base_init
│   ├── pinstalldirs_base_components.c  the global, base_init (the merge),
│   │                               open/close, framework declaration
│   └── pinstalldirs_base_expand.c  ${...}/@{...} variable substitution
├── config/                         build-time defaults (generated header)
└── env/                            runtime PMIX_* env / attribute overrides
```

Note there is **no** `*_base_frame.c` or `*_base_select.c`; the framework
declaration, open/close, and the merge all live in
`pinstalldirs_base_components.c`, and there is no selection step at all.

## Base infrastructure in detail

### The merge and lifecycle (`pinstalldirs_base_components.c`)

- **`pmix_pinstall_dirs`** — the global result, defined here and
  zero-initialized (every field `NULL`).
- **`pmix_pinstalldirs_base_open`** just calls
  `pmix_mca_base_framework_components_open()` — it opens both static
  components; there is nothing else to set up.
- **`pmix_pinstall_dirs_base_init(info, ninfo)`** — the heart of the
  framework, and a **separate call** the caller must make after opening
  (it is *not* the framework `init` hook). It:
  1. iterates the opened components in priority order;
  2. calls each component's `init(info, ninfo)` if present;
  3. runs `CONDITIONAL_COPY` for all 17 fields (first-to-set wins);
  4. rewrites every field in place through
     `pmix_pinstall_dirs_expand_setup()` (see below), which both
     expands `${...}` tokens and applies `$PMIX_DESTDIR` — and, crucially,
     `strdup`s, so afterward `pmix_pinstall_dirs` **owns** all its
     strings even though the components handed it borrowed pointers
     (string literals from `config`, `getenv`/attribute pointers from
     `env`).
  It returns `PMIX_SUCCESS`. (A code comment notes it deliberately does
  *not* close the components, to avoid deregistering variable groups.)
- **`pmix_pinstalldirs_base_close`** `free`s all 17 fields, `memset`s the
  global back to zero, and closes the components. This pairs with the
  `strdup`s done during init.
- The framework is declared with
  `PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pinstalldirs, NULL, NULL,
  open, close, static_components, NOREGISTER | NO_DSO)` — note the
  `NULL` description and `NULL` register function.

### Variable expansion (`pinstalldirs_base_expand.c`)

Two public entry points wrap one static worker
`pmix_pinstall_dirs_expand_internal(input, is_setup)`:

- **`pmix_pinstall_dirs_expand(input)`** — the public API (declared in
  `pinstalldirs.h`, `PMIX_EXPORT`). Expands `${field}` / `@{field}`
  tokens against `pmix_pinstall_dirs`; does **not** apply `PMIX_DESTDIR`.
- **`pmix_pinstall_dirs_expand_setup(input)`** — base-only (declared in
  `base/base.h`); same expansion **plus** `PMIX_DESTDIR` handling. Only
  to be used during framework setup, which is why `base_init` uses this
  variant on every field.

Mechanics worth knowing before editing:

- Both `${field}` and `@{field}` forms are recognized for every field.
  The `@{...}` spelling exists so a value can be passed through
  `AC_SUBST` without m4 mangling it. The `EXPAND_STRING`/`EXPAND_STRING2`
  macros do the `strstr` + `asprintf` splice; `EXPAND_STRING2` maps a
  differently-named token onto a field (the `pkgdatadir` → `pmixdatadir`,
  `pkglibdir` → `pmixlibdir`, `pkgincludedir` → `pmixincludedir`
  aliases).
- Expansion **loops until no token changes**, so nested references
  (e.g. `libdir` defined as `${exec_prefix}/lib`, `exec_prefix` as
  `${prefix}/...`) fully resolve.
- **`PMIX_DESTDIR`** (only in the `_setup` path): if set and non-empty,
  every expanded field is prepended with it via `pmix_os_path()`, so a
  staged install (`make install DESTDIR=/tmp/buildroot`) points at the
  staging tree during the build. The long comment in the file explains
  the three cases (bare dirs, flags with embedded `${}`, flags with
  none) it must handle correctly.
- A `#if 0` debug block at the end dumps every resolved path to stderr;
  handy when diagnosing a wrong-path problem (note it references the old
  `pkg*` field names, a stale comment).

## Threading

`pinstalldirs` has **no threading model to speak of** and no caddy
pattern. Open + `base_init` run once, synchronously, on the main thread
during `PMIx_Init` (or `main` in the tools), long before any progress
thread exists. After that, `pmix_pinstall_dirs` is treated as immutable
read-only data by the rest of the library, and
`pmix_pinstall_dirs_expand()` is a pure function of its input and that
read-only global — safe to call from anywhere. Do not mutate
`pmix_pinstall_dirs` after init.

## Building

Both components are always compiled and **statically** linked into
`libpmix` (the `NO_DSO` flag guarantees no plugin form); they are wired
through the generated
[`base/static-components.h`](base/static-components.h). Each ships a
`configure.m4` that (a) forces `static` compile mode and (b) declares its
priority (`env` = 10, `config` = 0) — the priorities are the merge order,
so changing them changes which source wins. The `config` component's
`configure.m4` additionally lists `pinstall_dirs.h` in `AC_CONFIG_FILES`
so `configure` generates that header (see the `config` component doc).

Because everything is resolved by `configure` — the priorities, the
generated header, the static-component wiring — a change to any
`configure.m4` or to `pinstall_dirs.h.in` requires the full
`./autogen.pl && ./configure ... && make`, not a bare `make`. Editing a
`Makefile.am` alone only needs `make`. The framework ships no `show_help`
text.

## When adding or modifying a component

Adding a third source of install paths is rare, but if you do:

- Open the component's `component` sub-struct with
  `PMIX_PINSTALLDIRS_BASE_VERSION_1_0_0` and set the component name.
- Fill in `install_dirs_data` for the fields you can supply and leave the
  rest `NULL` — the base only takes non-`NULL` fields, and only for
  fields no higher-priority component already set.
- Give it a `configure.m4` with a `_PRIORITY` that places it correctly in
  the merge order (higher = wins over `config`/`env`), and force
  `static` compile mode — this framework is `NO_DSO`.
- If your paths are known only at runtime, supply an `init` hook; if they
  are compile-time constants, leave `init` `NULL` and populate
  `install_dirs_data` statically.
- Do **not** add MCA parameters (the framework is `NOREGISTER`) and do
  **not** assume any other library subsystem is initialized — this code
  runs first.
