<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSEC `munge` Component

`munge` authenticates a connection using [MUNGE](https://dun.github.io/munge/),
a widely-deployed HPC credential service. A local `munged` daemon issues a
credential that encodes the caller's `uid`/`gid` and can be decoded and
verified by any process on a host sharing the same MUNGE key. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `munge`. It uses the **single-shot credential** model
(`create_cred` + `validate_cred`).

## Files

| File | Contents |
|------|----------|
| `configure.m4` | `OAC_CHECK_PACKAGE` gate — builds the component only if `munge.h` / `libmunge` are found. |
| `psec_munge.h` | Declares the component and module symbols. |
| `psec_munge_component.c` | Component struct + `component_query` (priority **80**). |
| `psec_munge.c` | The module: `init`/`finalize` + `create_cred` / `validate_cred`, wrapping `libmunge`. |

## When it is selected

Two gates must both pass:

1. **Build-time.** [`configure.m4`](configure.m4) runs
   `OAC_CHECK_PACKAGE([munge], …, [munge.h], [munge], [munge_encode], …)`.
   The component is compiled only if MUNGE is present; if `--with-munge`
   was given and MUNGE is *not* found, configure errors out rather than
   silently skipping. So on a host without MUNGE, `munge` is simply not in
   the tree's `static-components.h`.
2. **Run-time.** Even when built, `munge_init` calls `munge_encode` to
   obtain a credential as a liveness check on the local `munged` daemon.
   If that fails it returns `PMIX_ERR_SERVER_NOT_AVAIL`, and
   `pmix_psec_base_select` drops the module from the actives list. So a
   built-but-idle MUNGE deployment self-disqualifies.

When both pass, `component_query` reports **priority 80** — above `native`
(10), so in a MUNGE-enabled cluster `munge` becomes the default mechanism
a peer gets from `assign_module(NULL)`.

## The module

```c
pmix_psec_module_t pmix_munge_module = {
    .name = "munge",
    .init = munge_init,
    .finalize = munge_finalize,
    .create_cred = create_cred,
    .validate_cred = validate_cred
};
```

`munge_init` caches the first credential in the file-static `mycred`;
`munge_finalize` frees it at framework close. `pmix_psec_close` invokes
each active module's `finalize`, so `mycred` is reclaimed at teardown.
(`munge_finalize` guards on `initialized`/`mycred != NULL` and nulls
`mycred` after the free, so it is idempotent.)

## What the functions do

- **`create_cred`** honors an optional `PMIX_CRED_TYPE` directive
  (declining with `PMIX_ERR_NOT_SUPPORTED` if `"munge"` was not among the
  requested types), then returns a MUNGE credential. It copies the cached
  `mycred` on first use; on every subsequent call it re-encodes a fresh
  one, because **MUNGE credentials are single-use** — a decoded
  credential cannot be presented again. The credential bytes are the
  NUL-terminated MUNGE string (`strlen + 1`). It fills `*info` with a
  `PMIX_CRED_TYPE = "munge"` marker.
- **`validate_cred`** applies the same `PMIX_CRED_TYPE` filtering, then
  calls `munge_decode` to recover the sender's `uid`/`gid` from the
  credential (a failed decode → `PMIX_ERR_INVALID_CRED`). It compares them
  against `pr->info->uid` / `pr->info->gid` and rejects a mismatch. On
  success it returns `*info` with `PMIX_CRED_TYPE = "munge"`, the
  validated `PMIX_USERID`, and `PMIX_GRPID`.

## Gotchas

- **Credentials are not reusable.** The `refresh` flag in `create_cred`
  exists so the *second and later* calls re-encode instead of replaying
  the cached credential. Do not "optimize" by caching one credential and
  handing it out repeatedly — MUNGE will reject the replay.
- **`munge_init` is a real dependency probe, not a formality.** It talks
  to `munged`. Returning success from it when the daemon is down would put
  a dead module in the actives list and break connections that select it.
- **The `#if PMIX_TESTBUILD` stub block at the top of `psec_munge.c` is
  compile-test scaffolding only.** It provides fake `munge_encode`/
  `munge_decode`/`munge_strerror` so the file can be syntax-checked
  without libmunge; the real build takes the `#else` branch
  (`#include <munge.h>`). The `PMIX_TESTBUILD` macro is 0 by default and
  is set to 1 only by `--enable-test-build` (see the top-level AGENTS.md),
  so the stub is never active in a normal build. Do not hard-code the
  guard to `#if 1` in committed code. The stubbed `munge_encode`
  deliberately returns a **failure** (never `EMUNGE_SUCCESS`) so that
  `munge_init`'s liveness probe fails and the psec framework de-selects
  MUNGE at runtime: a `--enable-test-build` library therefore falls back
  to `native` rather than selecting a non-functional MUNGE module and
  crashing in `create_cred` on the credential it never produced.
- **libmunge is LGPL** (noted in the source header). Keep MUNGE behind its
  `configure.m4` gate so the dependency stays optional.
