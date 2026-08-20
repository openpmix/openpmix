<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSEC `native` Component

`native` is PMIx's built-in, always-available security module. It
authenticates a connection using the operating system's own notion of the
peer's user and group — the effective `uid`/`gid` — with no external
daemon or library. Read the framework [`AGENTS.md`](../AGENTS.md) first;
this file covers only what is specific to `native`. It uses the
**single-shot credential** model (`create_cred` + `validate_cred`, both
`*_handshake` pointers `NULL`).

## Files

| File | Contents |
|------|----------|
| `psec_native.h` | Declares the component and module symbols. |
| `psec_native_component.c` | Component struct + `component_query` (priority **10**, always available). |
| `psec_native.c` | The module: `init`/`finalize` + `create_cred` / `validate_cred`. |

## When it is selected

`native` has no `configure.m4`, so it is always compiled in, and its
`component_query` unconditionally returns the module at **priority 10**.
Because `assign_module(NULL)` returns the highest-priority active module,
`native` is the default mechanism whenever `munge` is absent or failed to
init and `dummy_handshake` is not built — which is the common case. It is
also selected explicitly whenever a peer names `native` in its security
mode.

## The module

```c
pmix_psec_module_t pmix_native_module = {
    .name = "native",
    .init = native_init,
    .finalize = native_finalize,
    .create_cred = create_cred,
    .validate_cred = validate_cred
};
```

`init`/`finalize` only emit verbose output; there is no external system to
connect to.

## What `create_cred` does

Runs on the connecting side. It branches on the peer's `ptl` protocol:

- **`PMIX_PROTOCOL_V1`** (the legacy Unix-domain-socket transport):
  produces an *empty* credential. The kernel already knows the peer's
  identity on a `usock` connection, so there is nothing to send — the
  server reads it off the socket (see below).
- **`PMIX_PROTOCOL_V2`** (TCP, today's transport): packs the process's
  effective `uid` then `gid` as raw bytes into the credential
  (`sizeof(uid_t) + sizeof(gid_t)`). This is what the server validates.
- Any other protocol → `PMIX_ERR_NOT_SUPPORTED`.

If the caller passed a `PMIX_CRED_TYPE` directive, `create_cred` first
checks that `"native"` is among the requested types (via
`pmix_psec_base_check_directives()` — see the framework
[`AGENTS.md`](../AGENTS.md)) and returns `PMIX_ERR_NOT_SUPPORTED` if not.
On success it fills the `*info` output array with a single
`PMIX_CRED_TYPE = "native"` entry marking the issuer. Note that it
constructs — and so empties — the credential it is handed before it
decides anything, so a caller cannot reuse the same
`pmix_byte_object_t` across a declined call and expect its contents to
survive.

## What `validate_cred` does

Runs on the accepting (server) side and recovers the peer's `uid`/`gid`
two different ways depending on protocol:

- **`PMIX_PROTOCOL_V1`**: ignores the (empty) credential and asks the
  kernel directly — `getsockopt(SO_PEERCRED)` on Linux, or `getpeereid()`
  on platforms that have it. If neither is available it returns
  `PMIX_ERR_NOT_SUPPORTED`.
- **`PMIX_PROTOCOL_V2`**: reads the `uid` then `gid` back out of the
  credential bytes the client packed, rejecting a `NULL` or too-short
  credential with `PMIX_ERR_INVALID_CRED`.

- **`PMIX_PROTOCOL_UNDEF`**: rejects with `PMIX_ERR_INVALID_CRED`. A peer
  whose transport was never established offers neither a socket to
  interrogate nor a credential format to trust, so there is nothing here
  that can be validated.

It then compares the recovered `euid`/`egid` against the values recorded
for the peer (`pr->info->uid` / `pr->info->gid`) and returns
`PMIX_ERR_INVALID_CRED` on any mismatch. On success it fills `*info` with
three entries: `PMIX_CRED_TYPE = "native"`, plus the validated
`PMIX_USERID` and `PMIX_GRPID`. As with `create_cred`, a `PMIX_CRED_TYPE`
directive that does not include `"native"` short-circuits to
`PMIX_ERR_NOT_SUPPORTED` — and that screen runs *first*, before the
protocol branch, so "not my mechanism" is never reported as "bad
credential.

## Gotchas

- **The V2 credential is raw host-endian `uid_t`/`gid_t` bytes.** It is a
  `memcpy`, not a portable encoding. It works because a client and its
  local server share an ABI, but it is not safe across differing
  endianness or integer widths. Do not "reuse" this format for a remote
  or cross-platform mechanism — add a new component instead.
- **V1 validation ignores the credential entirely** and trusts the
  socket. That is correct for `usock` (the kernel is the source of
  truth), but it means the credential bytes are meaningless on V1 — don't
  add checks that assume they carry data.
- The `SO_PEERCRED` / `getpeereid` selection is `#ifdef`-heavy for
  portability; if you touch it, preserve the fallbacks and the final
  `PMIX_ERR_NOT_SUPPORTED` for platforms that offer neither.
- **The `ucred` declaration guard has to match the guard on its use.**
  The declaration block names `HAVE_STRUCT_SOCKPEERCRED_UID` explicitly
  even though the use site tests `HAVE_STRUCT_UCRED_UID`, because it is
  that block which `#define`s the latter for the `sockpeercred`
  platforms. Guarding the declaration on `SO_PEERCRED` alone leaves
  `ucred` and `crlen` unused — and therefore a `-Werror` build failure —
  on a platform that has `SO_PEERCRED` but neither field macro.
- **The `PMIX_PROTOCOL_UNDEF` rejection is explicit on purpose.** It used
  to fall through to the `uid`/`gid` comparison and be rejected only
  because `euid`/`egid` were still their `(uid_t) -1` initializers.
  "Cleaning up" those initializers to `0` would have turned an
  unauthenticated peer into a successful validation against a `root`
  registration. Do not reintroduce the implicit form.
- Because `native` uses the credential model, both `*_handshake` slots are
  `NULL` and must stay so — the framework would misread a non-`NULL`
  `server_handshake` as "this module wants a live handshake."
