<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSEC `ssl` Component

`ssl` authenticates a peer on another host with an X.509 certificate.
`native` serves only peers on its own host (see
[`native/AGENTS.md`](../native/AGENTS.md)).
Read the framework [`AGENTS.md`](../AGENTS.md) first; this file covers
only what is specific to `ssl`. It uses the **single-shot credential**
model (`create_cred` + `validate_cred`, both `*_handshake` pointers
`NULL`).

**It authenticates; it does not encrypt.** psec runs only while a
connection is being established and never sees the traffic after it, so
an `ssl` connection is exactly as confidential as a `native` one. Despite
the name, there is no TLS session here — only OpenSSL's certificate and
signature machinery.

## Files

| File | Contents |
|------|----------|
| `psec_ssl.h` | Component and module symbols; the MCA parameter struct. |
| `psec_ssl_component.c` | Component struct, parameter registration, `component_query`. |
| `psec_ssl.c` | The module: key/chain/CA loading, `create_cred`/`validate_cred`, `create_bound`/`validate_bound`/`mutual` (between hosts), the shared `make_statement`/`check_statement`, the replay list. |
| `testbuild_ssl.h` | Non-functional OpenSSL stand-in for `--enable-test-build`. Every call fails. |
| `help-psec-ssl.txt` | Configuration errors: unreadable files, an exposed or encrypted key, a key that is not the certificate's. |
| `configure.m4` | Opt-in `--with-openssl[=DIR]`; defines `PMIX_HAVE_PSEC_SSL` for the unit tests, only when the component is built against the real library. |

## When it is built, and when it is selected

- **Built** only with `--with-openssl` (OpenSSL 1.1.1 or later — the
  configure probe is `EVP_DigestSign`), or under `--enable-test-build`
  against the shim. It is opt-in for the same reason `munge` is: OpenSSL
  is present nearly everywhere, and a component compiled into `libpmix`
  adds its library to every consumer's link.
- **Active** only when configured: `component_query` declines silently
  unless there is a CA to check others against (`psec_ssl_ca_file`) or an
  identity to present (`psec_ssl_cert_file` + `psec_ssl_key_file`).
  `init` then loads whichever it was given, and refuses to start — with a
  `show_help` — on anything malformed. A failed `init` leaves the module
  off the actives list, so `finalize` is never called for it; `init`
  releases what it loaded on every failure path.
- **Offered to peers** only with a CA. A process given an identity and no
  CA connects with `ssl` but can check nobody's credential, so its
  `validates` hook keeps `ssl` off the list a server advertises
  (`pmix_psec_base_get_accepted_modules()`).
- **Priority 5**, below `native`'s 10. A process that says nothing keeps
  authenticating local peers with `native`. A tool connecting to a server
  on another host takes `ssl` itself when it can
  (`pmix_psec_base_remote_module`), and a server accepts it per peer.
- **Between hosts it needs all three on each end** (`mutual`): a
  certificate and key to prove who this end is, and a CA to check the
  other. A server asked for remote connections with less fails its init;
  a tool with less does not use `ssl` for a server on another host.

## The credential

A statement signed with the peer's private key, carrying the certificate
chain that vouches for the key. All integers are network byte order —
unlike `native`, this crosses hosts.

```
"PMIXSSL1"        8 bytes
uid               uint32
gid               uint32
time              uint64, seconds since the epoch
nonce             16 random bytes
chain length      uint32
chain             PEM certificates, leaf first, then intermediates
signature length  uint32
signature         over every byte before the signature length
```

The signature is SHA-256 with whatever key type the certificate holds,
except Ed25519/Ed448, which hash internally and must be handed no digest
(`digest_for()`). `create_cred` re-writes the configured chain from what
it parsed rather than sending the file's bytes, so nothing but
certificates goes on the wire.

A credential made for one connection between hosts (`create_bound`) is
`PMIXSSL2`: the same, with whose it is and the bytes it is bound to
signed after the nonce:

```
"PMIXSSL2"        8 bytes
uid, gid, time, nonce   as above
role              uint8: 1 a server's, 0 a tool's
bind length       uint32 (at most 256)
bind              the bytes ptl gives - see "Between hosts" in the framework AGENTS.md
chain length, chain, signature length, signature   as above
```

Each format is accepted only where it is expected: `validate_cred`
refuses a `PMIXSSL2` credential and `validate_bound` a `PMIXSSL1` one.

**This format is a wire contract.** A tool and a server may be different
releases. `test/unit/psec_ssl_auth.c` builds its credentials, of both
formats, by hand from the layouts above, so a change to either fails
that test's good cases as well as its bad ones — that is deliberate. Extend by bumping the magic, never
by reinterpreting a field.

## What `validate_cred` checks, in order

1. **Framing.** Every length came from the peer: the chain is bounded
   (64 KB, at most 10 certificates), the signature is bounded, and the
   lengths must account for the credential exactly.
2. **The chain** leads to a CA in `psec_ssl_ca_file`, with the
   intermediates the peer supplied treated as untrusted. The purpose is
   `X509_PURPOSE_SSL_CLIENT`, so a certificate whose extended key usage
   is only `serverAuth` is refused - except for a server's bound
   credential, whose purpose is `X509_PURPOSE_SSL_SERVER`: a certificate
   whose key usage is only `clientAuth` proves no server. A bound
   credential must also carry exactly the expected bytes, as the
   expected role, before anything else is read from it. With `psec_ssl_crl_file`,
   `X509_V_FLAG_CRL_CHECK` is set — which also means a leaf whose
   issuer has *no* CRL in the file is refused.
3. **The signature** is the leaf's.
4. **The user.** The leaf's subject must have exactly one common name,
   and it must be the name of a local account (`getpwnam_r`). The claimed
   uid must be that account's uid. The claimed gid must be a group that
   account holds (`pmix_psec_base_gid_held()`), since no certificate
   records a group.
5. **The time** is within `psec_ssl_max_skew` seconds of now, either way.
6. **The peer.** As every mechanism does, the identity must be the one
   the peer registered or connected as (`pr->info->uid`/`gid`). Only
   for a connection (`pr->sd` valid): a credential handed to
   `PMIx_Validate_credential` has none, and is reported as the identity
   it carries rather than compared with the validator's own.
7. **The nonce** has not been seen. This runs **last**, so nothing that
   failed an earlier check ever reaches the replay list — which is
   therefore bounded by what authenticated peers can send.

`check_statement()` does steps 1-5 for both formats; `validate_cred`
adds 6 and 7, and `validate_bound` adds 7 and returns the identity for
ptl to compare.

The replay list keeps each nonce for `2 * max_skew` seconds, which is the
whole span a credential could be accepted in. It has its own mutex,
because `PMIx_Validate_credential` runs `validate_cred` on whichever
application thread called it. Everything else in the module is written
in `init` and only read afterwards.

## Identity: what a certificate is allowed to claim

The certificate's CN is a **user name**, resolved on the server's host.
So:

- Uids must agree across hosts for the same user. A tool whose uid
  differs from the server host's uid for the same name is refused: the
  handshake header carries the tool's own uid, and that is what `ptl`
  records for it and hands the host.
- A `CN=root` certificate authenticates as root. Which certificates are
  issued is the CA's policy.

## Configuration (MCA parameters)

| Parameter | Meaning |
|-----------|---------|
| `psec_ssl_ca_file` | PEM CAs a peer's certificate must chain to. Needed to *accept* ssl connections, and by a tool to check a server on another host. |
| `psec_ssl_crl_file` | PEM CRLs; enables revocation checking. |
| `psec_ssl_cert_file` | This process's certificate, then any intermediates. Needed to *connect* with ssl, and by a server to prove who it is to a tool on another host. |
| `psec_ssl_key_file` | Its private key: PEM, unencrypted, owned by this process's user and not accessible to group or others (refused otherwise, as ssh refuses one). |
| `psec_ssl_max_skew` | Seconds either side of now a credential stays valid (default 300). |
| `psec_ssl_priority` | Default 5. |

A passphrase-protected key is refused rather than prompted for:
`PEM_read_bio_PrivateKey` is given a callback that answers nothing,
because OpenSSL's default reads the terminal.

## Properties of the mechanism

- **A `PMIXSSL1` credential names no connection.** One from
  `create_cred` - `PMIx_Get_credential` - is accepted by any validator
  that trusts its CA, within the time window; each validator's replay
  list refuses a nonce it has already seen. Between hosts ptl uses
  `PMIXSSL2` instead, bound to the connection's two nonces.
- **Between hosts, authentication is mutual, the server first.** On one
  host the tool checks the server by what the kernel says of it, and
  the server checks the tool with `native`.

## Testing

`test/unit/psec_ssl_auth.c` mints a root, an intermediate, an untrusted
root and a set of leaves at run time, then checks every refusal above
with a credential that gets exactly one thing wrong, plus a round trip of
the module's own credentials with an EC and an Ed25519 key, every
truncation of a good credential, revocation, and the refusal to start
with an exposed key. It also makes and checks bound credentials: as
each role, with bytes of their own and another's, presented twice, in
the other format, cut short, and from a certificate for clients only as
a server's. `test/unit/ptl_remote_auth.c` runs the whole exchange, tool
and server, with a PKI of its own. Each check in `validate_cred` was disabled in turn
and its case failed. The test is built only where `PMIX_HAVE_PSEC_SSL`
is true — never against the shim.
