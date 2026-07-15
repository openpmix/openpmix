<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSEC `none` Component

`none` is the no-op security module: it creates an empty credential and
validates *any* credential as good. It exists for environments that want
PMIx to skip connection authentication entirely (for example, a fully
trusted, isolated deployment). Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `none`. It uses the
**single-shot credential** model, but both halves are deliberately inert.

## Files

| File | Contents |
|------|----------|
| `psec_none.h` | Declares the component and module symbols. |
| `psec_none_component.c` | Component struct + `component_query` (priority **0**) + the opt-in gate in `component_open`. |
| `psec_none.c` | The module: `init`/`finalize` + no-op `create_cred` / `validate_cred`. |

## When it is selected

`none` is compiled in unconditionally (no `configure.m4`), but it
**disqualifies itself unless explicitly requested**. Its `component_open`
looks up the `psec` MCA value and returns `PMIX_SUCCESS` only if that
value contains the substring `"none"`:

```c
/* we only allow ourselves to be considered IF the user
 * specifically requested so */
```

If the value is unset or does not mention `none`, `component_open`
returns `PMIX_ERROR` and the component is never opened, so it never
appears in the `actives` list. When it *is* requested, its
`component_query` returns the module at **priority 0** — the lowest, so it
only ever wins when the peer names it directly.

This gate is the safety interlock: you cannot accidentally end up with no
authentication. Turning it off is an explicit act
(`PMIX_MCA_psec=none` / `PMIX_SECURITY_MODE=none`).

## The module

```c
pmix_psec_module_t pmix_none_module = {
    .name = "none",
    .init = none_init,
    .finalize = none_finalize,
    .create_cred = create_cred,
    .validate_cred = validate_cred
};
```

## What the functions do

- **`create_cred`** constructs an empty `pmix_byte_object_t` and returns
  `PMIX_SUCCESS`. It ignores the peer, directives, and info entirely
  (`PMIX_HIDE_UNUSED_PARAMS`).
- **`validate_cred`** "always reports valid." It performs no credential
  check at all. Its only real logic is the `PMIX_CRED_TYPE` directive
  handling shared by all the modules: if the caller named specific
  mechanisms and `"none"` is not among them, it returns
  `PMIX_ERR_NOT_SUPPORTED`; otherwise it fills `*info` with a single
  `PMIX_CRED_TYPE = "none"` entry and returns `PMIX_SUCCESS`.

## Gotchas

- **This module intentionally provides no security.** Its `validate_cred`
  accepts anything. That is the entire point, but it means a
  configuration that selects `none` has opted out of authentication —
  never make `none` easier to reach than the explicit opt-in gate already
  allows.
- **Priority 0 plus the opt-in gate are two independent guards.** Even if
  `none` is in the actives list, its rock-bottom priority keeps
  `assign_module(NULL)` from ever returning it; a peer must ask for it by
  name. Do not raise its priority or loosen the `component_open` check
  without understanding you are widening a security hole.
- Like `native`, `none` uses the credential model, so both `*_handshake`
  slots are `NULL` and must stay so.
