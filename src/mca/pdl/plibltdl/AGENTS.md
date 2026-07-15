<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PDL `plibltdl` Component

`plibltdl` is the `pdl` component that implements dynamic loading on top
of **GNU libltdl** (Libtool's `lt_dlopen` / `lt_dlsym` / `lt_dlclose`
family) rather than the native `dlopen(3)`. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `plibltdl`. It is the **fallback** loader: it is only built when the
higher-priority [`pdlopen`](../pdlopen/AGENTS.md) component cannot be —
typically under `--disable-pmix-dlopen` or on a platform lacking native
`dlopen` but providing libltdl.

## Files

| File | Contents |
|------|----------|
| `pdl_libltdl.h` | Declares the module symbol, the private `pmix_pdl_handle_t`, and the component struct (with its `lt_dladvise` members). |
| `pdl_libltdl_component.c` | Component struct + register/open/close/query (priority **50**). |
| `pdl_libltdl_module.c` | The module: `open` / `lookup` / `close` / `foreachfile` over libltdl. |

## When it is selected

`plibltdl`'s `configure.m4` probes for libltdl
(`OAC_CHECK_PACKAGE([libltdl], ... ltdl.h, ltdl, lt_dlopen, ...)`, plus a
`--with-libltdl[-libdir]` knob and an `lt_dladvise_init` feature check).
At priority **50** it loses to `pdlopen` (80) under `STOP_AT_FIRST`, so in
a normal build it is *not* compiled at all — it only becomes the winner
when `pdlopen` is disabled or unavailable. See the framework doc: this is
a configure-time decision, not a run-time one.

> **Heads-up — this component's source is stale.** Unlike `pdlopen`, the
> `plibltdl` C sources have not been kept current with the tree's symbol
> renames and header reorganization. As checked in they reference
> pre-rename identifiers and old include paths — for example
> `#include "pmix/mca/dl/dl.h"` and `#include "pmix/constants.h"` (the
> current layout is `src/mca/pdl/pdl.h`), `PMIX_DL_BASE_VERSION_1_0_0`
> (now `PMIX_PDL_BASE_VERSION_1_0_0`), the un-prefixed `mca_base_module_t`
> / `mca_base_component_var_register` / `MCA_BASE_MAKE_VERSION` /
> `.mca_component_name` MCA symbols (now `pmix_mca_*` / `.pmix_mca_*`),
> the `mca_pdl_plibltpdl_component` global, and the macro
> `PMIX_DL_LIBLTDL_HAVE_LT_DLADVISE` (its `configure.m4` actually defines
> `PMIX_PDL_PLIBLTDL_HAVE_LT_DLADVISE`). This is why `pdlopen` is
> effectively always the component that builds: `plibltdl` would need
> these references updated before it could compile against the current
> tree. If you are tasked with reviving libltdl support, budget for a
> symbol/include modernization pass, and use `pdlopen` as the reference
> for the current MCA conventions. The description below reflects the
> component's *intended* design.

## The private handle

`plibltdl` defines the opaque `pmix_pdl_handle_t` as:

```c
struct pmix_pdl_handle_t {
    lt_dlhandle ltpdl_handle;   // the libltdl handle
#if PMIX_ENABLE_DEBUG
    char *filename;             // strdup of the opened name, debug builds only
#endif
};
```

## The component and lt_dladvise

The component struct carries up to four `lt_dladvise` objects — one for
each combination of `{private, public}` namespace × `{ext, noext}` suffix
handling — but **only when the linked libltdl provides `lt_dladvise`**
(the `PMIX_..._HAVE_LT_DLADVISE` config test). `component_open`
`lt_dlinit()`s the loader and, if advise is available, initializes the
four advise objects (setting `lt_dladvise_global` / `lt_dladvise_ext` as
appropriate); `component_close` destroys them and `lt_dlexit()`s. The
`register` function exports a single read-only info parameter,
**`pdl_plibltdl_have_lt_dladvise`**, reporting whether that support was
compiled in.

## The module

```c
pmix_pdl_base_module_t pmix_pdl_plibltpdl_module = {
    .open = plibltpdl_open,
    .lookup = plibltpdl_lookup,
    .close = plibltpdl_close,
    .foreachfile = plibltpdl_foreachfile
};
```

- **`plibltpdl_open`** — when `lt_dladvise` is available, selects the
  matching advise object from the `use_ext` × `private_namespace` matrix
  and calls `lt_dlopenadvise(fname, advise)`. Otherwise it falls back to
  `lt_dlopenext(fname)` (when `use_ext`) or plain `lt_dlopen(fname)`. On
  success it `calloc`s the handle; on failure it returns `PMIX_ERROR`
  with `*err_msg` set to a **`strdup` of `lt_dlerror()`**.
- **`plibltpdl_lookup`** — `lt_dlsym`; `PMIX_SUCCESS` if non-NULL, else
  `PMIX_ERROR` with a `strdup`'d `lt_dlerror()`.
- **`plibltpdl_close`** — `lt_dlclose`, frees the debug filename and the
  handle, returns `lt_dlclose`'s value.
- **`plibltpdl_foreachfile`** — a thin wrapper over libltdl's own
  `lt_dlforeachfile(search_path, func, data)`; returns `PMIX_SUCCESS` if
  libltdl returned 0, else `PMIX_ERROR`. (Contrast `pdlopen`, which
  hand-rolls the directory walk and basename de-duplication; here libltdl
  does that work.)

## Gotchas

- **Its `err_msg` strings are heap-allocated (`strdup`), unlike
  `pdlopen`'s.** This is a genuine behavioral difference between the two
  components: `pdlopen` hands back a pointer into the static `dlerror`
  buffer, whereas `plibltpdl` `strdup`s `lt_dlerror()`. The framework
  contract says callers must not free `err_msg`, so these `strdup`ed
  strings are, strictly, leaked on the error path — preserve the
  framework's ownership contract if you rework this, and prefer matching
  `pdlopen`'s non-allocating behavior.
- **The `lt_dladvise`-absent fallback loses the private/public
  distinction.** With old libltdl, `plibltpdl_open` uses
  `lt_dlopenext`/`lt_dlopen`, which cannot express `RTLD_LOCAL` vs
  `RTLD_GLOBAL`; the `private_namespace` argument is effectively ignored
  in that path.
- **See the staleness note above before touching this file.** Any real
  change here almost certainly needs the symbol/include modernization
  first, or it will not compile.
