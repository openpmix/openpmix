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
this file covers only what is specific to `native`.

`native` uses **both** psec models. Its credential (`create_cred` +
`validate_cred`) decides most connections on its own. When it cannot,
`validate_cred` returns `PMIX_ERR_READY_FOR_HANDSHAKE` and its handshake
(`client_handshake` + `server_handshake`) finishes the job. The
user-facing description is
[`docs/how-things-work/authentication/native.rst`](../../../../docs/how-things-work/authentication/native.rst).

## Files

| File | Contents |
|------|----------|
| `psec_native.h` | Declares the component and module symbols, and `pmix_psec_native_params`. |
| `psec_native_component.c` | Component struct, MCA parameters, `component_query` (priority **10**, always available). |
| `psec_native.c` | The module: credential, identity rules, and the local-socket handshake. |
| `help-psec-native.txt` | Why a connection was refused: `remote-peer`, `legacy-peer`, `foreign-group`, `handshake-failed`, `socket-unreachable`. |

## MCA parameters

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `psec_native_socket_dir` | server's tmpdir | Where a server makes the directory for its handshake sockets. On a peer: where that same base directory appears to it. |
| `psec_native_legacy_auth` | `true` | Accept a peer from an older release on its claimed identity when the kernel cannot confirm it. |

## The credential

`create_cred` (V2/TCP protocol only) produces:

```
uid_t euid | gid_t egid | "PNX1" | uint8 idlen | idlen bytes of kernel id
```

- The uid and gid are raw host-endian `memcpy`s. A peer on this host
  shares the server's ABI, which is all native serves.
- The **trailer** (`"PNX1"` onward) was added with the local-socket
  handshake. Its presence says "I can run the handshake". Its absence is
  how a server recognizes an older release.
- The **kernel id** is `/proc/sys/kernel/random/boot_id` on Linux and
  `kern.bootsessionuuid` on macOS — empty elsewhere. It is the same for
  every process on one kernel, containers included. It is not secret and
  nothing trusts it: it only decides whether a handshake could possibly
  succeed.
- Every server back to v3.2 reads the uid and gid and ignores what
  follows (`sizeof(uid_t) <= ln` checks, not `==`). That is what makes
  appending safe. **Append after the kernel id; never reorder.**

## What `validate_cred` decides

After the `PMIX_CRED_TYPE` screen and the protocol checks (a
`PMIX_PROTOCOL_UNDEF` peer is refused explicitly):

1. **No connection** (`pr->sd < 0`, i.e. `PMIx_Validate_credential`):
   refused. The credential is only a claim.
2. **The kernel names the owner** — `pmix_util_getid_tcp()`
   ([`src/util/pmix_getid.c`](../../../util/pmix_getid.c)) finds the
   peer's socket in this host's TCP table. That uid is the peer's uid.
   The group is the claimed one (the kernel records none for a socket).
   Then `settle_identity()`. If that refuses **the group alone** and the
   peer sent the trailer, the answer is `PMIX_ERR_READY_FOR_HANDSHAKE`
   instead, so the handshake can attest the group: with host networking
   and a user namespace, the claimed gid is the one inside the namespace.
3. **The kernel cannot, and the peer sent the trailer:**
   - its kernel id differs from ours → refused (`remote-peer`): it is on
     another host, where no local socket reaches it;
   - otherwise → `PMIX_ERR_READY_FOR_HANDSHAKE`.
4. **The kernel cannot, and there is no trailer** (an older release):
   accepted on its claim when `legacy_auth` is true — the claim must
   still match the recorded uid/gid — else refused (`legacy-peer`).

### `settle_identity()` — registered vs. claimed

`pr->info->host_registered` decides how an established identity is used:

- **Registered** (a client the host registered): the established uid and
  group must equal the registration. The credential's own claim is not
  compared: inside a user namespace it is the uid *there*, which is not
  the uid the host knows.
- **Claimed** (a tool, or a proc that connected before the host
  registered it): the established uid **replaces** `pr->info->uid`. The
  claimed group stands if the user holds it
  (`pmix_psec_base_gid_held()`, or it is our own egid for our own uid).
  Otherwise an attested group (handshake path) replaces it, and an
  unattested one (TCP path) is refused (`foreign-group`).

On success `*info` carries `PMIX_CRED_TYPE`, and `PMIX_USERID` /
`PMIX_GRPID` set to the identity settled on.

## The local-socket handshake

For a peer the kernel's TCP table cannot see — typically a container with
its own network namespace. TCP remains the connection; the AF_UNIX
socket exists only for this exchange:

```
server -> peer (TCP):   u32 32, 32-byte nonce, u32 len + base dir, u32 len + relative path
peer   -> server (AF_UNIX): the nonce
server -> peer (TCP):   u32 status
```

- The server makes one directory per server process,
  `<socket_dir>/pmix-native.XXXXXX` (`mkdtemp`, mode 0711), on first
  use, and removes it at finalize. Several servers can share a base.
- Each handshake gets its own socket (`auth.<n>`, mode 0777 — any user
  may connect; the nonce and `SO_PEERCRED`/`getpeereid()` decide), with a
  backlog of one. It is unlinked as soon as it has accepted, and on every
  exit path.
- The peer joins the **relative path** to its own `socket_dir` if set,
  else to the base the server sent. That is how a container that mounts
  the directory elsewhere still finds it.
- The peer keeps its AF_UNIX socket open until the server's status
  arrives, so the server can read its credentials while it is connected.
- The server waits with `poll()` on the listener **and** the TCP socket.
  **The whole server half shares one deadline**,
  `ptl_base_connect_ack_timeout` from the start of the handshake: the
  wait for the connection and the read of the nonce (non-blocking, a
  `poll()` per chunk) each wait only for what is left of it. Do not go
  back to `SO_RCVTIMEO` and `pmix_ptl_base_recv_blocking` there: that
  timeout restarts with every `recv()`, so a peer sending one byte at a
  time held the server for 32 timeouts. A peer that cannot reach the
  socket returns an error, ptl closes the TCP connection, and the
  server's wait ends at once.
- The nonce is compared without an early exit.
- The identity read from the AF_UNIX socket goes through
  `settle_identity()` with `gid_attested = true`.

### Where the handshake runs

- **Client path** (`_cnct_complete`): the standard place — status
  `READY_FOR_HANDSHAKE`, then `PMIX_PSEC_SERVER_HANDSHAKE_IFNEED`.
- **Tool path** (`process_tool_request`): **early**, before the host's
  `tool_connected` up-call, so the identity the host is given — and that
  `allow_foreign_tools` and namespace ownership are judged on — is the
  settled one. The server sends `READY_FOR_HANDSHAKE` as the tool's
  *first* status, runs the handshake on the scratch peer, copies the
  scratch `info->uid/gid` back into `pnd`, and then the tool's normal
  replies follow (status, ids, `SEC_STATUS = SUCCESS`). Only a tool that
  sent the trailer can receive this, and both tool-side readers
  (`pmix_ptl_base_tool_handshake` and the event-driven `cnct_handle`)
  accept it.

## Gotchas

- **Never ask an older peer for a handshake.** Every released native
  client has `client_handshake == NULL`. The server only returns
  `READY_FOR_HANDSHAKE` for a credential with the trailer, and
  `PMIX_PSEC_CLIENT_HANDSHAKE` refuses rather than call a NULL slot.
- **Only a live connection has an owner.** `pmix_util_getid_tcp()`
  answers only for an `ESTABLISHED` entry that matches all four addresses
  and ports; a `TIME_WAIT`/`FIN_WAIT` entry carries no owner (Linux
  reports uid 0 for it). The unit test uses a closed connection to stand
  in for "a peer the kernel cannot see".
- **Remote peers use another mechanism.** native serves peers on this
  host's kernel. A remote peer from a current release is refused; one
  from an older release is accepted on its claim only while
  `legacy_auth` is true. Remote tools should use `munge` or
  [`ssl`](../ssl/AGENTS.md).
- **The handshake blocks the server's progress thread** for as long as
  the peer takes, up to `connect_ack_timeout` in all, like every psec
  handshake (see [`../../ptl/base/AGENTS.md`](../../ptl/base/AGENTS.md)).
  A legitimate peer answers at once.
- **The `PMIX_PROTOCOL_UNDEF` rejection is explicit on purpose.** It does
  not rely on the `(uid_t) -1` initializers of `euid`/`egid` failing a
  comparison. Keep it explicit.
- **`test/unit/psec_credentials.c`** covers the trailer parse, both
  identity rules, the legacy switch, the remote refusal, and the
  handshake end to end on two threads. Each of the handshake and remote
  cases was checked to fail with its check removed.
