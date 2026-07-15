<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSEC `dummy_handshake` Component

`dummy_handshake` is a **test-only** component whose reason for existing
is to exercise the *multi-step handshake* path in `ptl` and `psec`. It is
not a real security mechanism — it swaps a fixed magic string over the
socket and checks it. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `dummy_handshake`. It is
the only in-tree component that uses the **handshake** model
(`client_handshake` + `server_handshake`, with `validate_cred` left
`NULL`).

## Files

| File | Contents |
|------|----------|
| `psec_dummy_handshake.h` | Declares the component and module symbols. |
| `psec_dummy_handshake_component.c` | Component struct + `component_query` (priority **100**). |
| `psec_dummy_handshake.c` | The module: `create_cred` + `client_handshake` / `server_handshake`. |

## When it is selected

It is **not built by default.** It has no `configure.m4`; instead
`config/pmix.m4` defines `--enable-dummy-handshake` (default: disabled),
which sets the Automake conditional `MCA_BUILD_PSEC_DUMMY_HANDSHAKE` that
its [`Makefile.am`](Makefile.am) wraps. In a stock build the component is
absent from `static-components.h` entirely.

When built, `component_query` returns the module at **priority 100** — the
highest of any psec component — so in a build configured with
`--enable-dummy-handshake` it becomes the default a peer gets from
`assign_module(NULL)`, which is exactly what you want when testing the
handshake path.

## The module and why `validate_cred` is `NULL`

```c
pmix_psec_module_t pmix_dummy_handshake_module = {
    .name = "dummy_handshake",
    .init = simple_init,
    .finalize = simple_finalize,
    .create_cred = create_cred,
    .client_handshake = client_hndshk,
    .validate_cred = NULL,          // <-- the switch
    .server_handshake = server_hndshk
};
```

Leaving `validate_cred` `NULL` is deliberate and load-bearing. On the
server, `PMIX_PSEC_VALIDATE_CONNECTION` sees no `validate_cred` but a
non-`NULL` `server_handshake`, so it returns
`PMIX_ERR_READY_FOR_HANDSHAKE` instead of validating a credential. That
signal is what makes `ptl` run `PMIX_PSEC_SERVER_HANDSHAKE_IFNEED` (and
the client its `PMIX_PSEC_CLIENT_HANDSHAKE`). If you filled `validate_cred`
in, the handshake code would never fire and the component would no longer
test what it exists to test.

## What the functions do

- **`create_cred`** returns a throwaway credential — the literal bytes
  `"dymmy_cred"` (the misspelling is in the source). It is essentially
  ignored; the real exchange is the handshake.
- **`server_hndshk(int sd)`** sends the length of a fixed magic string
  (`"PMIX_PSEC_DUMMY_HANDSHAKE_STRING"`) then the string itself over the
  socket using `pmix_ptl_base_send_blocking`, then reads back a status
  word from the client with `pmix_ptl_base_recv_blocking`.
- **`client_hndshk(int sd)`** reads the length then the string, verifies
  the length and bytes match the expected magic string (returning
  `PMIX_ERR_HANDSHAKE_FAILED` on mismatch), and sends `PMIX_SUCCESS` back
  to the server.

Both call the `ptl` base blocking send/recv helpers directly — this is
one of the few places outside `ptl` that touches the socket fd, which is
legitimate here because the handshake owns the connection during setup,
before steady-state traffic begins.

## Gotchas

- **Never ship this enabled.** It provides no real authentication; it is a
  test harness for the handshake plumbing. It is off by default for that
  reason — keep it that way.
- **It emits unconditional `pmix_output(0, …)` progress lines.** Those are
  fine for a test component but would be noise in a production module;
  don't copy that pattern into a real one.
- **The magic-string check is exact** (length *and* `strncmp`). If you use
  this component as a template for a genuine handshake module, replace the
  fixed-string swap with a real challenge/response, but preserve the
  `validate_cred == NULL` invariant that routes execution into the
  handshake path.
- Because it uses the handshake model, it does **not** implement
  `validate_cred`; do not "complete" the module by adding one.
