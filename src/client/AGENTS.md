<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PMIx Client Library

This document orients AI agents and human contributors working in
`src/client`, the client-role implementation of the PMIx APIs. It assumes
you have already read the top-level [`AGENTS.md`](../../AGENTS.md) — the
golden rules (prefix conventions, `pmix_config.h`-first include order,
constant-on-the-left comparisons, brace-everything, warning-free under
`--enable-devel-check`), the **thread-safety / progress-thread model and
the caddy pattern**, the backward-compatibility and wire-format rules,
and the copyright-header requirement all apply here and are not repeated.
This file covers what is specific to `src/client`: which public API each
file implements, the two recurring control-flow shapes (the blocking↔`_nb`
wrapper and the PTL send/recv round-trip), the client's global state, and
the invariants that are easy to break.

Like `src/class`, **`src/client` is not an MCA framework.** There is no
framework header, no components, no selection logic — just a set of `.c`
files compiled straight into `libpmix` (see [Building](#building)). It
does, however, lean heavily on nearly every framework: `ptl` for
transport, `bfrops` for pack/unpack, `gds` for the local datastore,
`psec` for the connection handshake, `pmdl`/`pnet` for spawn/fabric
support, and `preg` for regex proc expansion.

## What this directory is

`src/client` is the code that runs inside an application process (a
"client") that called `PMIx_Init`. It owns:

- the **connection lifecycle** to the local PMIx server (`pmix_client.c`);
- the **client half of every public `PMIx_*` operation** — get, fence,
  publish/lookup, spawn, connect/disconnect, group, resolve, fabric,
  topology — each of which either satisfies the request locally or packs
  a command and round-trips to the server;
- the client's **global state** (`pmix_client_ops.h`:
  `pmix_client_globals`).

A parallel `src/server` tree implements the server role and `src/tool`
the tool role; the three share `src/common`, `src/include/pmix_globals.*`,
and the frameworks. Many APIs in this directory contain role branches
(`PMIX_PEER_IS_SERVER`, `PMIX_PEER_IS_SCHEDULER`, `PMIX_PEER_IS_TOOL`)
because a server or tool process also links this same client code and may
enter these entry points.

## The two control-flow shapes

Almost every function in this directory is an instance of one of two
patterns. Learn to recognize them before editing.

### 1. The blocking ↔ non-blocking wrapper

The public API is offered in two forms: a blocking `PMIx_Foo` and a
non-blocking `PMIx_Foo_nb` that takes a user callback. The blocking form
is almost always a **thin wrapper** over the `_nb` form:

```c
PMIx_Foo(...) {
    pmix_cb_t cb;                        // or PMIX_NEW(pmix_cb_t)
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    rc = PMIx_Foo_nb(..., internal_cbfunc, &cb);  // pass an internal cb
    if (PMIX_SUCCESS != rc) { ...; return rc; }
    PMIX_WAIT_THREAD(&cb.lock);          // block until the cb fires
    rc = cb.status;
    PMIX_DESTRUCT(&cb);
    return rc;
}
```

The internal callback (commonly `op_cbfunc` / `mycbfunc` / `wait_cbfunc`)
records the result into the `pmix_cb_t` and calls `PMIX_WAKEUP_THREAD`.
When `cbfunc == NULL` is passed down, the `_nb` code recognizes the
blocking case: the caddy pointer it receives *is* the caller's stack `cb`,
so it must **not** `PMIX_NEW` its own and must **not** `PMIX_RELEASE` it.
Every `_nb` branch keys off `NULL != cbfunc` to decide whether to allocate
a caddy or reuse the passed-in one — getting that test wrong frees (or
double-frees) a stack object.

> **This wrapper is *not* the "don't call thread-shifting public APIs
> internally" hazard.** The `_nb` variants here do their work and then
> `PMIX_PTL_SEND_RECV` (or compute locally); they do not thread-shift, so
> a blocking wrapper calling its own `_nb` sibling is safe and is the
> sanctioned pattern. The hazard the top-level guide warns about is
> calling a *different* public API that posts a caddy to the progress
> thread — e.g. calling `PMIx_Notify_event`/`PMIx_Register_event_handler`
> and then `PMIX_WAIT_THREAD` from code that is *already on* the progress
> thread. `pmix_client_group.c` does exactly that on the caller thread
> during invite/join and would deadlock if driven from a handler (see its
> own header comments).

### 2. The PTL send/recv round-trip

When the request cannot be answered locally, the `_nb` code:

1. builds a `pmix_buffer_t` and `PMIX_BFROPS_PACK`s a command
   (`PMIX_*_CMD`) plus the operation's arguments;
2. allocates or reuses a `pmix_cb_t` holding the caller's callback and a
   pointer to any output object (a `pmix_fabric_t`, a `pmix_pdata_t[]`,
   etc. — **by pointer, not copied**, per the no-copy caddy rule);
3. calls `PMIX_PTL_SEND_RECV(rc, myserver, msg, recv_cbfunc, cb)` — this
   **takes ownership of `msg`** and registers `recv_cbfunc` to fire on the
   progress thread when the server replies.

`recv_cbfunc` runs on the progress thread. It first checks
`PMIX_BUFFER_IS_EMPTY(buf)` — a **zero-byte buffer means the connection
was lost** and the recv is being completed synthetically — then unpacks a
`PMIX_STATUS`, then any payload, stores payload via the GDS as needed, and
finally either invokes the user callback and `PMIX_RELEASE`s the caddy
(non-blocking) or `PMIX_WAKEUP_THREAD`s the waiter (blocking).

Getting the release/wakeup discipline right on **every** exit path
(including the pack-failure and send-failure early returns) is where most
of the bugs in this directory live. When a `PMIX_PTL_SEND_RECV` returns an
error, the caddy's recv callback will *not* fire, so the send site must
clean up the caddy itself — and then **return**, not fall through into the
blocking wait.

## The files

| File | Public API | Notes |
|------|-----------|-------|
| `pmix_client.c` | `PMIx_Init`, `PMIx_Finalize`, `PMIx_Initialized`, `PMIx_Get_version`, `PMIx_Abort`, `PMIx_Put`, `PMIx_Commit` | Connection lifecycle; the big one. |
| `pmix_client_get.c` | `PMIx_Get`, `PMIx_Get_nb` | Local-query parser, realm resolution, pending-request coalescing. |
| `pmix_client_fence.c` | `PMIx_Fence`, `PMIx_Fence_nb` | Collective barrier + modex. |
| `pmix_client_pub.c` | `PMIx_Publish[_nb]`, `PMIx_Lookup[_nb]`, `PMIx_Unpublish[_nb]` | Publish/lookup data store. |
| `pmix_client_spawn.c` | `PMIx_Spawn`, `PMIx_Spawn_nb` | Harvests envars, deep-copies the apps array, dispatches to host/pfexec/server. |
| `pmix_client_connect.c` | `PMIx_Connect[_nb]`, `PMIx_Disconnect[_nb]` | Collective connect; round-trips job data for all participating nspaces. |
| `pmix_client_group.c` | `PMIx_Group_construct/destruct/invite/join/leave[_nb]` | Largest file; invite/accept handshake via events; leader-failure watches. |
| `pmix_client_fabric.c` | `PMIx_Fabric_construct/register/update/deregister[_nb]` | Results written directly into the caller's `pmix_fabric_t`. |
| `pmix_client_topology.c` | `PMIx_Load_topology`, `PMIx_Parse_cpuset_string`, `PMIx_Get_cpuset`, `PMIx_Get_relative_locality`, `PMIx_Compute_distances[_nb]` | Mostly synchronous hwloc pass-throughs. |
| `pmix_client_resolve.c` | `PMIx_Resolve_peers`, `PMIx_Resolve_nodes` | Host-query → local-compute → server round-trip, with local fallback. |
| `pmix_client_convert.c` | `pmix_client_convert_group_procs`, `pmix_client_proc_is_included` | Internal helpers: expand PMIx-group nspaces into real members. |
| `pmix_client_ops.h` | — | Declares `pmix_client_globals` and the few cross-file helpers. |

## Client global state (`pmix_client_ops.h`)

`pmix_client_globals` (defined in `pmix_client.c`, declared in the header)
is the client's singleton state, initialized with `*_STATIC_INIT` macros
and torn down in `PMIx_Finalize`. The load-bearing fields:

- **`myserver`** (`pmix_peer_t *`) — the peer object for the local server;
  the target of every `PMIX_PTL_SEND_RECV`, `PMIX_BFROPS_PACK`, and
  server-side `PMIX_GDS_*` call. Its `sd < 0` on the singleton path.
- **`singleton`** and **`local_iof`** — **two independent booleans that
  must not be conflated.** `singleton` means "no server to talk to";
  `local_iof` means "this process constructed the server-side IOF lists."
  Finalize must tear the IOF lists down based on `local_iof`, never on
  `singleton`, or it will double-free or leak. See the comment blocks in
  `PMIx_Init`/`PMIx_Finalize`. (A prior latent crash traced to exactly
  this conflation.)
- **`pending_requests`** (`pmix_list_t` of `pmix_cb_t`) — outstanding
  `PMIx_Get` requests awaiting a server reply. Multiple gets for the same
  `nspace:rank` are **coalesced**: only the first sends; the rest are
  appended and satisfied together when the reply arrives (see
  `pmix_client_get.c`).
- **`groups`** (`pmix_list_t` of `pmix_group_t`) — PMIx groups this client
  currently belongs to; consulted by `pmix_client_convert_group_procs` to
  expand group references in collective calls.
- **`peers`** (`pmix_pointer_array_t`) — cached peer objects for data ops.
- **`iof_stdout` / `iof_stderr`** — the two static IOF sinks for forwarded
  output.
- the many `*_output` / `*_verbose` pairs — per-subsystem
  `pmix_output_verbose` channels, so debug output for get/connect/fence/
  pub/spawn/event/iof/group/base can be enabled independently.

Cross-file helpers declared here: `pmix_parse_localquery`,
`pmix_client_convert_group_procs`, `pmix_client_proc_is_included`, and
`pmix_client_group_cleanup` (drains leader-watch trackers still active at
finalize, after the progress thread has stopped).

## Invariants and gotchas

- **`PMIx_Init` returns `PMIX_ERR_UNREACH`, not `PMIX_SUCCESS`, on the
  singleton path.** A process with no server still initializes; callers
  must treat `PMIX_ERR_UNREACH` from init as "up but unconnected," not as
  failure.
- **A zero-byte recv buffer means the connection was lost.** Every PTL
  recv callback must check `PMIX_BUFFER_IS_EMPTY(buf)` before unpacking.
  Note `PMIX_BUFFER_IS_EMPTY` dereferences its argument — a NULL `buf`
  must be guarded separately (some callbacks do, some do not — be
  consistent when you touch one).
- **Realm directives change where data comes from.** In `PMIx_Get`, the
  `PMIX_NODE_INFO` / `PMIX_APP_INFO` / `PMIX_SESSION_INFO` directives and
  the hostname/nodeid/appnum/sessionid qualifiers redirect the lookup to a
  different realm; the parser in `process_request`/`get_data` augments the
  info array (setting `cb->infocopy = true` so the *new* array is freed,
  never the caller's). A **NULL key is legal** ("all data for this proc"),
  so realm code must not assume `cb->key` is non-NULL.
- **Local GDS fetches on the progress thread must set `PMIX_OPTIONAL`.**
  Otherwise the GDS may try to up-call the server, which deadlocks the
  progress thread. `pmix_client_resolve.c` relies on this.
- **`cb->infocopy` governs info-array ownership**; `fcd->copied` governs
  the spawn caddy's info/apps ownership; the group tracker's fields and
  the fabric `cb->fabric` are pointer-shared with the caller. When you add
  an allocation to a caddy, make sure the matching destructor frees it and
  the ownership flag is actually set.
- **`PMIX_RELEASE(x)` nulls `x`.** After releasing a caddy you cannot
  touch it (or dereference it in a later fall-through). The blocking paths
  in particular must `return` immediately after releasing on an error, not
  fall into a `PMIX_WAIT_THREAD`.
- **Some entry points run on the caller's thread by design**, doing their
  pre-send work (validation, group expansion, GDS reads) before handing
  off to the PTL/progress thread. Group invite/join issue events from the
  caller thread precisely because the blocking event APIs would deadlock
  on the progress thread. The client's `groups` list is also mutated from
  the caller thread in some paths and the progress thread in others; treat
  concurrent multithreaded use of these APIs with suspicion.
- **Wire compatibility.** These files pack/unpack command messages;
  the order and content of packed fields is ABI across versions. Append
  only at the end, tolerate `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` on
  trailing reads (older peers), and never reorder — see the top-level
  "Version Interoperability" rules.

## Building

`src/client` compiles straight into `libpmix` via `Makefile.include`
(which appends to the top-level `sources`/`headers` lists — there is no
`Makefile.am` here). Nothing is conditionally compiled, so a change takes
effect with a plain top-level `make` from an already-configured tree — see
the top-level guide's "Test-building your changes." You do **not** need
`autogen.pl`/`configure` unless you add or remove a source file (adding
one to `Makefile.include` then needs a `make`, which regenerates the
top-level `Makefile`). After a build, smoke-test with `make check` in
`test/` and `./simptest` in `test/simple/`. Do not diagnose functional
failures against an `--enable-test-build` tree — its shimmed
`pcompress`/`psec` components are non-functional (see the top-level guide).

## When modifying code here

- **Match the surrounding pattern exactly.** Pick the nearest sibling
  function that already does what you need (a blocking wrapper, a recv
  callback, a role-branched `_nb`) and mirror its caddy allocation,
  ownership flags, error-path cleanup, and release/wakeup discipline. The
  files are internally consistent; divergence is usually a bug.
- **Walk every early-return path.** For each `return`/`goto` in a `_nb`
  function or recv callback, confirm the caddy and any packed buffer are
  released exactly once and that the correct completion (callback for
  non-blocking, wakeup for blocking) happens on that path — including the
  `PMIX_PTL_SEND_RECV`-failed path.
- **Prefer a new attribute to a new API**, and if you must add an API,
  give it the `pmix_info_t info[], size_t ninfo` pair (top-level "Role of
  Attributes"). Deprecate, never alter, an existing signature.
- **Keep it warning-free and portable** under `--enable-devel-check`; use
  the `__pmix_attribute_*__` / `PMIX_HIDE_UNUSED_PARAMS` wrappers rather
  than bare GCC attributes.
