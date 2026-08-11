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
`pmix_client_convert_group_procs`, and `pmix_client_proc_is_included`.
(There used to be a `pmix_client_group_cleanup` here to drain
leader-watch trackers still live at finalize. The observer registry owns
those trackers now and releases them when the event lists are
destructed, so the private survivor list and its finalize hook are
gone — see [openpmix#4059][i4059].)

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

  **Which role makes those failure paths reachable is worth knowing
  before you decide one is dead code.** The peer these sites send to is
  `pmix_client_globals.myserver`, and *nothing in the client role ever
  sets `myserver->finalized`* — only `mypeer->finalized` is set, by
  `PMIx_Finalize`. The paths become live because the **server and tool
  roles alias the two**: `src/server/pmix_server.c` and two paths in
  `src/tool/pmix_tool.c` assign `pmix_client_globals.myserver =
  pmix_globals.mypeer`, so once `PMIx_server_finalize` /
  `PMIx_tool_finalize` marks that object finalized, every send from this
  directory fails. Keep the cleanup on those branches: it is not
  defensive padding, it is the self-served case.

  Note also the *other* way a send never completes, which the failure
  return does **not** cover: `pmix_ptl_base_send_recv()` drops the
  message and returns without ever invoking the recv callback when the
  peer has `sd < 0` or a NULL `info`/`nptr`. `PMIX_PTL_SEND_RECV` has
  already reported `PMIX_SUCCESS` by then, so a blocking caller waits on
  a lock nothing will wake. That is why `PMIx_Finalize` arms a timer
  around its wait rather than trusting the reply.
- **A `PMIX_WAIT_THREAD` after a call that might not have fired its
  callback is a hang.** `PMIx_Notify_event` and
  `PMIx_Register_event_handler` invoke the completion callback only when
  they return `PMIX_SUCCESS`; waiting on the lock unconditionally (as the
  group invite path did) blocks forever on any error. Guard every wait
  with `if (PMIX_SUCCESS == rc)`.
- **A zero-byte recv buffer means the connection was lost.** Every PTL
  recv callback must check `PMIX_BUFFER_IS_EMPTY(buf)` before unpacking.

  **`buf` itself is never NULL, and the callbacks here disagree about that
  only cosmetically.** Both places the PTL invokes a recv callback pass
  the address of a real stack `pmix_buffer_t` — see
  [`ptl_base_sendrecv.c`](../mca/ptl/base/ptl_base_sendrecv.c), where the
  lost-connection sweep constructs an empty buffer (setting `buf.type` so
  a zero-byte unpack does not error) and hands `&buf` to every posted
  recv, and the normal path does the same around `PMIX_LOAD_BUFFER`. So
  the `NULL == buf` guards scattered through this directory are dead
  code, and the callbacks without one — `frecv`, `direcv`,
  `pmix_client_fence.c`'s `wait_cbfunc`, `pmix_client_notify_recv`,
  `client_iof_handler` — are correct as written.

  This entry used to say the two styles should be made "consistent"
  without saying which way, which is worse than useless: the empty-buffer
  check is the one that carries meaning and must never be removed, while
  the NULL check is merely redundant. Do not add NULL guards to the
  remaining callbacks on the theory that it is a real hazard, and do not
  drop an empty-buffer check on the theory that the NULL check subsumes
  it.
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
- **Put a `pmix_output_verbose()` call *after* the parameter checks, not
  above them.** It is a macro (`src/util/pmix_output.h`) that tests the
  stream's level before evaluating anything, so its arguments cost
  nothing while the channel is off — but they *are* evaluated once it is
  on. A verbose line sitting above the validation therefore turns a
  clean `PMIX_ERR_BAD_PARAM` into a segfault for anyone who raises the
  verbosity, which is the worst possible shape: the crash appears only
  while being debugged. `PMIx_Put` printed `key` and dereferenced
  `val->type` before validating either.

  Note this cuts the other way too: because the arguments are skipped
  when the channel is off, there is no reason to hand-guard a verbose
  call with a verbosity test of your own. That is what the macro does,
  and `pmix_output_get_verbosity()` is a real function call, so the
  "optimization" is slower than the thing it replaces.
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
`PMIx_Resolve_peers`/`PMIx_Resolve_nodes`, the group out-parameters, the
fabric APIs and the topology entry points. Every case in it is a
regression for a specific defect listed below; several of them
segfaulted before. See [`test/unit/AGENTS.md`](../../test/unit/AGENTS.md).

> **Know what a singleton can and cannot reach.** Most entry points here
> check `initialized` → `connected` → `progress_thread_stopped` *before*
> validating their arguments, so with no server the state checks answer
> first and the parameter checks are unreachable. That is why the
> coverage above is exactly the set of APIs that validate before (or
> without) gating on `connected` — `PMIx_Get`, `PMIx_Put`,
> `PMIx_Compute_distances`, the resolve pair, the fabric calls, the
> `pmix_client_topology.c` entry points (which answer out of hwloc and
> never round-trip, so they gate on `initialized` alone), and
> `PMIx_Spawn`, whose apps check deliberately precedes its connected
> check. For the group APIs only the OUT-parameter defaults are
> testable, because those are established ahead of every check. Don't
> "fix" a group parameter check by hoisting it above the state checks
> just to make it testable — the ordering is the convention here.

**Tier 1b — `test/unit/resolve_api.c` (in `make check`).** The one
place in `make check` that reaches the *local computation* behind
`PMIx_Resolve_peers`/`PMIx_Resolve_nodes` rather than just their argument
checks. It comes up as a server with a stub host module — no `query`
entry point, so both APIs fall through their host branch to the
thread-shifted local path — and registers two namespaces on this one
node, one with peers here and one whose `PMIX_LOCAL_PEERS` is explicitly
the empty string. That is the input behind the ninth sweep's findings.
Note the second namespace must be registered with a node-info array and
**no proc map**: the map-driven derivation in `store_map()` replaces a
host-supplied `PMIX_LOCAL_PEERS`, so a proc map would overwrite the value
under test.

**Tier 1c — `test/unit/spawn_api.c` (in `make check`).** The same
stub-host-server trick, applied to `PMIx_Spawn`. It exists because a
singleton *client* cannot reach spawn's argument handling at all —
`PMIx_Spawn_nb` gates on `connected` before it looks at `job_info`, and
the apps copy sits below that gate too, so the only argument check a
singleton reaches is the `apps`/`napps` one that deliberately precedes it
(covered in `client_api.c`). A **server** is let through that gate so it
can hand the request to its own host, so the directive scan and the apps
copy both run and the call then stops at the absent
`pmix_host_server.spawn`. `PMIX_ERR_NOT_SUPPORTED` is therefore the "got
all the way through the argument handling" marker, and every case in the
file asserts it. Use the same shape for anything else in this file whose
input handling sits below a `connected` gate.

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
- **The library's own event handlers could be silently suppressed by the
  application — [openpmix#4059][i4059]. Mostly fixed; read this before
  touching the group event code.** Both registrations here
  (`invite_setup`'s answer counter and `setup_leader_watch`'s watch)
  were ordinary multi-code handlers, and the chain visits `first` →
  `single-code` → `multi-code` → `default` → `last`, so an application
  handler registered for a *single* one of those codes ran first and
  ended the chain with the ordinary `PMIX_EVENT_ACTION_COMPLETE`. That
  hung `PMIx_Group_invite` forever and kept the leader-failure watch
  from ever firing.

  Both are now **internal observers**
  (`pmix_event_register_observer`, see the "Internal observers" section
  of [`src/event/AGENTS.md`](../event/AGENTS.md)): they run ahead of the
  application's chain and cannot be pre-empted. Two rules follow for
  anyone editing them. **If you need the library to see an event here,
  register an observer, not a handler** — a handler is exactly the
  defect. And **an observer must not block**: it runs inline on the
  progress thread, so bookkeeping, `invite_wake()`, a non-blocking
  `PMIx_Notify_event`, and `pmix_event_deregister_observer()` are fine,
  while `PMIX_WAIT_THREAD` and the blocking public APIs are not.

  The follow-ons that work unblocked have since landed too: the
  announcement is now a chain of non-blocking notifications driven from
  the progress thread (which is what makes `PMIx_Group_invite_nb` work
  at all), and `PMIx_Group_join[_nb]` completes when the construct
  resolves, returning the group id and membership. See
  `PMIX_CAP_GROUP_JOIN_COMPLETES`.

  Do **not** re-derive any of this from the code: an earlier comment in
  this file blamed the construct event "not being guaranteed to reach
  the acceptor", which sent readers after a protocol problem that does
  not exist. The event always arrived reliably; the library just could
  not see it.

  Regression coverage: `test/unit/run_grpinvitesuppress.pl` drives
  `examples/group_invite_suppress`, whose leader registers exactly the
  chain-ending `PMIX_GROUP_INVITE_ACCEPTED` handler that used to hang
  it; `test_observer` in `test/unit/event_chain.c` covers the mechanism
  itself.

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

- **`PMIx_Group_invite_nb` and the join completion point are fixed** —
  both were left open here after the second sweep, and both landed with
  the [openpmix#4059][i4059] work.

  `PMIx_Group_invite_nb` used to be inert: it resolved the invitation,
  fired the caller's callback and stopped, announcing nothing, so no
  group formed and it leaked its tracker and registration on every call.
  The cause was structural — `invite_announce()` was a straight line of
  *blocking* `PMIx_Notify_event` calls, so it could only run on the
  blocking form's own thread after it woke, and the `_nb` form has no
  such thread to borrow. It is now `announce_step()`, a chain of
  non-blocking notifications driven from the progress thread with the
  teardown hung off the end, which both forms share. Two things to know
  if you touch it: the chain's position lives on the tracker
  (`astate`/`aidx`/`ainfo`) because nothing else survives between the
  steps, and each notification's info array must outlive the call that
  carries it — which is why it is built on the heap into `cb->ainfo` and
  freed by the completion callback.

  `PMIx_Group_join[_nb]` now completes when the construct resolves, per
  its man page, carrying the group id and membership; the construct
  watch is what completes it. Ownership of the caller's completion is
  decided *before* the acceptance notification goes out, because once
  that is away its own completion can fire at any moment — get that
  ordering wrong and the caller is either completed twice or never.

- The two `resolve_peers()` items are unchanged. The handler-suppression
  problem from the first pass is fixed; see the updated entry in the
  previous section for what that did and did not cover.

## Defects found in the August 2026 review (third sweep)

A third pass, reading every file in the directory again. Everything here
is fixed unless the entry says otherwise. The two topology ones have
regression coverage in `test/unit/client_api.c`; the rest are on paths a
singleton cannot reach (see [Testing](#testing)) and were verified by
inspection against the code that consumes them.

*Type confusion:*

- **`PMIx_Init` announced "ready for debug" to a target that does not
  exist.** The `PMIX_EVENT_CUSTOM_RANGE` directive was loaded straight
  from `pmix_client_globals.myserver` — a `pmix_peer_t *` — as
  `PMIX_PROC`. `PMIx_Value_load()` for `PMIX_PROC` `memcpy`s
  `sizeof(pmix_proc_t)` out of whatever it is handed, and a
  `pmix_peer_t` begins with its `pmix_object_t` header, so the
  notification's target nspace/rank was the object's class pointer and
  reference count reinterpreted as a string and an integer. The event
  went nowhere and no debugger ever saw the `PMIX_READY_FOR_DEBUG`.
  It now loads the server's real process ID from
  `myserver->info->pname`. The general rule: **`PMIX_INFO_LOAD` is a
  `memcpy` behind a type tag, so it cannot catch a mismatched
  argument** — nothing between the call site and the consumer will
  either. `pmix_client_group.c` has this right in every one of its five
  custom-range loads; this was the odd one out.

*Crashes on inputs the API permits:*

- **`PMIx_Load_topology(NULL)` segfaulted.**
  `pmix_hwloc_load_topology()` reads `topo->source` as its very first
  act. Its two siblings — `pmix_hwloc_parse_cpuset_string()` and
  `pmix_hwloc_get_cpuset()` — had already been given exactly this
  screen, with a comment saying they are reachable from a public API
  with whatever the caller passed; load_topology was the one missed.
- **`PMIx_Get_relative_locality(l1, l2, NULL)` stored to address zero.**
  It writes `*loc` on every path that gets past its input checks. Note
  the shape of the reproducer: two *well-formed* locality strings, so
  the computation actually runs to the store. A test that passes
  garbage strings stops at the payload check and proves nothing —
  `test_topology_bad_params()` covers both.
- `_putfn()` gave the kval a raw `malloc()`ed `pmix_value_t` and only
  filled it in on the transfer paths. The compression-failure branch
  (`compress_string` reporting success with a NULL buffer) jumps to
  `done:`, which `PMIX_RELEASE`s the kval — and the kval destructor
  reads `value->type` to decide what to free. It now uses
  `PMIX_VALUE_CREATE`, which zeroes and sets `PMIX_UNDEF`. The rule
  worth carrying: **anything a destructor will inspect has to be valid
  from the moment the object exists**, not from the moment the happy
  path fills it in.
- `PMIx_Spawn_nb` took `PMIX_PARENT_ID` straight to `PMIX_XFER_PROCID`
  without checking that the value really carried a `pmix_proc_t`. Same
  defect as the `PMIX_HOSTNAME` qualifier fixed in `process_request()`
  last pass, in the one other place this directory reads a
  caller-supplied union member by key.

*Correctness:*

- **A client could not use `PMIX_SETUP_APP_ENVARS` at all.** The `pmdl`
  framework that performs the harvest is opened only by the server
  (`pmix_server_init`) and tool (`pmix_tool_init`) roles, so
  `pmix_pmdl_base_harvest_envars()` answers a plain client with
  `PMIX_ERR_INIT` — which means "no programming-model support is open in
  this role", *not* "the library is not initialized".
  `PMIx_Spawn_nb` treated it as fatal and failed the whole spawn, giving
  the caller a reason that pointed nowhere near the cause. Nothing is
  lost by continuing: the directive is carried in the job-level info the
  client is about to send, and whoever launches for it does the harvest
  with its own `pmdl` (see `PMIx_server_setup_application`,
  `src/server/pmix_server_setup.c`). Both harvest sites now tolerate
  `PMIX_ERR_INIT` specifically, and nothing else.

  **This is why the role matters when reading this file.** `PMIx_Spawn_nb`
  is entered by clients, tools, launchers and servers, and which
  frameworks are open differs between them. A branch that is correct for
  the role you happen to be picturing can be dead — or fatal — in another.
- **`PMIx_Spawn_nb` permanently edited the caller's `const pmix_app_t
  apps[]`.** `PMIX_SETUP_APP_ENVARS` was harvested while scanning
  `job_info`, which happens *before* the "sadly, we have to copy the
  apps array" copy — so `PMIx_Setenv()` ran against `apps[m].env`, the
  caller's own array, and the copy then picked the results up. A caller
  that reused its `pmix_app_t` for a second spawn got the envars again
  on top of the ones already added. The harvest is now deferred until
  after the copy and applied to `fcd->apps`. Worth remembering when
  reading this function: the copy is not just a lifetime convenience,
  it is the boundary at which the caller's data stops being ours to
  touch, and anything that runs above it is running on their memory.
  Note this only ever bit the roles that *have* a `pmdl` open — tool,
  launcher, server (PRRTE's `prun` spawning, for instance) — because a
  client harvests nothing; see the entry above.
- `get_data()` did `strdup(kv->value->data.string)` on a `PMIX_HOSTNAME`
  fetched from the datastore without checking the type or the string.
  The datastore is fed by the host, so this is no more ours to assume
  than a caller's qualifier is.
- The spawn reply handler printed the namespace with `"%s"` on a path
  where a tolerated short read leaves it NULL. Passing NULL to a `%s`
  conversion is undefined, not a portable `"(null)"`.

*Noted, not changed:*

- **`test/simple/simptest` cannot host a spawn**, which is why the new
  `examples/spawn_reuse` coverage lives in the swarm suite rather than
  in `make check`. Its `spawn_fn` calls `set_namespace()`, which calls
  `PMIx_server_setup_application()` — a thread-shifting API — and then
  `DEBUG_WAIT_THREAD`s on the result. But `spawn_fn` is the host
  callback and runs *on* the server's progress thread, so the thing it
  is waiting for can never be serviced and the fake launch deadlocks.
  No test drives it, so this has never been noticed. Fixing it means
  making that path asynchronous; until then, do not add a `run_*.pl`
  that spawns.
- `refcb()` overwrites the `PMIX_GDS_STORE_KV` status with the next
  unpack's before anyone reads it, so a store failure while absorbing a
  cache refresh is silently dropped. The loop is driven by the unpack,
  which is the right termination condition; reporting the store error
  would change what `PMIx_Get` returns for a refresh that partially
  succeeded, and that is a behavior decision, not a bug fix.
- `pmix_client_convert_group_procs()` holds `grouplock` across a
  `PMIX_GDS_FETCH_KV` for `PMIX_JOB_SIZE`. That is safe only because
  the hash module answers locally and never up-calls the server. If a
  GDS module is ever added that can up-call from `fetch`, this becomes
  the deadlock the lock's own header comment warns about.

## Defects found in the August 2026 review (fourth sweep — `pmix_client_group.c`)

A pass devoted to the one file the third sweep did not read line by line.
Every finding is the *same defect*, in five places: **a
caller-supplied or wire-supplied `pmix_value_t` union member read without
first checking the type tag.** Read that as the standing hazard of this
file rather than as five separate bugs.

The type tag is the only thing that says which union member is valid, and
nothing downstream will catch a mismatch — `PMIX_INFO_LOAD` and
`PMIx_Value_load` are `memcpy` behind a tag, and the bfrops copy/pack
path dereferences whatever it is handed. So the check has to happen where
the value is first believed.

*Caller-supplied (an ordinary application mistake, so it must be an error
return, not a SIGSEGV):*

- **`construct_msg()` read `PMIX_GROUP_INFO` as a data array unchecked.**
  `info[n].value.data.darray->array` and `->size`, then `iarray[0]`. Hand
  `PMIx_Group_construct` a `PMIX_GROUP_INFO` carrying an integer and
  `darray` is that integer reinterpreted as a pointer, dereferenced
  twice. A NULL or zero-length array failed one step later at
  `iarray[0]`. Covered by `test/unit/run_grpbadinfo.pl`.

*Wire-supplied (another process's or the server's word, so even less ours
to assume):*

- `invite_observer()` took `PMIX_EVENT_AFFECTED_PROC` straight to
  `PMIX_CHECK_PROCID`, which dereferences it.
- `leader_watch_observer()` did the same, and additionally handed a
  `PMIX_GROUP_ID` to `strcmp()` without checking it was a string.
- `construct_cbfunc()` read the group-info blob the *server* sent —
  `grpinfo.value.data.darray->array` — and then, in the nested form,
  `iptr[m].value.data.darray->array` per element, all unchecked. A peer
  that sends anything else here (an older one, or a broken one) crashes
  the client.

*The one that was not in this directory at all:*

- **`pmix_bfrops_base_tma_copy_darray()` dereferenced a NULL source.**
  This is the reason the `construct_msg` fix alone was not enough, and it
  is worth understanding before adding a type check anywhere else: a
  value tagged `PMIX_DATA_ARRAY` whose `darray` is NULL survives the
  check above (it *is* a data array), then reaches `PMIX_INFO_XFER` →
  `value_xfer` → `copy_darray`, which read `src->type` and `src->size`
  with no guard. Any application that builds an info by hand can produce
  that value, and every `PMIX_INFO_XFER` in the tree is exposed to it. It
  now yields the same empty array the function already produces for a
  zero-length source. See [`src/mca/bfrops/AGENTS.md`](../mca/bfrops/AGENTS.md).

  The general lesson: **a type check at the entry point does not protect
  the copy/pack layer behind it.** The `run_grpbadinfo.pl` test was
  written against the entry-point fix and still segfaulted, which is how
  this was found — the fix was verified by reverting each half
  independently and confirming the test fails for each.

*Noted, not changed:*

- `PMIx_Group_leave_nb()` removes the group from
  `pmix_client_globals.groups` *before* issuing the `PMIX_GROUP_LEFT`
  notification, so a notification failure returns an error to a caller
  whose group we have already forgotten. Recovering would mean putting it
  back under the lock; leaving is terminal in practice, so this is
  recorded rather than restructured.
- `invite_setup()` registers the answer observer before it has finished
  populating the tracker (the range info, the timer). If every invitee
  were already dead, a cached `PMIX_PROC_TERMINATED` replay could resolve
  the invitation from the progress thread while the caller's thread is
  still in `invite_setup`. The membership and flags it needs *are* in
  place before registration, so this is narrow, and no invitee can accept
  before the invitation is sent.

## Defects found in the August 2026 review (fifth sweep — the union sweep)

Having established the type-tag hazard as this directory's signature defect,
this pass swept **every** union read in `src/client` for it rather than
reading file by file:

```sh
grep -n "\.value\.data\.\|value->data\." src/client/*.c \
    | grep -vE "PMIX_INFO_LOAD|PMIX_VALUE_LOAD|PMIx_Value_load|= *&|PMIX_INFO_XFER"
```

That is the right tool for this class, and it is worth re-running after any
change here. Two clusters survived the earlier sweeps:

- **`info_cbfunc()` read `PMIX_GROUP_MEMBERSHIP` and `PMIX_GROUP_ID`
  unchecked.** On the join path this array is a copy of the *leader's*
  `PMIX_GROUP_CONSTRUCT_COMPLETE` event (see `join_complete`), so it is
  another process's word — and `add_group()` is handed the results.
  Same class as the fourth sweep's findings, in the one place they missed.
- **The three `PMIX_QUALIFIED_VALUE` unwrap sites in `pmix_client_get.c`**
  reached `kv->value->data.darray->array` and then `iptr[0]` on nothing
  more than the key having matched. They now share a `qualified_value()`
  helper that returns NULL unless the kval really is shaped like one.

  **Be precise about what this one is worth.** `PMIx_Put` screens the
  shape on the way in (`_putfn`), and the GDS rejects a malformed
  qualified value on store — verified, it returns `PMIX_ERR_BAD_PARAM`
  rather than storing it. So this is *not* reachable from a local put,
  and there is no singleton reproducer. It hardens the case where the
  datastore was filled from the server. Defensive, not demonstrated.

*Noted, not changed:*

- `process_request()` takes `PMIX_DATA_SCOPE` as `info[n].value.data.scope`
  with no type check. Unlike the others this cannot crash — it is a scalar
  read out of the union — but a mistyped directive silently produces a
  wrong scope and therefore a wrong answer. Left alone because "reject" and
  "ignore" are both defensible and the choice is a behavior decision.

### The stale-install trap — read this before believing any hand-built reproducer

Most of this sweep was spent chasing a segfault that did not exist.

A probe compiled by hand against this tree — `cc … -Lsrc/.libs -lpmix
-Wl,-rpath,$PWD/src/.libs` — **does not load the library you just built.**
`libpmix`'s `install_name` is the absolute installed path, so dyld resolves
it to `$prefix/lib/libpmix.2.dylib` and ignores both `-L` and the rpath. The
probe silently exercises whatever was last `make install`ed, which in a
review session is code from before your fixes.

The symptom is very convincing: a crash that reproduces every time, in a
function whose source visibly contains the guard that should prevent it, on
a library whose timestamp is newer than the source. Instrumenting the
function does nothing, because the running code is not that code — which is
itself the tell. Confirm with:

```sh
DYLD_PRINT_LIBRARIES=1 ./probe 2>&1 | grep libpmix
```

`make check` and the swarm suite are both immune — the former runs through
libtool wrappers bound to the build tree, and the latter deliberately
insists on the PMIx `build.sh` put in the shared volume (see
[`contrib/dockerswarm/README.md`](../../contrib/dockerswarm/README.md) §12).
The exposure is exactly the ad-hoc probe, which is the thing you reach for
when you want a quick answer. Either `make install` first, or add the case
to `test/unit/client_api.c` and run it under `make check`.

## The sixth sweep found nothing — what it covered, so you need not repeat it

A pass devoted to caddy and buffer lifetime, which the earlier sweeps had
touched only where a specific defect led. It produced **no code changes**,
and that is the useful result: the following are audited and correct as of
this sweep, so a future reviewer can start elsewhere.

- **Every `PMIX_PTL_SEND_RECV` failure path** in the directory (thirteen of
  them) releases both the message and the caddy, and every one that is
  followed by a `PMIX_WAIT_THREAD` returns before reaching it. The two
  fabric sites correctly release the caddy only when they allocated it
  (`NULL != cbfunc`), leaving the blocking wrapper's stack caddy alone.
- **Every recv callback completes its caller exactly once on every exit**,
  including the pack-failure and short-read paths. `destruct_cbfunc` drops
  the group from the local list before any of its early exits, so the two
  failure paths can no longer disagree about whether it is still
  registered.
- **`buf` is never NULL** — see the invariant above, which this sweep
  settled by reading the PTL rather than by inference.

The audit was mechanical and is worth repeating the same way:

```sh
grep -n "PMIX_PTL_SEND_RECV" -A 7 src/client/*.c \
    | grep -E "PMIX_PTL_SEND_RECV|RELEASE|WAIT_THREAD|return|^--"
```

*Noted, not changed:*

- `construct_cbfunc()` hands the caller `relfn` as its release function and
  returns; a user-supplied `pmix_info_cbfunc_t` that never calls it leaks
  the tracker. That is the documented contract for the callback signature,
  not a defect here, but it is the one place in this directory where a
  caller's mistake leaks library memory.

## The seventh sweep, and the things it deliberately left alone

Each file in this pass was audited five times over, through one lens per
pass — memory and lifetime, threading and the PMIx boundary, error paths
and control flow, portability and representation, and caller contracts —
rather than re-read the same way five times.

**A group can outlive its membership, and expanding it yields nothing.**
The `PMIX_GROUP_LEFT` handler in
[`pmix_event_notification.c`](../event/pmix_event_notification.c)
decrements `grp->nmbrs` as each member departs and does *not* drop the
group when the count reaches zero — so a `pmix_group_t` with
`nmbrs == 0` is a reachable state on `pmix_client_globals.groups`.
`pmix_client_convert_group_procs()` expanded a `PMIX_RANK_WILDCARD`
reference to such a group into no participants at all and called that
success, handing back `*outsize == 0` and the **NULL** that
`PMIx_Proc_create(0)` returns for a zero count. `PMIx_Get`'s caller then
`memcpy`'d out of `procs[0]`: a SIGSEGV on a documented-legal call.
Expansion to nothing is now the same `PMIX_ERR_NOT_FOUND` the function
already returns for a group rank past the end of the membership, and
`pmix_client_get.c` additionally requires exactly one proc back rather
than merely not-more-than-one. **Those two are redundant, not two halves
of one fix** — either alone closes the crash — so if you rework one,
check what the other is still doing rather than assuming it is
load-bearing. Regression: `test_get_empty_group()` in
`test/unit/client_api.c`.

Two general points fall out of it. `PMIx_Proc_create(0)` returning NULL
means **any `PMIX_PROC_CREATE` whose count is computed rather than
literal needs the count checked before the array is indexed.** And this
function is the directory's one exception to the "define every OUT
parameter before the first thing that can fail" rule — it sets
`*outprocs`/`*outsize` only on success. All five callers return
immediately on a failure status, so that is safe today; it is the kind
of thing to preserve deliberately rather than discover.

**`pmix_client_connect.c` came through all five with nothing to fix**,
which is worth recording because two of its shapes look alarming and are
not. `PMIx_Connect_nb` hands the same `darray` to `PMIX_INFO_LOAD` and
then destructs both it and the resulting `pmix_info_t` — that is correct,
not a double free: `PMIx_Info_load` routes `PMIX_DATA_ARRAY` through
`pmix_bfrops_base_copy_darray()`, so the info owns a deep copy. (The two
call sites destruct the source in a different order relative to
`append_optional()`; both are fine for the same reason.) And
`wait_cbfunc`'s per-namespace blob loop never leaks `bo`, because
`PMIX_LOAD_BUFFER` *takes* the bytes — it NULLs the source pointer and
zeroes its length — so the `pmix_buffer_t` it fills is what frees them.
Use `PMIX_LOAD_BUFFER_NON_DESTRUCT` if you ever need the byte object to
survive.

**`pmix_client_fence.c` also came through all five with nothing to fix.**
The one shape to understand before touching it is that
`pmix_client_convert_group_procs()` always hands back a **fresh**
`PMIX_PROC_CREATE`d array on success — never the caller's `procs` — so
`PMIx_Fence_nb`'s `created` flag guarding `PMIX_PROC_FREE(rgs, nrg)` is
freeing library memory, not the application's `const pmix_proc_t
procs[]`. All four exits below the conversion honor it. The other thing
worth knowing is that `nrg` is `>= 1` by the time `pack_fence()` runs:
the zero-length expansion is caught one step earlier by
`pmix_client_proc_is_included()`, which cannot find the caller in an
empty array and returns `PMIX_ERR_NOT_A_MEMBER`.

*Noted, not changed:*

- **`PMIx_Fence_nb` blocks when handed a NULL `cbfunc`**, and it is the
  only `_nb` entry point in this directory that does. It allocates its
  caddy, sends, and then takes a `PMIX_WAIT_THREAD` branch of its own —
  so a caller who chose the non-blocking form precisely to stay off a
  blocking path gets a blocking call, and one that deadlocks outright if
  it was issued from an event handler on the progress thread. Note the
  directory has **four** different meanings for a NULL `cbfunc` and this
  is the last: `PMIx_Get_nb`, `PMIx_Compute_distances_nb` and
  `PMIx_Group_invite_nb` reject it with `PMIX_ERR_BAD_PARAM`, the two
  fabric `_nb` entry points read it as "the caddy is in `cbdata`" (see
  the invariant above), the remaining group `_nb` entry points
  (`PMIx_Group_construct_nb`, `PMIx_Group_join_nb`, `PMIx_Group_leave_nb`,
  `PMIx_Group_destruct_nb`) **and `PMIx_Spawn_nb`** accept it as
  fire-and-forget — their reply handlers all test `NULL != cb->cbfunc`
  (`NULL != fcd->spcbfunc` for spawn, in both `wait_cbfunc` and
  `_lclcbfunc`) before completing, and the operation still takes effect —
  and fence blocks. Check which one you are looking at before copying a
  NULL test between them.

  This entry used to name `PMIx_Group_construct_nb` among the rejecters.
  It does not reject: it has never tested `cbfunc` at all, and
  `construct_cbfunc` is written for the NULL case. Do not "restore" a
  check that was never there.

  Left alone because every way of resolving it is a behavior change to a
  released public API rather than a fix: rejecting NULL breaks any caller
  relying on the block, and returning without waiting silently converts
  their synchronization into a fire-and-forget. `PMIx_Fence` does not go
  through this branch — it passes `op_cbfunc` — so nothing in the library
  depends on the answer.

The sweep found two leaks, both on error paths that only a failing
server can reach, and both fixed: `job_data()` dropped the `nspace`
string the unpack had just allocated when the identity did not match
our own (the only exit of that function that did not free it), and
`PMIx_Init` walked away from the `suri` the connect handed back — and,
on the first of the three server-ID stores, from its `kval` too — on
three of the error returns between `connect_to_peer` and the point
where the URI is finally stored. The three stores now release and free
identically; they used to disagree.

**A blocking wrapper that reads `cb.status` needs a recv callback that
writes it.** `PMIx_Fabric_update` reported `PMIX_SUCCESS` for a fabric
refresh that never happened. It is the one blocking wrapper here that
passes `cbfunc == NULL` — so the caddy `frecv` completes *is* its stack
`pmix_cb_t`, and the wakeup branch runs rather than the callback branch.
But `frecv` wrote `cb->status` in exactly one place: the unpack of the
server's own status. Every other exit — the lost-connection empty
buffer, a failed status unpack, a failed `ninfo`/`PMIX_INFO` unpack —
carried its outcome only in the local `rc` that the *callback* branch
passes along, leaving `cb->status` at the `PMIX_SUCCESS` `cbcon()` gave
it. The wrapper then returned success with the caller's `pmix_fabric_t`
untouched and stale. `frecv` now assigns `cb->status = rc` at
`complete:`, where `rc` is authoritative on every path.

Note the sibling did not have this: `PMIx_Fabric_register` passes
`mycbfunc`, which records the status it is handed, so only the
`cbfunc == NULL` shape is exposed. **When you add a blocking wrapper,
check which of the two it is** — the "caddy is in `cbdata`" convention
means the recv callback is writing the *caller's* status field directly,
and every early exit has to set it. No automated coverage: a singleton
returns `PMIX_ERR_UNREACH` at the `connected` gate long before the PTL
round-trip, and no in-tree host implements `pmix_server_module_t.fabric`.

**A short-circuit that declines must leave the caddy as clean as it
found it.** `try_local_fetch()` in `pmix_client_get.c` answers a get on
the caller's thread when the GDS module says its store is safe to read
there, and returns false to mean "not answered, take the ordinary path".
Its failed-*fetch* return drains `cb->kvs` and rebuilds it for exactly
that reason. Its failed-*`process_values()`* return did not — and
`process_values()` does not drain the list either, since on the path it
is written for the kval stays alive to own the value. So a decline after
a **successful** fetch fell through to `get_data()` with the fetched
entries still on `cb->kvs`, and `get_data()`'s own `PMIX_GDS_FETCH_KV`
appends to that same list. That matters because `process_values()`
distinguishes "the value" from "an aggregate of everything this proc
put" by **counting** the list: one stale entry turns a scalar `PMIx_Get`
into a `PMIX_DATA_ARRAY`. It now drains on both returns.

Nothing leaks either way — `cbdes()` destructs `kvs` — which is why this
is a wrong-answer bug rather than a leak, and why it would not show up
under valgrind. Reachability is narrow: `process_values()` declines only
on a malformed `PMIX_QUALIFIED_VALUE` in the datastore (see the fifth
sweep — not producible by a local `PMIx_Put`) or an allocation failure.
The rule to carry is the one the function's own comment already
states: **a short-circuit is never a partial one.** If you add an early
return to it, ask what state the slow path is about to inherit.

Far more useful is what was examined and **rejected**, because each one
looks like a defect until you chase it down. Do not re-open these
without new evidence.

- **`PMIX_ACQUIRE_OBJECT(cb)` sitting above the line that assigns `cb`**
  — in `_value_cbfunc()` and `get_data()` — is not a read of an
  uninitialized pointer. The macro is `#define PMIX_ACQUIRE_OBJECT(o)
  pmix_atomic_rmb()` ([`pmix_threads.h`](../threads/pmix_threads.h)): it
  discards its argument entirely, so the operand is never evaluated.
  Reordered here for readability, but do not go looking for a bug behind
  the same shape elsewhere in the tree.
- **`PMIx_Value_get_number(kv->value, …)` is called without a NULL check
  on `kv->value`** at the nodeid, appnum and sessionid fetches in
  `get_data()`, and that function dereferences `value->type` as its
  first act. The hostname fetch beside them *does* check. Left alone
  because nothing demonstrates a local GDS fetch producing a kval with a
  NULL value — `pmix_kval_t.value` is documented as legitimately
  NULL-able by [`src/mca/bfrops/AGENTS.md`](../mca/bfrops/AGENTS.md),
  but that is about what arrives off the wire through `pack_kval`, and
  these three read the local datastore. If you decide to close it, the
  screen belongs in `PMIx_Value_get_number` rather than at three call
  sites here, since every other caller in the tree is equally exposed.
- **`strdup(pmix_globals.hostname)` in `get_data()`'s invalid-rank
  branch is unguarded** where the branch above it tests
  `NULL != pmix_globals.hostname` first. It is safe:
  [`pmix_rte_init`](../runtime/pmix_init.c) resolves the hostname from
  the directive, then `PMIX_HOSTNAME`, then `gethostname()`, so the
  field is non-NULL for the entire life of an initialized library. The
  guarded sibling is being defensive, not covering a real state.

- **A client's `PMIx_Fabric_deregister` never tells the server, and
  cannot.** There is no `PMIX_FABRIC_DEREGISTER_CMD` — the command enum
  in [`pmix_globals.h`](../include/pmix_globals.h) has only
  `PMIX_FABRIC_REGISTER_CMD` (30) and `PMIX_FABRIC_UPDATE_CMD` (31). So
  the client frees `fabric->info` locally and stops, while the
  registration `pmix_server_fabric_register` made on its behalf stays on
  `pmix_pnet_globals.fabrics` for the life of the server. Closing it
  means a new wire command and a server handler, which is a protocol
  addition rather than a review fix; recorded so the local-only
  deregister is not mistaken for an oversight in this file.
- **`pmix_pnet.register_fabric` returning `PMIX_OPERATION_IN_PROGRESS`
  would break `PMIx_Fabric_register`'s blocking wrapper**, which treats
  anything that is neither `PMIX_SUCCESS` nor `PMIX_OPERATION_SUCCEEDED`
  as an error and destructs its stack caddy before the module's callback
  can fire. [`pnet.h`](../mca/pnet/pnet.h) does document that return, but
  the gap is not in this directory: `pmix_pnet_base_register_fabric`
  records the fabric on `pmix_pnet_globals.fabrics` only for
  `PMIX_OPERATION_SUCCEEDED`, so an async component's fabric could never
  be resolved by a later update or deregister either. No shipped
  component takes that path (see
  [`src/mca/pnet/AGENTS.md`](../mca/pnet/AGENTS.md)). Fix the base first
  if one ever does; patching the wrapper alone would hide half of it.

- **`PMIx_Finalize` throws the server's reply away, and that is not the
  `_commitfn`/`PMIx_Abort` defect.** The server *does* ack
  `PMIX_FINALIZE_CMD` with a status — `op_cbfunc2()` in
  [`pmix_server_switchyard.c`](../server/pmix_server_switchyard.c) packs
  whatever the host's `client_finalized` returned — and `finwait_cbfunc`
  discards the buffer, exactly the shape flagged for commit and abort
  above. It stays that way on purpose: the caller cannot act on it (the
  process is leaving), and `PMIx_Finalize` returning anything but
  `PMIX_SUCCESS` would newly fail applications that check it. Changing
  it is a behavior decision about a universally-called API, not a fix.
- **`PMIx_Init`'s debugger-wait handler is left registered, holding a
  pointer into a stack frame that is about to die, if the
  `PMIX_READY_FOR_DEBUG` notification fails.** `PMIX_EVENT_RETURN_OBJECT`
  is stored as a bare `cbobject` pointer (see the parser in
  [`pmix_event_registration.c`](../event/pmix_event_registration.c)), and
  it points at `releaselock` on `PMIx_Init`'s stack. On the success path
  the handler is `PMIX_EVENT_ONESHOT` and removes itself when it fires,
  so only the notify-failure return leaves it behind. It is not fixed
  because the obvious fix does not work: `PMIx_Deregister_event_handler`
  gates on `pmix_globals.initialized`, which `PMIx_Init` does not set
  until forty lines later, so the public API is a no-op from there and
  removing the handler means open-coding the threadshift the public
  entry point performs. Weigh that against the reachability — the
  process must ignore a failed `PMIx_Init`, keep running, and then be
  sent a `PMIX_DEBUGGER_RELEASE` it never asked for.
- **`pmix_globals.init_called` is set with `__atomic_test_and_set` and
  cleared with a plain `= false`.** It is a bare `bool`, not an
  `atomic_bool` like `initialized` beside it, so the clear is a
  non-atomic store racing an atomic RMW. Left alone because it is not a
  `src/client` decision: `pmix_client.c`, `pmix_server.c` and
  `pmix_tool.c` all do exactly this, and the only interleaving that
  reaches it is a concurrent `PMIx_Init`/`PMIx_Finalize`, which the
  comment at the top of `PMIx_Init` already declines to support.
- **The re-entrant `PMIx_Init` returns `PMIX_SUCCESS` where the first
  call returned `PMIX_ERR_UNREACH`.** A second library in the same
  process therefore reads "connected" from a singleton that is not, and
  discovers otherwise only when a server round-trip fails. Recorded
  rather than changed: the multi-init refcount path exists precisely for
  those second callers, so altering what it returns is a contract change
  affecting all of them.
- **`PMIx_Abort`'s server-role branch calls `pmix_host_server.abort()`
  with `NULL` for both `cbfunc` and `cbdata` and returns the host's
  status verbatim.** The typedef documents the host as completing
  through that callback, so a host honoring the contract either
  dereferences NULL or never completes, and a host answering
  `PMIX_OPERATION_SUCCEEDED` (-157) has that handed to the application
  as a failure. Not changed because the library's own abort up-call in
  [`pmix_server_ops.c`](../server/pmix_server_ops.c) treats
  `PMIX_OPERATION_SUCCEEDED` as an error too, so this is a tree-wide
  convention rather than a divergence here, and the in-tree host
  (`test/simple/simptest.c`) ignores `cbfunc` entirely, so nothing
  available demonstrates the failure.

## The eighth sweep — `pmix_client_group.c` and the setup/progress-thread seam

A second pass devoted to this one file, five lenses over it. The theme is
not the type-tag hazard the fourth and fifth sweeps established — that
ground held — but a different seam: **the moment a group tracker becomes
the progress thread's, and what is still being written to it afterwards.**

**Registering the invitation observer publishes the tracker, and the
invitation can resolve inside that call.** `pmix_event_register_observer`
runs `check_cached_events()` (see
[`pmix_event_registration.c`](../event/pmix_event_registration.c)) before
it acknowledges the registration, and that replays every matching cached
notification through `pmix_invoke_local_event_hdlr` →
`sweep_observers()` — synchronously, on the progress thread, with the
brand-new observer already on the list. So a leader inviting procs that
have *already* terminated, whose `PMIX_PROC_TERMINATED` events were cached
because nothing was watching for them, has `invite_observer()` count every
answer and call `invite_wake()` from inside the registration call itself.

`invite_setup()` then went on to do three things to a tracker that the
announcement chain already owned:

- it set `cb->optional` from `PMIX_GROUP_OPTIONAL` *after* `announce_step()`
  had read it, so an invitation the caller marked optional was aborted
  under the default all-or-nothing policy;
- it armed the timeout timer on a tracker the chain might already have
  retired, leaving a live `pmix_event_t` on freed memory;
- and in the `_nb` form it kept reading `cb->info` to notify the invitees
  after `invite_finish()` had released the tracker — a use-after-free.

The directive scan, the range info and the timer are now all established
**before** the registration, and `invite_setup()` holds a reference of its
own (`PMIX_RETAIN`/`PMIX_RELEASE`) across the registration and the
notification so nothing can free the tracker underneath it. It also
reports success when `cb->completed` is set on the way out, because the
caller has been completed and the tracker retired by then — returning an
error there had `PMIx_Group_invite_nb` run `invite_teardown()` on a
tracker that was already torn down.

**The rule worth carrying:** in this file, *registering an observer is a
publication*. Everything the resolution path reads must be on the tracker
before it, nothing may be armed on the tracker after it, and any use of
the tracker below it needs a reference. The same reasoning applies to
`setup_leader_watch()`, which is why that one hands the registry ownership
via `watch_relcb` and touches nothing afterwards.

*Wire format:*

- **`PMIx_Group_destruct_nb` packed a field the server never unpacks.**
  Both group commands are unpacked by the same routine,
  `pmix_server_group()` in
  [`pmix_server_group.c`](../server/pmix_server_group.c), which reads the
  proc array only under `if (0 < nprocs)`. `construct_msg()` guards its
  pack to match; destruct packed unconditionally — and
  `pmix_bfrops_base_pack()` writes an element count even for a zero-length
  array, so destructing a group whose membership had drained put bytes on
  the wire that the server never consumed. Its next unpack read that stray
  count as the directive count and *succeeded* having unpacked nothing
  (`pmix_bfrops_base_unpack` reports `PMIX_SUCCESS` with `*num_vals = 0`
  when the stored count is zero), leaving the server's `ninf` uninitialized
  to size an allocation and then index it. Both halves are closed: the
  client guards the pack, and `ninf` is initialized.

  The general point: **a guarded unpack demands a guarded pack.** When two
  commands share an unpack routine, they have to agree with it
  field-for-field, and "pack it anyway, the count is zero" is not a no-op
  on this wire.

*Crashes on wire- or caller-supplied input:*

- `pmix_server_process_grpinfo()` read `pinfo[0]` and dereferenced
  `pinfo[0].value.data.proc` on nothing more than the key matching
  `PMIX_PROCID`. All three of its callers — this file's
  `construct_cbfunc()`, `pmix_server_setup.c` and `pmix_server_group.c` —
  hand it an array that came off the wire, so neither the length nor the
  union member was theirs to assume. The guard went into the helper, where
  it covers all three; `construct_cbfunc()` additionally now requires a
  non-empty array, which it was already checking for non-NULL.
- `invite_setup()` fetched each wildcard's job size **twice** — once to
  size `cb->members` and once to fill it — and wrote the second pass's
  count into the first pass's allocation. An elastic job that grew between
  the two overran the array. The writes are bounded now and a disagreement
  is an error rather than a short membership.
- `construct_cbfunc()` unpacked the returned membership into an unchecked
  `PMIX_PROC_CREATE` result, and `construct_msg()` filled an unchecked
  `PMIX_INFO_CREATE` one.
- `PMIx_Group_construct[_nb]` accepted `procs == NULL` with `nprocs > 0`
  and walked the array. A NULL `procs` is legal for an add-members or
  bootstrap participant, but only when it is declared empty.

*Leaks:*

- `get_endpts()` was the one exit of that function that returned without
  `PMIX_DESTRUCT`ing its `pmix_cb_t`, dropping the kvals the GDS fetch had
  just handed it.
- `invite_finish()` took `pmix_event_deregister_observer()` on trust. That
  call fails once the library is shutting down, and when it does no
  callback comes, so the tracker was never released. `invite_teardown()`
  checks the same return for the same reason.

*Correctness, lesser:*

- `PMIx_Group_leave_nb()` treated the NULL from `PMIX_PROC_CREATE(0)` as an
  allocation failure, so leaving a group whose membership had drained
  returned `PMIX_ERR_NOMEM` — and returned *before* dropping the group, so
  the caller could never leave it. An empty membership means there is
  nobody to notify, which is not an error. (See the seventh sweep for why
  `nmbrs == 0` is a reachable state.)

*Noted, not changed:*

- **`PMIx_Group_invite_nb` deadlocks if called from an event handler.**
  `invite_setup()` runs on the caller's thread and takes two
  `PMIX_WAIT_THREAD`s on operations that thread-shift — the observer
  registration and the `PMIX_GROUP_INVITED` notification — so driving it
  from a handler waits on the progress thread from the progress thread.
  For the blocking `PMIx_Group_invite` that is merely the documented
  property of this file; for the `_nb` form it is a contract defect, since
  being callable from a handler is the whole point of the non-blocking
  variant. Closing it means making the setup itself a chain of
  non-blocking steps, the way `announce_step()` already is — a
  restructure, not a review fix.
- `cb->completed` and `cb->timer_active` are plain `bool`s read from the
  caller's thread and written from the progress thread. Every read is
  downstream of a mutex operation today, so the ordering is incidental
  rather than declared. Left as-is because it matches the rest of the
  file; if one more cross-thread flag appears here, the tracker wants a
  lock rather than a fourth ad-hoc barrier.
- `info_cbfunc()` calls `add_group()` with its own tracker's `notterm`,
  which is always false — the flag lives on the tracker
  `construct_cbfunc()` reads. That is harmless only because
  `add_group()` ignores a repeat request and `construct_cbfunc()` gets
  there first with the right value. On the *join* path there is no first
  call, so a joiner never records `PMIX_GROUP_NOTIFY_TERMINATION` — which
  is correct, since a joiner supplied no construct directives.

*No regression coverage was added, and none is possible in `make check`.*
Every finding above sits behind either a server round-trip or a cached-event
replay, and the group entry points check `connected` before their
arguments (see [Testing](#testing)), so a singleton cannot reach any of
them. The thirteen `run_grp*.pl` tests do cover the reordered
`invite_setup` end-to-end through `simptest` — construct, invite,
invite-nb, join, decline, leave, destruct, both timeout cases, both abort
cases, and the suppression case — and all pass.

## Defects found in the August 2026 review (ninth sweep — `pmix_client_resolve.c`)

Five lenses over the resolve pair. Almost everything found is one fact
wearing four hats, so learn the fact rather than the four sites.

**`PMIx_Argv_split()` returns NULL for a string with no tokens, not an
empty array** — see `pmix_bfrops_base_tma_argv_split_inter()`, whose loop
never runs for an empty source. Every list this file gets out of the
datastore can legitimately be the **empty string**, because a namespace's
map can name a node it placed nothing on, and `store_map()` records
`PMIX_LOCAL_PEERS` for such a node as `strdup("")`. The `NULL == string`
guards that are everywhere in this file do **not** cover that state; they
are a different one. Three sites indexed the NULL:

- `resolve_peers()`'s aggregate walk recorded an `"nspace:"` entry for the
  empty namespace and then split every recorded entry back apart to fill
  the proc array — a SIGSEGV on the progress thread, taken as soon as any
  *other* namespace contributed a proc and made the transfer pass run.
  Entries are now counted before they are recorded, so an empty one is
  never recorded at all.
- `resolve_nodes()`'s aggregate walk did the same with `PMIX_NODE_LIST`.
  That one is hardening rather than a reproduced defect: `PMIX_NODE_LIST`
  is derived by joining the node map, so the hash datastore cannot
  produce an empty one — only a host storing the key itself could.
- `pmix_server_locally_resolve_peers()` and `..._node()` in
  [`pmix_server_resolve.c`](../server/pmix_server_resolve.c) are the
  *same walk* and had the *same* defects. See the twin rule below.

**And `PMIX_PROC_CREATE(0)` returns the same NULL an allocation failure
does**, which is the seventh sweep's rule meeting the same empty list.
Asking for a namespace by name on a node it placed nothing on counted
zero peers, handed the count to `PMIX_PROC_CREATE`, and reported
`PMIX_ERR_NOMEM` — where `PMIx_Resolve_peers(3)` documents an empty result
as `PMIX_SUCCESS` with a NULL array and a zero count. Both the client and
the server had it. Regression: `test/unit/resolve_api.c`.

### `pmix_client_resolve.c` and `pmix_server_resolve.c` are twins

`resolve_peers()`/`resolve_nodes()` here and
`pmix_server_locally_resolve_peers()`/`..._node()` there are the same
computation over the same datastore, written twice. **Every defect found
in one of them was present in the other**, in both directions: the empty-
list crash and the `PMIX_PROC_CREATE(0)` report went client → server this
sweep, and the per-namespace rank reset went server → client (the server
had already fixed it, with a comment saying why, while the client had
not). When you change one, read the other before you stop.

The rank reset is worth understanding rather than copying. `try_fetch()`
mutates `cb->proc->rank` to `PMIX_RANK_WILDCARD` for its retry and does
not put it back, and it *declines outright* for a rank that is not
`PMIX_RANK_UNDEF` — so a single aggregate walk gave its first namespace a
different lookup from all the rest, and the answer depended on the order
the namespaces happened to sit in `pmix_globals.nspaces`. It was benign
against today's `hash` module, and it is worth knowing exactly why before
anyone decides the reset is unnecessary: for a `PMIX_NODE_INFO`-qualified
fetch, `pmix_gds_hash_fetch()` sends both `UNDEF` and `WILDCARD` down the
`!PMIX_RANK_IS_VALID` branch to `fetch_nodeinfo()`, and only `WILDCARD`
additionally gets the `doover` retry against the job-level table. So
`WILDCARD` is a superset there, and nothing was lost — by accident, and
only for one module.

### The caddy outlives the reply, and both halves must agree about it

Both `PMIx_Resolve_peers` and `PMIx_Resolve_nodes` **reuse their
`pmix_resolve_caddy_t`** when the server round-trip fails: they reset the
lock and re-shift the *same* object into the local computation, because a
server that does not recognize the command is the expected reason to fall
back. Two rules follow, and neither is visible from the recv callback
alone:

- **The recv callback must leave `procs` and `nprocs` agreeing with each
  other on every failure path.** `wait_peers_cbfunc()` freed the array on
  a failed unpack and left the count standing; the local fallback then
  found nothing, reported `PMIX_SUCCESS`, and the caller was handed a
  NULL array with a non-zero count to index. `PMIX_PROC_FREE` nulls the
  pointer it is given, so the caddy destructor was safe — this is a
  wrong-answer bug, not a double free, which is why it would not show up
  under valgrind.
- **Report what actually arrived, not what the peer said it was
  sending.** `pmix_bfrops_base_unpack()` reports `PMIX_SUCCESS` having
  filled *fewer* entries when the wire count exceeds the payload, writing
  the real number back through `num_vals`. Same defect and same fix as
  `_lookup_cbfunc()` in `pmix_client_pub.c`; and the wire-sized
  `PMIX_PROC_CREATE` needs its NULL check for the same reason that one
  does.

*Correctness, lesser:*

- `resolve_nodes()` `PMIX_ERROR_LOG`ged `PMIX_ERR_INVALID_VAL` for a
  `PMIX_NODE_LIST` that was not a string and then fell through to a
  `done:` where `rc` was still `PMIX_SUCCESS` — so the caller was told the
  call succeeded and handed a NULL `nodelist` it is entitled to parse. Its
  sibling `resolve_peers()` set the status in the same spot. Note the two
  halves of that test are *not* the same thing and are now split: a wrong
  **type** is a datastore that has garbage under a reserved key and is an
  error; a **NULL string** is "no nodes", which is the documented empty
  answer.
- Both host-query branches called `pmix_host_server.query` with a query
  they had failed to build — the `PMIX_ARGV_APPEND` status was unchecked
  and `PMIX_INFO_CREATE(iptr, 2)` was dereferenced unchecked. They now
  skip the up-call and work the answer out locally, which is what the
  branch already does for a host that cannot answer.
- `respeer()`/`resnode()` dereferenced an unchecked `PMIX_INFO_CREATE`.
- Two `strdup()` results were handed back as the answer without being
  checked, so a failed allocation was reported as `PMIX_SUCCESS` with a
  NULL string.
- `resolve_peers()` recorded the *counted* peer total rather than the
  number it actually transferred, so a failed `PMIx_Argv_append_nosize`
  would have handed the caller phantom entries.

*Noted, not changed — do not re-open these without new evidence:*

- **`PMIX_NEW` is not NULL-checked anywhere in this directory**, for
  buffers or for caddies, and that is the convention rather than an
  oversight in this file. The earlier sweeps flagged unchecked
  `PMIX_INFO_CREATE` and `PMIX_PROC_CREATE` repeatedly and never flagged
  `PMIX_NEW`. Adding checks here alone would be the inconsistency.
- **`pa[np].rank = strtoul(p[m], NULL, 10)` validates nothing** — a
  non-numeric token yields rank 0 and a value past `UINT32_MAX` truncates
  silently. There are fourteen such sites in the tree, including the exact
  mirror of this line in `pmix_server_resolve.c`. If this is ever worth
  closing it wants a shared helper, not a screen here.
- **`kv->value` is dereferenced without a NULL check** at all four
  datastore reads. `pmix_kval_t.value` is legitimately NULL-able off the
  wire (see [`src/mca/bfrops/AGENTS.md`](../mca/bfrops/AGENTS.md)), but
  these are local GDS fetches — the same reasoning the seventh sweep
  recorded for `get_data()`.
- **A lost connection between the `connected` check and the send hangs
  these two APIs forever.** `pmix_ptl_base_send_recv()` drops the message
  without invoking the recv callback when the peer has `sd < 0`, after
  `PMIX_PTL_SEND_RECV` has already reported success — so the
  `PMIX_WAIT_THREAD` below it waits on a lock nothing will wake. This is
  the tree-wide condition described under [Invariants](#invariants-and-gotchas);
  `PMIx_Finalize` is the only entry point that arms a timer against it.
  Fixing it in this one file would be an inconsistency, not a fix.
- **The `singleton` half of the "gate every round-trip" pattern is
  genuinely redundant here.** `pmix_globals.connected` is set in exactly
  one place — `ptl_base_fns.c` after a successful connect — so a
  singleton never has it, and every sibling in this directory gates on
  `!connected` alone.
- `PMIX_INFO_LOAD(&iptr[0], PMIX_NSPACE, str, PMIX_STRING)` with a NULL
  `str` is safe: `pmix_bfrops_base_value_load()` zeroes the union for a
  NULL source rather than reaching `strdup`. A NULL nspace is the
  documented "all namespaces" request and reaches both host queries.
- The dead `PMIX_ERR_DATA_VALUE_NOT_FOUND` arm in both functions, and the
  dead `proc.rank = PMIX_RANK_WILDCARD` store in the pre-v3.2 branch, are
  unchanged and still carry their explaining comments. `try_fetch()`
  returns only `PMIX_SUCCESS`, `PMIX_ERR_INVALID_NAMESPACE` and
  `PMIX_ERR_NOT_FOUND`.

## Defects found in the August 2026 review (tenth sweep — `pmix_client_spawn.c`)

Five lenses over the spawn pair. The theme is **the boundary between the
caller's arrays and ours**, which the third sweep opened when it moved the
envar harvest below the apps copy. Everything above that copy is running on
the application's memory and reading counts the application owns, and three
of the findings are that boundary being crossed in one direction or the
other.

*The caller's arrays are the caller's:*

- **`PMIx_Spawn_nb` wrote the computed directive count back into the
  caller's `const pmix_app_t apps[]`.** An app may declare its directives by
  terminating them with an end-marked info instead of setting `ninfo`; the
  scan that counts them stored the result into `aptr->ninfo`, and `aptr` is
  a cast-away-`const` alias for `&apps[n]`. The value written happens to be
  the right one, which is why nothing ever misbehaved — but a caller whose
  apps sit in read-only storage takes a SIGSEGV on the store. The count is a
  local now. **This is the same defect the third sweep fixed for
  `PMIx_Setenv()`, in the one other place above the copy that writes
  through `aptr`** — if you add a third, put it below the copy or give it a
  local.
- **`(info, ninfo)` is a pair, and a zero count means "no directives"
  whatever the pointer is.** The directive scan entered on `NULL !=
  job_info` alone, built an info list from zero entries, and handed it to
  `PMIx_Info_list_convert()` — which reports `PMIX_ERR_EMPTY` for an empty
  list. `PMIx_Spawn(job_info, 0, …)` therefore failed the whole spawn with
  a status that names nothing the caller did wrong. Every sibling that
  builds an info list here already guards it: `pmix_client_connect.c` and
  `pmix_client_group.c` both gate the `convert` on a `found` flag rather
  than on the pointer. Regression: `test/unit/spawn_api.c`.

*Correctness:*

- **An app-level `PMIX_SETUP_APP_ENVARS` harvest served only the first app
  that asked for one.** The per-app branch set the same `jobenvars` flag
  that says "the job-level directive already covered every app", so after
  app 0 harvested, app 1's own directive was never looked at. The two are
  not the same thing — a job-level harvest is applied to *all* apps' `env`
  (see the loop below `joblevel_envars`), an app-level one only to that
  app's — so an MPMD spawn whose second app asked for its own programming
  model's envars silently got none. The redundant flag is gone; the scan
  gates on `joblevel_envars` alone. Note this only ever bit the roles that
  *have* a `pmdl` open — tool, launcher, server — for the reason the third
  sweep's entry gives.

*Unchecked allocations that are then indexed:*

- `PMIX_APP_CREATE(fcd->apps, …)`, the per-app `PMIX_INFO_CREATE`, and the
  bare `malloc(2 * sizeof(char *))` for a synthesized `argv` were all
  dereferenced on the next line. The server's own unpack of this very
  message (`pmix_server_spawn`, `pmix_server_ops.c`) checks both of the
  same two macros, so this was a divergence between the two halves of one
  wire operation rather than a house style. `fcd->copied` is now set at
  construction rather than after the arrays are filled, so the new error
  returns between the two actually free what the caddy already holds.

  This is the class the earlier sweeps have consistently closed
  (`respeer()`, `construct_msg()`, `construct_cbfunc()`). It is **not** an
  invitation to check `PMIX_NEW`, and the unchecked `strdup()` /
  `PMIx_Argv_copy()` / `PMIx_Argv_prepend_nosize()` results in the same
  loop are deliberately left: hardening this function against allocation
  failure end-to-end is a separate piece of work, and doing half of it is
  worse than none.

*Noted, not changed — do not re-open these without new evidence:*

- **A host `spawn` that returns `PMIX_OPERATION_SUCCEEDED` is dropped on
  the floor.** The tree-wide host up-call convention (see
  [`src/server/AGENTS.md`](../server/AGENTS.md)) is that
  `PMIX_OPERATION_SUCCEEDED` means "done now, I will not call back — you
  invoke the completion yourself", and this site treats it as an error:
  it releases the caddy, never fires `fcd->spcbfunc`, and the blocking
  `PMIx_Spawn` wrapper then maps the status to `PMIX_SUCCESS` and hands
  the caller an **empty nspace**. Left alone because the spawn contract has
  no synchronous channel for the namespace in the first place — a host
  completing atomically has no way to tell us what it launched — so the
  return is not really usable here; because `pmix_server_spawn` in
  `src/server` has the identical shape, making this a convention rather
  than a divergence; and because nothing in the tree returns it (`pfexec`
  does not, and the in-tree host modules do not). Closing it properly means
  extending the `pmix_server_spawn_fn_t` contract, not editing this branch.
- **The `spawn_iof_flags` stand-in is process-wide and is written from the
  caller's thread** (`stash_spawn_iof_flags`) while `spawn_or_global_flags()`
  in [`pmix_iof.c`](../common/pmix_iof.c) reads it on the progress thread.
  It is safe for the case it was built for: the store happens before
  `PMIX_PTL_SEND_RECV`, and forwarded output cannot arrive before the
  request goes out, so the send path's peer lock publishes it. What it does
  **not** survive is two spawns in flight at once from one process — the
  second overwrites the first's flags. That is inherent to a single
  process-wide slot, not a bug in this file; if a tool ever needs
  concurrent spawns formatted differently, the stand-in has to become a
  per-request one.
- **The server branch reads `pmix_server_globals.clients` from the
  caller's thread.** `pmix_get_peer_object()` walks that pointer array
  unlocked to resolve a `PMIX_PARENT_ID`, and `PMIx_Spawn_nb` in a server
  role runs on whatever thread the host called it on, while the progress
  thread adds and removes clients. Recorded rather than fixed because the
  cure is to thread-shift the whole entry point — the sibling call in
  `pmix_monitor.c` has exactly the same exposure — and because a host
  spawning on behalf of a client it is concurrently losing is not a state
  anything demonstrates.
- **`req->flags = cd->flags` in `pmix_server_process_iof()` aliases the
  caddy's `file`/`directory` strings**, which `scaddes` frees when
  `_lclcbfunc` releases the caddy — so the IOF request is left holding two
  dangling pointers. It is harmless today only because **nothing reads
  `req->flags.file` or `req->flags.directory`**: the output path formats
  from `nptr->iof_flags` (the namespace's copy, which `wait_cbfunc`
  deliberately NULLs those two members on). Worth knowing before adding a
  reader.

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

  The directory was swept for gaps in this (480 sites now). What is
  deliberately left **unmarked** is worth knowing, because it is not
  oversight:

  - **Success branches.** Marking the error side `PMIX_UNLIKELY` already
    tells the compiler everything; adding `PMIX_LIKELY` to the inverse
    test is redundant. `if (PMIX_SUCCESS == rc)` stays bare.
  - **Expected-outcome dispatch.** `resolve_peers()`/`try_fetch()` branch
    on `PMIX_ERR_INVALID_NAMESPACE`, `PMIX_ERR_NOT_FOUND` and
    `PMIX_ERR_DATA_VALUE_NOT_FOUND` as *answers*, not failures — "that
    namespace is unknown", "the node is not in it", "the host did not
    supply it". Same for a lookup that misses, `get_data()`'s
    retry-as-wildcard fallback, and `PMIX_ERR_EXISTS_OUTSIDE_SCOPE`. A
    `PMIX_ERR_` prefix does not make a branch rare.

- **`PMIX_LIKELY` has no place in this directory, and that is on
  purpose.** It is used five times in the whole tree, all in genuinely
  hot code (`pmix_hotel.h`, the `ptl` byte-count loop) where the marked
  branch is the taken one and there is no error branch to mark instead.
  Nothing in `src/client` is on a per-byte or per-message-fragment path,
  and essentially every conditional worth hinting here is "did this
  fail?", which `PMIX_UNLIKELY` already covers. Adding `PMIX_LIKELY`
  alongside it would be noise that implies a hot path where there is
  none.

- **Wrap only the rare sub-expression.** `PMIX_UNLIKELY` expands to
  `__builtin_expect(!!(expr), 0)`, which *returns* its argument, so
  `PMIX_UNLIKELY(A) && B` is exactly `A && B` with a hint on `A` alone.
  Prefer that to hinting the whole test when only one half is rare — as
  in `if (PMIX_UNLIKELY(PMIX_SUCCESS != rc) && NULL != msg)`, where the
  failure is rare but the "did we allocate a message?" half is not.

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
