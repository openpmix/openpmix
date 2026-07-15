<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PDL `pdlopen` Component

`pdlopen` is the `pdl` component that implements dynamic loading directly
on the POSIX `dlopen(3)` family — `dlopen` / `dlsym` / `dlclose` /
`dlerror`. Read the framework [`AGENTS.md`](../AGENTS.md) first; this file
covers only what is specific to `pdlopen`. It is the default loader on
Linux, macOS, and essentially every other POSIX platform, and is the
component you are almost certainly looking at in any normal build.

## Files

| File | Contents |
|------|----------|
| `pdl_pdlopen.h` | Declares the module symbol, the private `pmix_pdl_handle_t`, and the component struct type. |
| `pdl_pdlopen_component.c` | Component struct + register/open/close/query (priority **80**). |
| `pdl_pdlopen_module.c` | The module: `open` / `lookup` / `close` / `foreachfile` over `dlopen(3)`. |

## When it is selected

`pdlopen` wins whenever its `configure.m4` probe
(`OAC_CHECK_PACKAGE([dlopen], ... dlfcn.h, dl, dlopen, ...)`) succeeds and
`--disable-pmix-dlopen` was not passed. At priority **80** it outranks
`plibltdl` (50) under `STOP_AT_FIRST`, so on any platform with a native
`dlopen` it is the only `pdl` component built. Its `component_query`
returns the module and the priority, but — as the framework doc explains
— that priority is decided at configure time, not here.

## The private handle

`pdlopen` defines the opaque `pmix_pdl_handle_t` as:

```c
struct pmix_pdl_handle_t {
    void *dlopen_handle;      // the void* returned by dlopen
#if PMIX_ENABLE_DEBUG
    void *filename;           // strdup of the opened name, debug builds only
#endif
};
```

The `filename` copy exists purely for debugging and is compiled out of
release builds.

## The module

```c
pmix_pdl_base_module_t pmix_pdl_pdlopen_module = {
    .open = pdlopen_open,
    .lookup = pdlopen_lookup,
    .close = pdlopen_close,
    .foreachfile = pdlopen_foreachfile
};
```

All four `pdl` entry points are implemented.

- **`pdlopen_open`** builds the `dlopen` flags — always `RTLD_LAZY`, plus
  `RTLD_LOCAL` when `private_namespace` is set or `RTLD_GLOBAL`
  otherwise. If `use_ext` is true and a non-NULL `fname` was given, it
  loops over the component's `filename_suffixes` list, `asprintf`s
  `fname+suffix`, `stat`s each candidate, and `dlopen`s the first that
  exists (breaking after the first attempt). If `use_ext` is false it
  `dlopen`s `fname` verbatim (and `fname == NULL` opens the running
  process). On success it `calloc`s a `pmix_pdl_handle_t`, stores the
  `dlopen` handle, and returns `PMIX_SUCCESS`; on failure `PMIX_ERROR`
  with `*err_msg` pointing at `dlerror()` (or an "File … not found"
  string for a missing suffixed candidate).
- **`pdlopen_lookup`** is a thin `dlsym`: `PMIX_SUCCESS` if the symbol
  resolves non-NULL, else `PMIX_ERROR` with `*err_msg` from `dlerror()`.
- **`pdlopen_close`** `dlclose`s the handle, frees the debug filename and
  the handle struct, and returns `dlclose`'s own return value.
- **`pdlopen_foreachfile`** splits `search_path` on `PMIX_ENV_SEP`,
  `opendir`s each directory, and `stat`s every entry. It skips
  non-regular files and Libtool/object artifacts (`.la`, `.lo`, `.o`),
  strips the remaining suffix, and **de-duplicates by basename** so a
  `foo.so`/`foo.la` pair yields a single `foo`. It collects the unique
  basenames into an argv, then invokes the caller's callback on each,
  stopping early if a callback returns non-`PMIX_SUCCESS`.

## MCA parameter

`pdlopen_component_register` registers one string parameter,
**`pdl_pdlopen_filename_suffixes`** (default `".so,.dylib,.dll,.sl"`),
splitting it into the `filename_suffixes` argv that `pdlopen_open` walks
when `use_ext` is set. The argv is freed in the component's `close`.

## Gotchas

- **`use_ext` only tries suffixes for a non-NULL `fname`.** With
  `fname == NULL` (open-this-process) or `use_ext == false`, the suffix
  list is ignored and the bare argument is passed straight to `dlopen`.
- **`err_msg` from `dlerror()` is not yours to free.** The lookup/open
  error strings point into libc's static `dlerror` buffer; they are
  overwritten by the next dynamic-loader call. (Contrast `plibltdl`,
  which `strdup`s its error strings — a real behavioral difference
  between the two components.)
- **`open` returns `PMIX_ERROR`, not a `PMIX_ERR_*` variant, on a failed
  `dlopen`.** Callers that switch on specific error codes should treat
  any non-`PMIX_SUCCESS` as "could not load."
- **`foreachfile` bails on the first `stat`/`opendir` failure** with
  `PMIX_ERR_IN_ERRNO`; it is not tolerant of an unreadable directory in
  the middle of the path. Keep that in mind if you extend the search
  logic.
