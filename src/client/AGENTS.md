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
- **`grouplock`** (`pmix_mutex_t`) — guards `groups` (below). The one
  piece of client state that is genuinely shared between the caller's
  thread and the progress thread; see the rules under
  [Invariants](#invariants-and-gotchas).
- **`pending_requests`** (`pmix_list_t` of `pmix_cb_t`) — outstanding
  `PMIx_Get` requests awaiting a server reply. Multiple gets for the same
  `nspace:rank` are **coalesced**: only the first sends; the rest are
  appended and satisfied together when the reply arrives (see
  `pmix_client_get.c`).
- **`groups`** (`pmix_list_t` of `pmix_group_t`) — PMIx groups this client
  currently belongs to; consulted by `pmix_client_convert_group_procs` to
  expand group references in collective calls. **Always hold `grouplock`
  across a traversal and across any use of a group's `members`** — unlike
  `pending_requests`, this list is not confined to the progress thread.
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
- **`PMIX_RELEASE` dereferences its argument — it is *not* a no-op on
  `NULL`.** (`pmix_obj_update()` reads the reference count straight off
  the pointer.) Any caddy field that is only sometimes populated —
  `cb->lg`, `cb->info`, a tracker's `members` — must be `NULL`-checked
  before release. This is not a defensive nicety; it is the difference
  between a working call and a segfault, and it is exactly how
  `PMIx_Get_nb(NULL, PMIX_PROCID, …)` used to crash.
- **Anything a caddy hands to the request parser must be initialized
  first.** `process_request()` both reads and writes through the `val`
  pointer it is given (`PMIX_GET_STATIC_VALUES` dereferences it to find
  the caller's storage), so passing an uninitialized local means the
  parser's own "did they provide storage?" test reads the stack.
- **Every server round-trip must gate on being connected.** The pattern
  is `pmix_client_globals.singleton ||
  !pmix_atomic_check_bool(&pmix_globals.connected)`. A helper that packs a
  command without that check (`refresh_cache` did) will happily build a
  message for a peer that was never given a transport.
- **`PMIX_PTL_SEND_RECV` only fails when the peer is already
  `finalized`, and in that case it has *not* taken the buffer.** So the
  send site owns the message on the failure path and must `PMIX_RELEASE`
  it — but must *not* release it on success, where the transport frees it.
  Several sites in this directory leaked the message here.
- **A `PMIX_WAIT_THREAD` after a call that might not have fired its
  callback is a hang.** `PMIx_Notify_event` and
  `PMIx_Register_event_handler` invoke the completion callback only when
  they return `PMIX_SUCCESS`; waiting on the lock unconditionally (as the
  group invite path did) blocks forever on any error. Guard every wait
  with `if (PMIX_SUCCESS == rc)`.
- **A zero-byte recv buffer means the connection was lost.** Every PTL
  recv callback must check `PMIX_BUFFER_IS_EMPTY(buf)` before unpacking.
  Note `PMIX_BUFFER_IS_EMPTY` dereferences its argument — a NULL `buf`
  must be guarded separately (some callbacks do, some do not — be
  consistent when you touch one).
- **A recv callback that gives up must still complete every waiter.**
  The pending-request list in `pmix_client_get.c` is shared: the caddy the
  reply was addressed to is itself on that list, and other gets for the
  same `nspace:rank` were coalesced behind it. Releasing the caddy and
  returning (as the status-unpack failure path did) both strands a
  blocking `PMIx_Get` on a lock nothing will ever wake and frees the
  object it is still waiting on. Fall into the delivery loop instead.
- **The `_nb` entry points use `cbfunc == NULL` to mean "the caddy is in
  `cbdata`".** That is an internal convention of their blocking wrappers
  (`PMIx_Fabric_register_nb`, `PMIx_Fabric_update_nb`), but these are
  *public* APIs, so user code can call them with both arguments NULL and
  reach a `cb = (pmix_cb_t *) cbdata` that yields NULL. Validate the pair
  up front. Where the API genuinely has no other completion route — as in
  `PMIx_Compute_distances_nb`, whose reply handler calls the callback
  unconditionally — reject a NULL `cbfunc` outright.
- **An out-parameter the caller may hand you as NULL is a crash, not a
  courtesy.** `PMIx_Get` writes `*val` on both the success and the failure
  path, so it has to reject `val == NULL` before doing anything else.
  Define every OUT parameter *before* the first thing that can fail, so
  the caller can read it on an error return too.
- **The groups list is the one piece of client state that is not
  single-threaded.** `pmix_client_globals.groups` is touched by both the
  progress thread and the caller's thread, so
  `pmix_client_globals.grouplock` guards it — the list spine *and* the
  `members` array of every group on it, because the `PMIX_GROUP_LEFT`
  handler shifts that array down underneath a reader. Two rules:
  **(1)** never hold it across anything that waits on the progress
  thread — a blocking `PMIx_Notify_event`, say — because the handler on
  the other side wants the same lock; copy out what you need and release
  it first, as `PMIx_Group_destruct_nb` and `PMIx_Group_leave_nb` do.
  **(2)** it is a plain, non-recursive mutex, so nothing called while
  holding it may take it again. Do allocations and anything that can
  fail before acquiring, so error paths do not have to unwind through
  it.
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
- **An optional trailing field must be packed through a scratch buffer,
  and a pack failure on one is not necessarily an error.** The receiver
  reads these with a trailing unpack and treats
  `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` as "the peer sent none", so
  *omitting* one is a supported wire state — and sometimes the only
  possible one. A `PMIX_DATA_ARRAY` of `PMIX_INFO` (which is what both
  of `PMIx_Connect_nb`'s trailing blobs are) **cannot be packed for a
  pre-v4 peer at all**: its bfrops has no registered array-element
  handler for `PMIX_INFO`. Two consequences:
  - Packing straight into the message is wrong even when you ignore the
    status, because the failure is partial — the element type and count
    are written before the elements fail — so the wire gets a
    half-encoded field the receiver has to trip over and skip.
  - Turning that failure into a hard error breaks cross-version connect
    outright. This is not hypothetical: it is what the
    `run-xversion` v3.2 job caught (`--test-resolve-peers`, which drives
    a multi-nspace connect, hung).

  `append_optional()` in `pmix_client_connect.c` is the pattern: pack to
  a scratch `pmix_buffer_t`, `PMIX_BFROPS_COPY_PAYLOAD` it onto the
  message only if that succeeded, and report success either way. Reserve
  a hard error for failing to append something you *did* manage to pack.
- **Reproduce cross-version failures locally rather than guessing.** The
  `run-xversion` workflow just runs `test/pmix_test` from one build
  against `test/pmix_client` from the other, so a checkout of the release
  branch beside your tree reproduces any of its cases directly, e.g.
  `…/v3.2/test/pmix_test -n 5 --test-resolve-peers --ns-dist "1:2:2" -e
  …/master/test/pmix_client`. Run it both directions — the two are
  separate CI steps and fail independently.
- **Tolerating a short read is not the same as reporting one.** A recv
  callback that treats `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` as
  acceptable — the server had nothing more to send — must then *reset*
  `rc`, because that value is usually what gets handed to the user's
  callback. `frecv` and `direcv` both tested for it and then fell
  through to the completion with it still set, so a fabric register or a
  distance computation the server said had succeeded was reported to the
  application as an unpack error.
- **A blocking API that discards the server's reply always returns
  success.** Two entry points used a generic "just wake the caller"
  recv callback that throws the buffer away — `PMIx_Commit` (fixed in
  the first pass) and `PMIx_Abort` (fixed in the second). The server
  packs a status for both. If you add a command whose reply carries a
  status, give it a callback that unpacks one; `unpack_ack()` in
  `pmix_client.c` is the shared helper.
- **`pmix_output_verbose()` is a function call, so its arguments are
  always evaluated** — including when the channel is off, and including
  when the call sits above the parameter checks that were supposed to
  make them safe. `PMIx_Put` printed `key` and dereferenced `val->type`
  before validating either. Put the verbose line *after* the checks.
- **`PMIX_BFROPS_PACK` rejects a NULL source before the type-specific
  packer sees it.** `pmix_bfrops_base_pack()` fails `NULL == src && 0 <
  num_vals` with `PMIX_ERR_BAD_PARAM`, so a packer that handles NULL
  (`pmix_hwloc_pack_topology`, `pmix_hwloc_pack_cpuset`) can never be
  reached that way. To send "I have none of this, use yours", pack an
  *empty object* — a zeroed `pmix_topology_t`/`pmix_cpuset_t` — not a
  NULL pointer. `PMIx_Compute_distances_nb` had exactly this wrong and
  its entire ask-the-server fallback failed with BAD_PARAM instead of
  sending.
- **A recv callback that fills a caller-owned object must free what was
  already in it.** `PMIx_Fabric_update` refreshes a `pmix_fabric_t` that
  `PMIx_Fabric_register` already populated, and both `frecv` and `fcb`
  simply `PMIX_INFO_CREATE`d over `fabric->info`, orphaning the previous
  array on every update.
- **Ownership flags govern the caddy, not everything the caddy touched.**
  `cbdes()` does *not* release `cb->value`, so any path that copies out
  of it rather than handing it over owns the original. `PMIx_Get` under
  `PMIX_GET_STATIC_VALUES` xfers into the caller's storage and used to
  walk away from the library's copy.

## Testing

There are two tiers, and the split is forced by the architecture: almost
every function here either answers locally or round-trips to a server, so
a test with no server can only reach the first half.

**Tier 1 — `test/unit/client_api.c` (in `make check`).** Runs as a
singleton. Covers what a client decides by itself: the `PMIx_Get`
shortcuts that `process_request` answers outright, the pointer/static
value forms, and the parameter validation on `PMIx_Get[_nb]`,
`PMIx_Put`, `PMIx_Spawn[_nb]`, `PMIx_Compute_distances[_nb]`,
`PMIx_Resolve_peers`/`PMIx_Resolve_nodes`, the group out-parameters and
the fabric APIs. Every case in it is a regression for a specific defect
listed below; several of them segfaulted before. See
[`test/unit/AGENTS.md`](../../test/unit/AGENTS.md).

> **Know what a singleton can and cannot reach.** Most entry points here
> check `initialized` → `connected` → `progress_thread_stopped` *before*
> validating their arguments, so with no server the state checks answer
> first and the parameter checks are unreachable. That is why the
> coverage above is exactly the set of APIs that validate before (or
> without) gating on `connected` — `PMIx_Get`, `PMIx_Put`,
> `PMIx_Compute_distances`, the resolve pair, the fabric calls, and
> `PMIx_Spawn`, whose apps check deliberately precedes its connected
> check. For the group APIs only the OUT-parameter defaults are
> testable, because those are established ahead of every check. Don't
> "fix" a group parameter check by hoisting it above the state checks
> just to make it testable — the ordering is the convention here.

**Tier 1a — `test/unit/run_grpinviteothers.pl` (in `make check`).**
Drives `examples/group_invite_others` through `test/simple/simptest`:
four clients, with rank 0 inviting ranks 1..3 and *not* joining. That
shape is what distinguishes it from `run_grpinvite.pl` — see the
`invite_setup()` entry in the August 2026 defect list. The test fails
loudly (an aborted construct) rather than hanging when the invitation
resolves early.

**Tier 2 — `contrib/dockerswarm/run-client-tests.sh`.** Drives the
`examples/` clients through PRRTE across the ten-container swarm, so the
ranks sit behind *different* PMIx servers. That is the only way to
exercise the real subject of this directory: a `PMIx_Get` whose answer is
on another node takes the whole
pack → `PMIX_PTL_SEND_RECV` → `_getnb_cbfunc` → GDS-store → deliver path,
including the pending-request coalescing; `PMIx_Fence` has something to
modex; publish/lookup resolves up through the daemons; `PMIx_Spawn` plus
`PMIx_Connect` exercise the multi-nspace job-info exchange. Requires
`./build.sh` first, so the clients link the PMIx you are reviewing rather
than one baked into the image.

Do not diagnose functional failures against an `--enable-test-build`
tree — its shimmed `pcompress`/`psec` components are non-functional (see
the top-level guide).

## Defects found in the July 2026 review

Recorded so the same ground is not re-covered, and because each one names
a pattern worth checking whenever you touch a sibling function. All are
fixed on `topic/client-review`; the first three are the ones with
regression coverage in `test/unit/client_api.c`.

*Crashes:*

- `gcbfn()` released `cb->lg` unconditionally, but the caddy built for a
  locally-answered request never has one — so **every**
  `PMIx_Get_nb(NULL, PMIX_PROCID, …)` (and the `PMIX_VERSION_NUMERIC` and
  `PMIX_RANK` shortcuts) segfaulted.
- `PMIx_Get_nb` passed an uninitialized `val` to `process_request`, which
  dereferences it under `PMIX_GET_STATIC_VALUES`.
- `refresh_cache()` dereferenced the caller's `proc`, which `PMIx_Get`
  explicitly allows to be NULL, and packed a message with no server to
  send it to.
- `PMIx_Get` wrote through `val` without checking it for NULL.
- `PMIx_Spawn_nb` called `pmix_basename(aptr->cmd)` on the branch where
  the caller supplied `argv` but no `cmd` — a documented-legal
  combination, and `pmix_basename(NULL)` returns NULL straight into a
  `strcmp`.
- `PMIx_Spawn` copied `cb->pname.nspace` without checking it, which the
  server-role path leaves NULL when the spawn fails.
- The fabric `_nb` entry points and `PMIx_Compute_distances_nb`
  dereferenced a NULL caddy / callback (see the invariant above).

*Hangs and use-after-free:*

- `_getnb_cbfunc()` released the caddy and returned when the status
  unpack failed, stranding blocked callers on a freed lock.
- `invite_setup()` waited on a lock unconditionally after
  `PMIx_Register_event_handler` and after `PMIx_Notify_event`.

*Leaks and lifetime:*

- `PMIX_PTL_SEND_RECV` failure paths leaked the message in `PMIx_Init`,
  `PMIx_Finalize`, `PMIx_Abort`, `_commitfn`, `get_data`,
  `refresh_cache`, `fallback_to_next_gds` and both `PMIx_Resolve_*`.
- `get_data()` left `cb2` undestructed when a GDS fetch failed.
- `PMIx_Connect_nb` leaked `cb2` on the info-list error paths, and
  constructed a `pmix_buffer_t pbkt` it never used.
- `respeer()`/`resnode()` allocated `cb->info` without setting
  `cb->infocopy`, so the caddy destructor never freed it.
- `PMIx_Fabric_deregister` freed `fabric->info` but left the pointer
  dangling, so a second deregister freed it again.
- `construct_cbfunc()` adopted `darray` from `PMIx_Info_list_convert`
  without checking the return, handing the tracker an uninitialized
  pointer for its destructor to free.

*Correctness:*

- `pmix_client_convert_group_procs()` silently dropped a proc whose group
  rank was past the end of the membership, producing a short participant
  list that failed much later as `PMIX_ERR_NOT_A_MEMBER` or as a
  collective that never completed. It now returns `PMIX_ERR_NOT_FOUND`.
  Its wildcard branch also `continue`d the group loop where it meant to
  `break`.
- Several GDS fetches took `pmix_list_remove_first()` without a NULL
  check on paths where the siblings around them have one.

*Concurrency and API contract (second pass):*

- **`pmix_client_globals.groups` had no lock**, but is not confined to
  one thread: the progress thread appends to it (`add_group` from
  `construct_cbfunc`, and the `PMIX_GROUP_CONSTRUCT_COMPLETE` handler in
  `src/event/pmix_event_notification.c`) and *edits a live membership
  array in place* (the `PMIX_GROUP_LEFT` handler shifts a departed proc
  out), while the caller's thread reads and removes from it in
  `PMIx_Group_leave_nb`, `PMIx_Group_destruct_nb`, and every collective
  that expands a group reference through
  `pmix_client_convert_group_procs()`. It is now guarded by
  `pmix_client_globals.grouplock` — see the rules below.
- `_commitfn()` ignored the server's reply. The generic `wait_cbfunc` it
  used discards the buffer, so the status the server returns for
  `PMIX_COMMIT_CMD` was thrown away and every commit reported success —
  including one the server rejected, which is exactly the case a caller
  needs to hear about. It now has its own `commit_cbfunc` that unpacks
  the reply.
- `PMIx_Group_join` declared `results`/`nresults` unused, leaving the
  caller's pointers as whatever was on the stack. Both are now defined
  before anything can fail, and a NULL for either is honored (the man
  page documents them as optional). See the known gap below for why they
  come back empty.

*Known, left alone (deliberately):*

- `resolve_peers()` sets `proc.rank = PMIX_RANK_WILDCARD` for a pre-v3.2
  server and then unconditionally resets it to `PMIX_RANK_UNDEF` two
  lines later, so the legacy branch is dead. `try_fetch()` retries an
  UNDEF rank as WILDCARD, which is why the path still works. Not changed
  without a pre-v3.2 server to test against; see the comment in the code.
- **The library's own event handlers can be silently suppressed by the
  application — [openpmix#4059][i4059].** Both multi-code registrations
  in `pmix_client_group.c` (`invite_setup`'s `invite_handler` and
  `setup_leader_watch`'s watch) land in the **multi-code** category,
  and the chain visits `first` → `single-code` → `multi-code` →
  `default` → `last`, so an application handler registered for a
  *single* one of those codes runs first and ends the chain with the
  ordinary `PMIX_EVENT_ACTION_COMPLETE`. Consequences you will meet
  here: `PMIx_Group_invite` can hang forever, the invitee-side
  leader-failure watch never fires, and `PMIx_Group_join` cannot
  complete at the point its man page documents (so it returns no
  results). Read the issue before touching any of it — it carries the
  reproducers, the sweep of every internal registration in the tree,
  what was ruled out (not delivery, not a registration race, not a
  filter mismatch), and the options for a fix. Until then an
  application takes the membership from its own
  `PMIX_GROUP_CONSTRUCT_COMPLETE` handler.

  Do **not** re-derive this from the code: an earlier comment in this
  file blamed the construct event "not being guaranteed to reach the
  acceptor", which sent readers after a protocol problem that does not
  exist. The event arrives reliably; the library just cannot see it.

[i4059]: https://github.com/openpmix/openpmix/issues/4059

## Defects found in the August 2026 review (second sweep)

A second pass over the same directory. Grouped the same way; everything
here is fixed on `topic/client-review-2` unless the entry says otherwise.
The regression cases are in `test/unit/client_api.c` and (for the group
one) `test/unit/run_grpinviteothers.pl` plus the swarm suite.

*Crashes on inputs the API permits:*

- `PMIx_Put` dereferenced `val->type` and printed `key` in a
  `pmix_output_verbose()` that sat **above** the validation meant to
  catch them — and never checked `val` at all, so `_putfn` dereferenced
  it again. Both NULLs were segfaults.
- `PMIx_Compute_distances`, `PMIx_Resolve_peers` and `PMIx_Resolve_nodes`
  each set their OUT parameters to a default on the way in without
  checking them, so a NULL for any of them was a store to address zero.
- `PMIx_Group_construct` and `PMIx_Group_invite[_nb]` wrote `*results` /
  `*nresults` unchecked, though the man page documents both as optional
  (this is the same defect fixed in `PMIx_Group_join` last pass, in the
  three siblings that were missed).
- `PMIx_Group_join[_nb]` never validated `grp`, and `setup_leader_watch()`
  hands it straight to `strdup()`.
- `PMIx_Spawn[_nb]` never validated `apps`/`napps` before walking the
  array, so `apps == NULL` with `napps > 0` dereferenced NULL. An `argv`
  whose first element is NULL reached `strdup()`/`pmix_basename()` the
  same way.
- `PMIx_Fabric_deregister[_nb]` was the one fabric entry point with no
  `initialized` gate, yet it reaches `PMIX_PEER_IS_SCHEDULER(mypeer)` —
  and `mypeer` does not exist until `PMIx_Init` has run.
  `PMIx_Fabric_construct(NULL)` dereferenced its argument.
- `PMIx_Init` took `pmix_list_remove_first()` for the debugger-stop key
  without a NULL check and then dereferenced `kv->value` (and leaked
  `kv` on the unrecognized-type return).
- `process_request()` `strdup`'d a `PMIX_HOSTNAME` qualifier's string
  without checking its type or the string itself.

*Functional defects:*

- **`PMIx_Compute_distances_nb` could never ask the server.** The
  fallback deliberately passes a NULL topology (and, on one path, a NULL
  cpuset) to mean "use your own" — which the server explicitly supports
  — but `pmix_bfrops_base_pack()` rejects a NULL source before the
  hwloc packer that understands it is reached. Every request that could
  not be answered locally therefore returned `PMIX_ERR_BAD_PARAM`
  instead of being sent. Now packs empty objects, and
  `pmix_hwloc_pack_topology()` treats an empty topology the same as the
  NULL it already handled. **No automated coverage** — forcing a client
  whose local hwloc computation fails is not something the test
  environments can arrange; verified against the server handler
  (`pmix_server_device_dists`) by inspection.
- **`PMIX_GROUP_NOTIFY_TERMINATION` was never recorded on a group.**
  `PMIx_Group_construct` scanned the directives and stored the flag on
  its own wrapper tracker; `construct_cbfunc` reads it from the *inner*
  tracker built by `PMIx_Group_construct_nb`, where it was always false.
  Worse, `construct_cbfunc` calls `add_group()` first and `add_group()`
  ignores a repeat request for a group it already holds — so the
  correctly-flagged call that followed from `info_cbfunc` could not
  correct it. `PMIx_Group_destruct` therefore never re-applied the
  policy for anybody. The scan now happens in `_nb`, on the tracker that
  is actually consulted.
- **`invite_setup()` resolved an invitation one answer early whenever
  the leader was not itself an invitee.** It credited the leader's own
  answer unconditionally (`nanswered = 1`) but only set the matching
  per-member flag if it found itself in the `procs` array. A leader
  inviting only the others therefore started one ahead of the
  membership: the last accept arrived after the decision, was recorded
  as a non-responder, and — the default construct being
  all-or-nothing — aborted the whole thing. Covered by
  `examples/group_invite_others.c`.
- `PMIx_Abort` discarded the server's reply and always returned
  `PMIX_SUCCESS`, so a host that refused the abort (or does not support
  one) never reached the caller. Same shape as the `_commitfn` defect
  from the first pass.

*Leaks and lifetime:*

- `PMIx_Fabric_update` orphaned the fabric's previous info array —
  `frecv()` and `fcb()` both `PMIX_INFO_CREATE`d straight over
  `fabric->info`, and update is precisely the call made against an
  already-populated fabric.
- `PMIx_Get` under `PMIX_GET_STATIC_VALUES` copied into the caller's
  storage and abandoned `cb->value`, which no destructor frees.
- `resolve_peers()` leaked its `tmp` argv when the aggregate walk
  collected entries but resolved no peers (an nspace whose local-peer
  list is an empty string).

*Correctness, lesser:*

- `frecv()` and `direcv()` tolerated a trailing
  `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` and then reported it to the
  caller as the result of an operation the server had said succeeded.
- `PMIx_Connect_nb` ignored the pack status when appending the
  job-level info array, silently sending a message missing the data the
  server waits for. Every other pack in that function is checked.
  **Simply checking it was wrong too** — see the optional-trailing-field
  invariant above. Both of its trailing blobs now go through
  `append_optional()`, which omits what a peer cannot represent and
  errors only on a genuine failure. The first attempt at this fix broke
  connect against every pre-v4 server; caught by the `run-xversion` v3.2
  job, and the endpoint blob had the same latent exposure even before
  this pass.
- `destruct_cbfunc()` dropped the group from the local list on the
  empty-buffer path but not on the NULL-buffer path.
- `PMIx_Spawn_nb`'s "find the end sentinel" loop tested `m < SIZE_MAX`
  *after* dereferencing `aptr->info[m]`, so the guard below it was dead.
- `PMIx_Group_join_nb` carried a `PMIX_TIMEOUT` loop that recognized the
  directive and did nothing with it. Removed, and the unused parameters
  named as such — join takes no directives today.

*Known, left alone (deliberately):*

- **`PMIx_Group_invite_nb` never announces the outcome, and leaks its
  tracker and event-handler registration.** `invite_announce()` and
  `invite_teardown()` are reached only from the blocking
  `PMIx_Group_invite`. The non-blocking form resolves the invitation
  (`invite_wake` fires the caller's callback) and then stops: no
  `PMIX_GROUP_INVITE_FAILED`, no `PMIX_GROUP_CONSTRUCT_COMPLETE`, no
  `PMIX_GROUP_CONSTRUCT_ABORT` — so **no group forms** — and nothing
  releases the tracker or deregisters `invite_handler`.

  It is structural rather than an oversight. `invite_wake()` runs on the
  progress thread, and `invite_announce()` is built out of *blocking*
  `PMIx_Notify_event` calls, which would deadlock there; the blocking
  wrapper gets away with it only because it announces on the caller's
  own thread after waking. Fixing it means rewriting `invite_announce()`
  as a chain of non-blocking notifications (the shape `emit_leader_failed`
  already uses from the progress thread), with the teardown hung off the
  last completion. That is a real change to the invite state machine and
  wants review on its own, not a drive-by. Until then, treat
  `PMIx_Group_invite_nb` as unimplemented.

  **Distinct from [openpmix#4059][i4059], but landing on the same
  code.** That issue is about the library's handlers being suppressed;
  this is the non-blocking path never running the announcement at all,
  and it would still be broken with #4059 fixed. Whoever reworks this
  machinery will meet both, and the same rewrite — announcement driven
  by non-blocking notifications — is what #4059's follow-on behavior
  change (completing a pending join from the construct event) also
  needs. Noted on that issue.
- The two `resolve_peers()` items and the handler-suppression problem
  from the first pass are unchanged — see the previous section.

## Coding conventions specific to this directory

- **Mark the exceptional branch.** Error checks, parameter validation,
  allocation failures and the "should never happen" guards are wrapped
  in `PMIX_UNLIKELY()` (from
  [`src/include/pmix_prefetch.h`](../include/pmix_prefetch.h), which
  every file here includes explicitly). This is the idiom the rest of
  the tree uses; match it when you add a check. `PMIX_UNLIKELY` is for
  branches that really are rare — do **not** put it on ordinary control
  flow such as `NULL == proc` in `process_request()` (a documented,
  common way to call `PMIx_Get`) or `NULL == tp` in
  `PMIx_Compute_distances_nb` (the usual case, meaning "use mine").

## Building

`src/client` compiles straight into `libpmix` via `Makefile.include`
(which appends to the top-level `sources`/`headers` lists — there is no
`Makefile.am` here). Nothing is conditionally compiled, so a change takes
effect with a plain top-level `make` from an already-configured tree — see
the top-level guide's "Test-building your changes." You do **not** need
`autogen.pl`/`configure` unless you add or remove a source file (adding
one to `Makefile.include` then needs a `make`, which regenerates the
top-level `Makefile`). After a build, smoke-test with `make check` in
`test/` and `./simptest` in `test/simple/`, then see
[Testing](#testing) above for the two tiers that actually cover this
directory.

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
