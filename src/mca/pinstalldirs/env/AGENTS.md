<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PINSTALLDIRS `env` Component

`env` is the `pinstalldirs` component that supplies **runtime overrides**
to the installation paths, read from `PMIX_*` environment variables (and
one `PMIx_Init` attribute) each time the library starts. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `env`. It is the highest-priority source (priority **10**),
so any path it resolves wins over the compiled-in `config` default —
this is the mechanism that lets a relocated installation be pointed at a
new prefix without recompiling.

## Files

| File | Contents |
|------|----------|
| `pmix_pinstalldirs_env.c` | The component struct plus its `init` hook, which reads the environment into `install_dirs_data`. |
| `configure.m4` | Priority (**10**), static compile mode, `AC_CONFIG_FILES` for the component `Makefile`. |
| `Makefile.am` | Builds `libpmix_mca_pinstalldirs_env.la`. |

## What its `init` hook does

Unlike `config`, `env` has no data at compile time — every field of its
`install_dirs_data` starts `NULL`. The work happens in its `init` hook,
`pinstalldirs_env_init(info, ninfo)`, which the base calls (passing the
`PMIx_Init` info array through) *before* merging this component's fields.
It fills each field from the matching environment variable:

| Field | Environment variable |
|-------|----------------------|
| `prefix` | `PMIX_PREFIX` (or the `PMIX_PREFIX` **attribute**, see below) |
| `exec_prefix` | `PMIX_EXEC_PREFIX` |
| `bindir` | `PMIX_BINDIR` |
| `sbindir` | `PMIX_SBINDIR` |
| `libexecdir` | `PMIX_LIBEXECDIR` |
| `datarootdir` | `PMIX_DATAROOTDIR` |
| `datadir` | `PMIX_DATADIR` |
| `sysconfdir` | `PMIX_SYSCONFDIR` |
| `sharedstatedir` | `PMIX_SHAREDSTATEDIR` |
| `localstatedir` | `PMIX_LOCALSTATEDIR` |
| `libdir` | `PMIX_LIBDIR` |
| `includedir` | `PMIX_INCLUDEDIR` |
| `infodir` | `PMIX_INFODIR` |
| `mandir` | `PMIX_MANDIR` |
| `pmixdatadir` | `PMIX_PKGDATADIR` |
| `pmixlibdir` | `PMIX_PKGLIBDIR` |
| `pmixincludedir` | `PMIX_PKGINCLUDEDIR` |

Each assignment goes through the `SET_FIELD` macro, whose one important
behavior is: **an unset or empty (`strlen == 0`) variable leaves the
field `NULL`.** That is deliberate — a `NULL` field is skipped by the
base's `CONDITIONAL_COPY`, so it falls through to the `config` default.
`env` only overrides a path when the corresponding variable is actually
present and non-empty.

### The `PMIX_PREFIX` attribute special case

`prefix` is the one field that can also come from the `info` array handed
to `PMIx_Init`. Before consulting the environment, `init` scans
`info[0..ninfo)` for the `PMIX_PREFIX` key
(`PMIX_CHECK_KEY(&info[n], PMIX_PREFIX)`); if found, it takes
`info[n].value.data.string` as the prefix and skips the `PMIX_PREFIX`
environment lookup. All other fields still come only from the
environment. This lets an embedding host set the install prefix
programmatically at init time.

## The module struct it fills in

`env` provides a `pmix_pinstalldirs_base_component_t` named
`pmix_mca_pinstalldirs_env_component`, opened with
`PMIX_PINSTALLDIRS_BASE_VERSION_1_0_0`. Two things distinguish it from
`config`:

- It is **not `const`** — `init` writes into its own `install_dirs_data`
  at runtime, so the struct must be mutable.
- It sets `.init = pinstalldirs_env_init`; `config` has no `init`.

Component open/close functions are `NULL`; the struct closes with
`PMIX_MCA_BASE_COMPONENT_INIT(pmix, pinstalldirs, env)`.

## When it is selected

Always active (statically linked, `NO_DSO`, no selection). Its priority
of **10** places it first in the base merge, ahead of `config` (0), so
its non-`NULL` fields win. In a typical run with no `PMIX_*` variables
set it contributes nothing and `config` supplies every path.

## Gotchas

- **Borrowed pointers.** The field values are raw `getenv()` return
  pointers (into `environ`) or the attribute's `value.data.string` — this
  component does **not** `strdup`. That is safe only because the base
  immediately copies each field through
  `pmix_pinstall_dirs_expand_setup()` (which `strdup`s) during
  `base_init`. Do not stash these pointers anywhere with a longer
  lifetime, and do not have the base free this component's
  `install_dirs_data`.
- **Empty means unset.** Setting `PMIX_LIBDIR=` (empty) does *not* force
  an empty libdir; it is treated as "not provided" and the `config`
  default is used. This is intentional.
- Overriding only some paths is fine and common — e.g. exporting just
  `PMIX_PREFIX` after relocating the tree. The unspecified fields still
  come from `config`, so they may not reflect the new prefix unless they
  were expressed relative to `${prefix}` at build time. If a partial
  override yields inconsistent paths, that interaction (env prefix +
  compiled-in absolute subdir) is the usual cause.
