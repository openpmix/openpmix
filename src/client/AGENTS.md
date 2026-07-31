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

## Testing

There are two tiers, and the split is forced by the architecture: almost
every function here either answers locally or round-trips to a server, so
a test with no server can only reach the first half.

**Tier 1 — `test/unit/client_api.c` (in `make check`).** Runs as a
singleton. Covers what a client decides by itself: the `PMIx_Get`
shortcuts that `process_request` answers outright, the pointer/static
value forms, and the parameter validation on `PMIx_Get[_nb]`,
`PMIx_Compute_distances_nb` and the fabric APIs. Every case in it is a
regression for a specific defect listed below; several of them segfaulted
before. See [`test/unit/AGENTS.md`](../../test/unit/AGENTS.md).

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
- **`PMIx_Group_join` completes earlier than its man page says, and so
  returns no results** — and the invitee-side **leader-failure watch does
  not fire at all**. Both have one cause, and it is *not* the one the
  comment above `pmix_group_leader_watches` gives.

  The library observes group lifecycle events by registering an ordinary
  handler (`setup_leader_watch`). That registration lands in the
  **multi-code** category, and the chain visits `first` → `single-code` →
  `multi-code` → `default` → `last`. An application handler registered
  for a *single* code therefore runs first, and if it returns
  `PMIX_EVENT_ACTION_COMPLETE` — the normal way to say "handled" — the
  chain ends and the library's own handler is never reached. Any
  application that watches `PMIX_GROUP_CONSTRUCT_COMPLETE`, which an
  invite/join application must, blinds the library to it.

  Demonstrated with `test/unit/run_grpinvite.pl`: every acceptor's
  application handler receives the event while not one of the library's
  watches does, in the same process and the same run; flipping only the
  example's return to `PMIX_EVENT_NO_ACTION_TAKEN` makes all three
  watches receive it. It is not a delivery problem and not a
  registration race (reordering the watch ahead of the accept changes
  nothing).

  So join cannot complete at the documented point until the library can
  see the construct event. The fix is local, not protocol-level: the
  pre-chain block in `pmix_invoke_local_event_hdlr()`
  (`src/event/pmix_event_notification.c`) already maintains
  `pmix_client_globals.groups` on these same codes, unconditionally and
  ahead of the chain, and that is where the library's interest belongs.
  Written up for discussion before implementing. Until then an
  application takes the membership from its own
  `PMIX_GROUP_CONSTRUCT_COMPLETE` handler.

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
