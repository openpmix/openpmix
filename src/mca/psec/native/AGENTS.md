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
| `help-psec-native.txt` | Why a connection was refused: `unverified-peer`, `foreign-group`. |

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
according to the protocol:

- **`PMIX_PROTOCOL_V2`**: reads the `uid` then `gid` back out of the
  credential bytes the client packed, rejecting a `NULL` or too-short
  credential with `PMIX_ERR_INVALID_CRED`.

- **`PMIX_PROTOCOL_UNDEF`**: rejects with `PMIX_ERR_INVALID_CRED`. A peer
  whose transport was never established has no credential format, so
  there is nothing here that can be validated.

**Then it confirms the peer's identity with the kernel**, before
comparing anything (`check_os_identity()`):

- `pmix_util_getid_tcp()` ([`src/util/pmix_getid.c`](../../../util/pmix_getid.c))
  looks the peer's end of `pr->sd` up in the host's own TCP table and
  returns the uid that created that socket. The claimed `euid` must equal
  it.
- A **tool** names its own group — nobody registered one for it — and no
  kernel table records a socket's group, so a tool's claimed `egid` must
  also be a group its (kernel-confirmed) user holds: its primary group or
  one listing it as a member. Root holds every group. A client's group was
  registered by the host, and is compared against that below.
- If the kernel cannot answer — the peer is on another host or in another
  network namespace, the platform has no way to ask, or there is no
  connection at all (`PMIx_Validate_credential`) — the credential is
  refused, with the `unverified-peer` help once. native serves peers on
  this host; a remote peer authenticates with [`ssl`](../ssl/AGENTS.md)
  or `munge`.

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
- **The identity comes from the connection.** The
  `SO_PEERCRED`/`getpeereid()` branch served the `usock` transport
  (`PMIX_PROTOCOL_V1`), which is gone along with the v1.x peers that were
  its only users. Every connection is now TCP, and `check_os_identity()`
  confirms the uid in the credential against the owner of the peer's
  socket. The wire format is unchanged, so older clients connect as
  before. **Keep `check_os_identity()` ahead of the comparisons in
  `validate_cred`.**
- **Only a live connection has an owner.** `pmix_util_getid_tcp()`
  answers only for an `ESTABLISHED` entry that matches all four addresses
  and ports; a `TIME_WAIT`/`FIN_WAIT` entry carries no owner (Linux
  reports uid 0 for it).
- **Remote peers are refused.** native confirms identity through the
  local kernel, so it serves only peers on this host. A deployment that
  accepts remote tool connections (`PMIX_SERVER_REMOTE_CONNECTIONS`) uses
  `ssl` or `munge`. On a platform `pmix_util_getid_tcp()` does not
  support (anything but Linux and macOS today), native refuses every
  peer; supporting such a platform means adding a lookup for it.
- **The `PMIX_PROTOCOL_UNDEF` rejection is explicit on purpose.** It does
  not rely on the `(uid_t) -1` initializers of `euid`/`egid` failing the
  `uid`/`gid` comparison. Keep it explicit; do not reintroduce the
  implicit form.
- Because `native` uses the credential model, both `*_handshake` slots are
  `NULL` and must stay so — the framework would misread a non-`NULL`
  `server_handshake` as "this module wants a live handshake."
