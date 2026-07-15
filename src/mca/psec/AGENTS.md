<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSEC Framework

This document orients AI agents and human contributors working in the
`psec` (**P**MIx **Sec**urity) framework. It assumes you have already
read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden rules,
prefix conventions, thread-safety model, wire-format/interoperability
rules, and MCA concepts described there all apply here and are not
repeated. This file covers what is specific to `psec`: what the framework
is for, the two credential models it supports, how a module is negotiated
between two peers during the connection handshake, and the contract every
component must honor. Each component subdirectory (`native/`, `none/`,
`munge/`, `dummy_handshake/`) carries its own `AGENTS.md` with
component-specific detail.

## What PSEC does

`psec` **authenticates a connection** between two PMIx processes. When a
client, tool, or server opens a socket to a PMIx server (all of that
socket machinery lives in [`ptl`](../ptl/AGENTS.md)), the two ends must
prove to each other that the connection is legitimate before any real
traffic flows. `psec` provides that proof in the form of a **credential**
— an opaque `pmix_byte_object_t` the connecting side creates and the
accepting side validates.

Concretely, `psec` owns two operations, each with a client half and a
server half:

1. **Credential creation / validation** (the common case). The connecting
   side calls `create_cred` to produce a credential; the accepting side
   calls `validate_cred` to check it. This is a **single-shot** exchange:
   the credential rides inside the `ptl` connection handshake blob, and
   the server accepts or rejects it in one step.
2. **A multi-step handshake** (the rare case). Some security protocols
   need a live back-and-forth over the socket — a challenge, a response,
   a confirmation — that a single credential blob cannot express. For
   these, a module provides `client_handshake` / `server_handshake`,
   which read and write the socket directly during connection setup.

`psec` is **not** in the steady-state data path. It runs only while a
connection is being established. Once a peer is validated, `psec` is not
consulted again for that peer.

The same `create_cred` / `validate_cred` functions also back the public
credential APIs (`PMIx_Get_credential` / `PMIx_Validate_credential`) via
[`src/common/pmix_security.c`](../../../src/common/pmix_security.c), so a
module's create/validate logic must work both as part of the connection
handshake and as a standalone request carrying caller directives.

## Single-select in a client, multi-select in a server

The header comment in [`psec.h`](psec.h) states the rule precisely:

> Only *one* plugin will be active in a client, but multiple plugins may
> be active in a server. Thus, this is a multi-select framework.

So `psec` is a **multi-select** framework — `pmix_psec_base_select`
builds a priority-ordered list of *all* usable modules
(`pmix_psec_globals.actives`) rather than choosing a single winner.
But how that list is used differs by role:

- A **client / tool** selects exactly one module for itself at init
  (`pmix_psec_base_assign_module(getenv("PMIX_SECURITY_MODE"))` in
  [`src/client/pmix_client.c`](../../../src/client/pmix_client.c) and
  [`src/tool/pmix_tool.c`](../../../src/tool/pmix_tool.c)) and uses it
  for its one connection *up* to its server. It also assigns that same
  module to its `myserver` peer, because both ends of that socket must
  speak the same protocol.
- A **server** keeps the whole `actives` list and picks a module
  **per accepted peer**, by name, from the handshake (see "Negotiation"
  below). Different clients on the same server may therefore be
  authenticated with different modules.

The selected module for a given peer is reached through
`peer->nptr->compat.psec` — a `pmix_psec_module_t *` cached on the peer's
namespace compatibility struct. All the `PMIX_PSEC_*` macros dereference
that pointer; there is **no** exported "current module" global the way
`ptl` has `pmix_ptl`.

## The two credential models

This is the single most important distinction to understand before
editing `psec`, because a module implements *one* of these two models and
the framework macros branch on which functions it left `NULL`.

### Single-shot credential (`create_cred` + `validate_cred`)

The common model. The credential is a self-contained blob:

- Client side: `create_cred` fills in a `pmix_byte_object_t` (and may
  return an info array naming the issuing agent). The blob is packed into
  the `ptl` handshake by `pmix_ptl_base_construct_message`.
- Server side: `validate_cred` inspects the blob (and/or the socket) and
  returns `PMIX_SUCCESS` or `PMIX_ERR_INVALID_CRED`.

`native`, `munge`, and `none` all use this model (they leave both
`*_handshake` pointers `NULL`).

### Multi-step handshake (`client_handshake` + `server_handshake`)

The rare model, for protocols that need live socket I/O during setup.
A module leaves `validate_cred` `NULL` and instead supplies
`server_handshake` (and a matching `client_handshake`). The framework
detects the absence of `validate_cred` and drives the handshake instead
(see the macro logic below). `dummy_handshake` is the only in-tree
component that uses this model; it exists precisely to exercise and
test the `ptl` handshake code path.

Per the header, it would be "rare" for a module to use *both* the
credential and the handshake interfaces — one of the two pairs is
normally `NULL`.

## Module interface (`pmix_psec_module_t`)

Defined in [`psec.h`](psec.h). A component fills in the subset it needs;
unused pointers stay `NULL`, and the framework macros guard on them.

| Field | Signature | Side | Purpose |
|-------|-----------|------|---------|
| `name` | `char *` | both | component name string; this is what travels in the handshake and what `assign_module` matches on |
| `init` | `(void) -> pmix_status_t` | both | one-time module init at selection; return non-success to disqualify the module (e.g. `munge` checks the local `munged` daemon is reachable) |
| `finalize` | `(void) -> void` | both | module tear-down, invoked by `pmix_psec_close` for each active module at framework close (e.g. `munge_finalize` frees the cached credential) |
| `create_cred` | `(peer, directives, ndirs, &info, &ninfo, cred)` | client | build a credential blob for this process |
| `client_handshake` | `(int sd)` | client | run the client half of a live handshake over socket `sd` |
| `validate_cred` | `(peer, directives, ndirs, &info, &ninfo, cred)` | server | check a received credential; `NULL` signals "I need a handshake instead" |
| `server_handshake` | `(int sd)` | server | run the server half of a live handshake over socket `sd` |

The `directives`/`info` pairs carry the attribute arrays required of
every PMIx interface: `directives` are caller inputs (e.g.
`PMIX_CRED_TYPE` naming which mechanism to use), and `*info` is an
allocated output array the module fills with results (e.g. the issuing
agent name, or the `PMIX_USERID`/`PMIX_GRPID` a validated credential
contained). The credential itself is a `pmix_byte_object_t` (bytes +
size) so it can be either a printable string or raw binary.

## Component interface (`pmix_psec_base_component_t`)

Also in [`psec.h`](psec.h). Beyond the standard `pmix_mca_base_component_t`
base, a psec component adds:

| Field | Purpose |
|-------|---------|
| `priority` | ordering weight, echoed through `component_query` |
| `init` / `finalize` | *component*-level (not module-level) hooks; currently unused by all four components |
| `assign_module` | returns this component's `pmix_psec_module_t *` — the hook the base uses to hand out a module by name |

Every component struct opens with the version macro
`PMIX_PSEC_BASE_VERSION_1_0_0` and sets `.pmix_mca_component_name` to its
directory name.

## The base layer

`psec`'s base is small — there is no per-message routing layer like
`preg`'s stubs, because callers invoke the selected module directly
through the `PMIX_PSEC_*` macros. The base does three things: framework
open/close and selection, and handing out modules by name.

### `base/psec_base_frame.c` — framework decl and lifecycle

Declares the framework (`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, psec, …)`),
instantiates the `pmix_psec_globals` struct (the `actives` list plus
`initialized` / `selected` flags), and defines the `PMIX_CLASS_INSTANCE`
for `pmix_psec_base_active_module_t`. `pmix_psec_open` opens all
components; `pmix_psec_close` walks `actives`, calls each active
module's `finalize`, releases the wrappers, and closes the components.
Note that
this frame registers **no MCA parameters of its own** — there is no
`register` function (see "MCA parameters" below).

### `base/psec_base_select.c` — building the actives list

`pmix_psec_base_select` (called once from
[`src/runtime/pmix_init.c`](../../../src/runtime/pmix_init.c)) queries
every component. For each that returns a module, it calls the module's
`init` (if any) and, on success, wraps it in a
`pmix_psec_base_active_module_t { pri, module, component }` inserted into
`pmix_psec_globals.actives` in **descending priority order**. A module
whose `init` fails is silently skipped — this is how `munge` removes
itself when the `munge` daemon is unreachable. If the list ends up empty
it emits the shared `no-plugins` `show_help` topic (from
`help-pmix-runtime.txt`) and returns `PMIX_ERR_SILENT`: `psec` requires
at least one usable module. At verbosity >4 it prints the resolved
priority list.

### `base/psec_base_fns.c` — module lookup

Two exported helpers, both in [`base/base.h`](base/base.h):

- **`pmix_psec_base_get_available_modules()`** — returns a comma-joined
  string of the active component names (highest priority first). This is
  the list a server advertises so a connecting peer knows which
  mechanisms it can choose from.
- **`pmix_psec_base_assign_module(const char *options)`** — the heart of
  negotiation. Given `options` (a comma-separated list of acceptable
  names, or `NULL`), it walks `actives` in priority order and returns the
  first module whose component name matches one of the requested names.
  With `options == NULL` it returns the highest-priority active module
  (each `active->component->assign_module()` simply hands back that
  component's module). Returns `NULL` if nothing matches — which the
  callers treat as a fatal connection/init error.

## Negotiation: how two peers agree on a module

The module name is the contract that lets differently-configured peers
interoperate — the same idea as `preg`'s self-identifying tag, but
carried in the `ptl` handshake rather than in the payload:

1. A client selects its one module at init and, in
   `pmix_ptl_base_construct_message`
   ([`ptl_base_fns.c`](../ptl/base/ptl_base_fns.c)), writes
   `psec->name` into the handshake blob alongside the credential produced
   by `PMIX_PSEC_CREATE_CRED`.
2. On accept, the server parses that name out of the handshake
   (`pnd->psec`) and calls
   `peer->nptr->compat.psec = pmix_psec_base_assign_module(pnd->psec)`
   in [`ptl_base_connection_hdlr.c`](../ptl/base/ptl_base_connection_hdlr.c).
   If the server has no matching active module, the connection is
   refused.
3. The server then validates via the **`PMIX_PSEC_VALIDATE_CONNECTION`**
   macro ([`psec.h`](psec.h)), which encodes the single-shot-vs-handshake
   branch:
   - if the module has a `validate_cred`, call it;
   - else if it has a `server_handshake`, set the reply to
     `PMIX_ERR_READY_FOR_HANDSHAKE` (a signal, not an error);
   - else return `PMIX_ERR_NOT_SUPPORTED` (a module with neither is
     invalid).
4. The server sends that reply status back, then runs
   **`PMIX_PSEC_SERVER_HANDSHAKE_IFNEED`**, which invokes
   `server_handshake` *only* when the reply was
   `PMIX_ERR_READY_FOR_HANDSHAKE`. The client mirror is
   `pmix_ptl_base_client_handshake`, which calls
   **`PMIX_PSEC_CLIENT_HANDSHAKE`** on the same signal.

So `validate_cred == NULL` is not an oversight in `dummy_handshake`: it
is the switch that tells the framework to take the handshake path. Keep
that invariant in mind before "filling in" a `NULL` slot.

## The `PMIX_PSEC_*` macros ([`psec.h`](psec.h))

Back-end code never calls a module function pointer directly; it uses
these macros, all of which dereference `p->nptr->compat.psec`:

| Macro | Runs | Notes |
|-------|------|-------|
| `PMIX_PSEC_CREATE_CRED(r,p,d,nd,in,nin,c)` | client | build a credential |
| `PMIX_PSEC_VALIDATE_CRED(r,p,d,nd,in,nin,c)` | server | validate directly (used by the credential APIs) |
| `PMIX_PSEC_VALIDATE_CONNECTION(r,p,…)` | server | validate *or* request a handshake — the branch above |
| `PMIX_PSEC_CLIENT_HANDSHAKE(r,p,sd)` | client | run `client_handshake` |
| `PMIX_PSEC_SERVER_HANDSHAKE_IFNEED(r,p)` | server | run `server_handshake` iff `r == PMIX_ERR_READY_FOR_HANDSHAKE` |

`PMIX_PSEC_VALIDATE_CONNECTION` and `PMIX_PSEC_SERVER_HANDSHAKE_IFNEED`
are the two that contain real control flow; the rest are thin call
wrappers.

## Selection and priorities

Default priorities come from each component's `component_query`:

| Component | Priority | Model | Active when |
|-----------|----------|-------|-------------|
| `dummy_handshake` | 100 | handshake | built only with `--enable-dummy-handshake`; always active when built |
| `munge` | 80 | single-shot | built only if libmunge is found; `init` succeeds only if the `munged` daemon issues a credential |
| `native` | 10 | single-shot | always (no `configure.m4`, no gate) |
| `none` | 0 | single-shot (no-op) | **only** if the `psec` MCA value explicitly names `none` (its `component_open` checks) |

Because the list is priority-ordered and `assign_module(NULL)` returns
the highest-priority module, in a stock build (`dummy_handshake` off,
`none` not requested) a peer that does not name a mechanism gets `munge`
if it initialized, otherwise `native`.

## MCA parameters

`psec` registers **no framework-specific MCA parameters** — there is no
`register` function in `psec_base_frame.c`. Behavior is steered entirely
through the implicit framework component-selection variable, `psec`
(i.e. `PMIX_MCA_psec`, e.g. `PMIX_MCA_psec=native`), which restricts
which components are opened/considered. As a convenience,
[`pmix_init.c`](../../../src/runtime/pmix_init.c) copies the
`PMIX_SECURITY_MODE` environment variable into `PMIX_MCA_psec` at
startup, and clients pass their selected mechanism to
`assign_module` via that same `PMIX_SECURITY_MODE` env var. The `none`
component reads this variable directly in its `component_open` to decide
whether it is allowed to participate. Per the top-level guidance, prefer
adding an attribute honored by `create_cred`/`validate_cred` (they
already take a directives array) over hard-coding new behavior.

## Threading

`psec` module functions are **synchronous** and run inline wherever the
connection is being established:

- On a client/tool/server, `create_cred` runs during `PMIx_Init` /
  `PMIx_tool_init` in the caller's thread as part of the blocking connect.
- On a server, `validate_cred` / `server_handshake` run inside the `ptl`
  connection handler on the progress thread, with the socket temporarily
  in blocking mode for the handshake I/O.
- The credential-API path (`pmix_security.c`) invokes
  `create_cred`/`validate_cred` through the macros *inline in the calling
  thread* — it does **not** thread-shift — consistent with `psec` doing
  no thread-shifting of its own.

There is **no caddy pattern inside `psec`** — the framework does no
thread-shifting of its own; it relies on its callers to have already put
execution where it belongs. Module functions must therefore be free of
their own async hops: do the work and return a status. They may allocate
the `*info` output array (the caller frees it) but must not stash pointers
into caller-owned `directives`.

## Directory layout

```
src/mca/psec/
├── psec.h                    Framework API: module & component structs, version macro, PMIX_PSEC_* macros
├── base/
│   ├── base.h                Internal base API + pmix_psec_globals + active-module struct
│   ├── psec_base_frame.c     open/close, framework decl, class instance (no MCA params)
│   ├── psec_base_select.c    query components, build priority-ordered actives list
│   └── psec_base_fns.c       get_available_modules + assign_module (name lookup)
├── native/                   uid/gid-over-socket credential (default, always available)
├── none/                     no-op module (opt-in only)
├── munge/                    MUNGE credentials (conditional on libmunge)
└── dummy_handshake/          test-only multi-step handshake (opt-in build)
```

## Building

`native` and `none` have no `configure.m4` and are always compiled into
`libpmix`; they are wired through the generated `base/static-components.h`
(which in a stock build lists exactly `native` and `none`). The other two
are conditional:

- **`munge`** ships a [`configure.m4`](munge/configure.m4) that runs
  `OAC_CHECK_PACKAGE` for `munge.h` / `libmunge`. It is built only if
  MUNGE is present (or errors out if `--with-munge` was requested and not
  found). Its source can be *compile-tested* without the real library via
  the `#if 0` stub block at the top of `psec_munge.c` — that block is not
  used in real builds.
- **`dummy_handshake`** has no `configure.m4`; it is gated by the
  Automake conditional `MCA_BUILD_PSEC_DUMMY_HANDSHAKE`, set by
  `--enable-dummy-handshake` (default: disabled) in `config/pmix.m4`.

`psec` ships **no `show_help` file of its own** — the only `show_help` it
uses (`no-plugins`) lives in `help-pmix-runtime.txt` — so the
regenerate-the-help-content golden rule does not bite here. Editing a
`Makefile.am` needs only a plain `make`; adding or removing a *component
directory*, or changing a `configure.m4`, changes the build wiring
resolved by `configure`, so re-run `./autogen.pl && ./configure … &&
make`.

## When working in this framework

- **`name` is a wire contract.** It is what a client advertises and a
  server matches on via `assign_module`. Renaming a component's module
  breaks interoperability with peers that request it by the old name;
  treat it like the `preg` tag or a `bfrops` version.
- **`validate_cred == NULL` means "handshake."** Do not fill it in for a
  handshake-style module, and do not null it out for a credential-style
  one — `PMIX_PSEC_VALIDATE_CONNECTION` branches on exactly this.
- **Match `create_cred` to `validate_cred` and `client_handshake` to
  `server_handshake`.** The two ends of a connection may be built from
  different PMIx releases; a change to what one side emits must be a
  change the other side already tolerates. Extend by appending, never by
  reformatting an existing credential.
- **`native`'s credential is raw host-endian `uid`/`gid` bytes** (a
  `memcpy` of `uid_t`/`gid_t`), not a portable encoding. It works because
  a client and its local server share an ABI, but do not assume it would
  survive a cross-endian or differing-`uid_t`-width pair. If you ever need
  wire portability there, add a new mechanism rather than mutating
  `native`.
- **Do not thread-shift inside a module.** psec functions run where their
  callers place them; keep them synchronous. Allocate only the `*info`
  output (caller-freed); never retain caller `directives`.
- **A new mechanism is a new component, not a new API.** Prefer expressing
  optional behavior as a `PMIX_CRED_TYPE`-style directive on the existing
  create/validate calls; add a whole component only for a genuinely new
  authentication protocol, and discuss it on a GitHub issue first.
