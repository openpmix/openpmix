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

`native` uses the single-shot psec model: its credential (`create_cred`
+ `validate_cred`) decides every connection on its own, and it has no
handshake. The user-facing description is
[`docs/how-things-work/authentication/native.rst`](../../../../docs/how-things-work/authentication/native.rst).

## Files

| File | Contents |
|------|----------|
| `psec_native.h` | Declares the component and module symbols, and `pmix_psec_native_params`. |
| `psec_native_component.c` | Component struct, MCA parameters, `component_query` (priority **10**, always available). |
| `psec_native.c` | The module: the credential and the identity rules. |
| `help-psec-native.txt` | Why a connection was refused: `legacy-peer`, `unowned-peer`, `foreign-group`. |

## MCA parameters

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `psec_native_legacy_auth` | `false` | Accept a peer connecting over TCP on its claimed identity on a platform with no TCP table to ask. |

## The credential

`create_cred` (V2 protocol only) produces the peer's effective `uid_t`
and `gid_t`, and nothing more. A server reads those and **ignores
anything after them**, as every release has: a later release may append,
and must not be refused for it. (An unreleased trailer, `"PNX1"`, once
marked a peer able to run an AF_UNIX side handshake; it is gone, and a
server that sees it ignores it.)

## What `validate_cred` decides

After the `PMIX_CRED_TYPE` screen and the protocol checks (a
`PMIX_PROTOCOL_UNDEF` peer is refused explicitly):

1. **No connection** (`pr->sd < 0`, i.e. `PMIx_Validate_credential`):
   refused. The credential is only a claim.
2. **A local-transport connection** (`pr->transport->peer_identity`, the
   usock transport): the kernel's uid and gid for the far end are the
   peer's, whatever it claimed. Then `settle_identity()`, with the gid
   attested.
3. **Over TCP, the kernel names the owner** — `pmix_util_getid_tcp()`
   ([`src/util/pmix_getid.c`](../../../util/pmix_getid.c)) finds the
   peer's socket in this host's TCP table. That uid is the peer's uid.
   The group is the claimed one (the kernel records none for a TCP
   socket). Then `settle_identity()`, with the gid not attested.
4. **Over TCP, the kernel cannot.** A peer at an address of this host
   (`peer_on_host`) that the table does not show (`PMIX_ERR_NOT_FOUND`) is
   refused whatever `legacy_auth` says (`unowned-peer`): its end is in the
   table once it has sent. A platform with no table
   (`PMIX_ERR_NOT_SUPPORTED`, and only that) - accepted on its claim when
   `legacy_auth` is true, the claim still matching the recorded uid/gid,
   else refused (`legacy-peer`). Any other failed lookup -
   `PMIX_ERR_OUT_OF_RESOURCE` (the server is out of descriptors or
   memory), `PMIX_ERR_BAD_PARAM`, `PMIX_ERR_NOT_FOUND` for a peer not at
   an address of this host - is no answer, and refuses the peer
   (`PMIX_ERR_INVALID_CRED`, verbose output only), whatever
   `legacy_auth` says. A peer at an address not this host's never gets here:
   ptl refuses it first - native has no `create_bound`/`validate_bound`,
   and such a peer must authenticate with a module that has.

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
  Otherwise an attested group (local transport) replaces it, and an
  unattested one (TCP) is refused (`foreign-group`).

On success `*info` carries `PMIX_CRED_TYPE`, and `PMIX_USERID` /
`PMIX_GRPID` set to the identity settled on.

## Gotchas

- **Only a live connection has an owner.** `pmix_util_getid_tcp()`
  answers only for an `ESTABLISHED` entry that matches all four addresses
  and ports; a `TIME_WAIT`/`FIN_WAIT` entry carries no owner (Linux
  reports uid 0 for it). The unit test uses a closed connection to stand
  in for "a peer the kernel cannot see".
- **A relay's connection is the relay's.** A peer that comes over TCP
  through a process on this host that forwards connections (`socat`,
  `docker-proxy`) is reported by `pmix_util_getid_tcp()` as the relay's
  user: that is whose socket the server is connected to. Nothing in the
  connection tells a relay apart from the peer itself, so this is
  documented in `native.rst` (Limits), not handled in code.
- **Remote peers use another mechanism.** native serves peers on this
  host's kernel and has no `create_bound`/`validate_bound`: ptl refuses a
  TCP peer whose address is not this host's unless it and the server
  have authenticated each other with a module that has, before native is
  asked. Remote tools use `munge` or [`ssl`](../ssl/AGENTS.md).
- **The `PMIX_PROTOCOL_UNDEF` rejection is explicit on purpose.** It does
  not rely on the `(uid_t) -1` initializers of `euid`/`egid` failing a
  comparison. Keep it explicit.
- **`test/unit/psec_credentials.c`** covers the credential's shape, both
  identity rules and the legacy switch; `test/unit/ptl_accept.c` covers a
  local-transport peer whose credential claims another user.
