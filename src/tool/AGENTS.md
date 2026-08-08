# AGENTS.md: The PMIx Tool Library

This document orients AI agents and human contributors working in
`src/tool`, the tool-role implementation of the PMIx APIs. It assumes you
have already read the top-level [`AGENTS.md`](../../AGENTS.md) — the
golden rules (prefix conventions, `pmix_config.h`-first include order,
constant-on-the-left comparisons, brace-everything, warning-free under
`--enable-devel-check`), the **thread-safety / progress-thread model and
the caddy pattern**, the "never call a thread-shifting `PMIx_*()` from
inside the library" rule, the backward-compatibility / wire-format
rules, and the copyright-header requirement all apply here and are not
repeated. It also assumes familiarity with
[`src/client/AGENTS.md`](../client/AGENTS.md) and
[`src/server/AGENTS.md`](../server/AGENTS.md): a tool process links and
drives **both** of those code bases, and this file only covers what is
specific to the tool role.

Like `src/client` and `src/server`, **`src/tool` is not an MCA
framework.** There is no framework header, no components, no selection
logic — two `.c` files compiled straight into `libpmix` via
[`Makefile.include`](Makefile.include).

## What a "tool" is (and why it is three roles at once)

Per the top-level terminology, a **tool** is a process that initializes
with `PMIx_tool_init` rather than `PMIx_Init` — a debugger, profiler,
launcher, or workflow manager. Unlike a client, a tool may or may not
have been pre-registered with a server, and it establishes its own
connection(s) rather than inheriting one.

The defining structural fact of this directory is that a single tool
process is simultaneously **a client, a server, and a tool**, and which
hats it wears is decided at init time from directives and the
environment. The `pmix_proc_type_t` computed in `PMIx_tool_init` spans:

| Role bits | Set when | Consequence |
|-----------|----------|-------------|
| `PMIX_PROC_TOOL` | always (baseline) | links the client op code; can connect to a server |
| `PMIX_PROC_LAUNCHER` | `PMIX_LAUNCHER` directive true | also acts as a **server**: starts a listener, opens `pfexec`/`pnet`/`hwloc`, can spawn |
| `PMIX_PROC_SCHEDULER` | `PMIX_SERVER_SCHEDULER` directive true | server + scheduler; does **not** request its own job info |
| `PMIX_PROC_CLIENT_TOOL` | tool *and* `PMIX_RANK`/`PMIX_NAMESPACE` came from the env | it was itself launched by a PMIx server, so it is also that server's client |
| `PMIX_PROC_CLIENT_LAUNCHER` | launcher launched by a server | launcher that is also a client of its parent |

Because of this, almost everything in `pmix_tool.c` reaches into
`pmix_client_globals` (for `myserver`, IOF sinks, peer arrays) **and**
`pmix_server_globals` (for the clients array, nspace/collective lists,
tmpdirs). `pmix_server_initialize()` is called **unconditionally** on
every `PMIx_tool_init` (even for a plain tool), because a tool may later
be handed a server module via `PMIx_tool_set_server_module`, and because
it needs the server-side receive/relay machinery to talk to other tools.

## The files

| File | Public API / role |
|------|-------------------|
| `pmix_tool.c` | `PMIx_tool_init`, `PMIx_tool_finalize`, and the multi-server management API: `PMIx_tool_connect_to_server`, `PMIx_tool_attach_to_server`, `PMIx_tool_disconnect`, `PMIx_tool_get_servers`, `PMIx_tool_set_server`, `PMIx_tool_is_connected`, `PMIx_tool_set_server_module`. Also the internal `pmix_tool_init_info` (synthesizes singleton job data) and all the PTL receive callbacks. |
| `pmix_tool_ops.c` | The tool-to-tool **relay** engine: `pmix_tool_relay_op` (relay an unsupported command — currently only `PMIX_SPAWNNB_CMD` — from a tool that connected to *us* onward to a real server) and its response `tool_switchyard`. Plus the two server-switching thread-shift handlers `pmix_tool_retry_set` and `pmix_tool_retry_attach` (declared here, so the server switchyard can reach them; **defined in `pmix_tool.c`**). |
| `pmix_tool_ops.h` | Declares the three `PMIX_EXPORT` symbols above. |

> Naming caution: `pmix_tool_ops.h` declares `pmix_tool_retry_set` /
> `pmix_tool_retry_attach`, but their bodies live in `pmix_tool.c`, while
> `pmix_tool_relay_op` / `tool_switchyard` live in `pmix_tool_ops.c`. Do
> not assume the header and the `_ops.c` file are a matched pair.

## `PMIx_tool_init`: the long pole

`PMIx_tool_init` is by far the biggest function here; read it top to
bottom before editing. Its phases, in order:

1. **Reference-counted re-entry.** `pmix_globals.init_called` is a
   test-and-set latch; `tool_init_cntr` is the nesting count. A second
   `PMIx_tool_init` in the same process just bumps the counter and
   returns the cached id — only the first call does real work, and only
   the last `PMIx_tool_finalize` tears down (see finalize below). A tool
   calling init twice is bad practice but is tolerated.
2. **Directive + environment parsing.** `PMIX_TOOL_NSPACE`/`PMIX_TOOL_RANK`
   (must be given together or neither), `PMIX_TOOL_DO_NOT_CONNECT`,
   `PMIX_TOOL_CONNECT_OPTIONAL`, `PMIX_FWD_STDIN`, `PMIX_LAUNCHER`,
   `PMIX_SERVER_SCHEDULER`, tmpdir overrides, `PMIX_IOF_LOCAL_OUTPUT`.
   Then env fallbacks: `PMIX_NAMESPACE`/`PMIX_RANK` (set when a server
   launched us — this is what promotes us to a `CLIENT_*` type),
   `PMIX_KEEPALIVE_PIPE` (registers `pdiedfn` to synthesize a
   `PMIX_ERR_JOB_TERMINATED` event if the parent dies).
3. **`pmix_rte_init`** with the computed proc type and
   `pmix_tool_notify_recv` as the notification receive.
4. **Compat-module selection.** bfrops (native/highest), psec (from
   `PMIX_SECURITY_MODE`), buffer type (from `PMIX_BFROP_BUFFER_TYPE`),
   and gds — **tools are hard-restricted to the `"hash"` gds component**
   for talking to a server. The same modules are copied onto
   `myserver->nptr->compat`.
5. **`pmix_server_initialize()`** (always) and zero out
   `pmix_host_server`.
6. **Connect or self-assign.** If `do_not_connect`, or if connect fails
   and `connect_optional` was set, the tool **self-assigns** a namespace
   of `hostname:pid` and rank 0 and points `myserver` back at *itself* —
   this is the tool analog of the client singleton path. Otherwise it
   `pmix_ptl.connect_to_peer`s and stores the resulting `PMIX_SERVER_URI`.
7. **Local job-data synthesis** (`pmix_tool_init_info`): because a tool is
   a singleton job of size 1, it fabricates the well-known job keys
   (`PMIX_JOBID`, `PMIX_RANK`, `PMIX_UNIV_SIZE`, `PMIX_JOB_SIZE`,
   `PMIX_LOCAL_PEERS`, `PMIX_NODE_MAP`/`PMIX_PROC_MAP`, hostname, the
   server's nspace/rank, …) directly into its own GDS rather than asking
   a server.
8. **Job-info round-trip.** If connected (and not a scheduler), send
   `PMIX_REQ_CMD` and block on `job_data` for the real job info; if the
   server returned nothing (e.g. a re-invoked launcher whose registration
   was already reaped), fall back to `pmix_tool_init_info` again.
9. **Server-role setup** (launcher/scheduler only): post the wildcard
   `pmix_server_message_handler` recv, open `pfexec`, discover topology
   (`pmix_hwloc_setup_topology`), open/select `pnet`, and
   `pmix_ptl_base_start_listening`.
10. **Launcher rendezvous / debugger release.** If
    `PMIX_LAUNCHER_RNDZ_URI` is set, attach to that server, pull its job
    info, register a one-shot handler for `PMIX_DEBUGGER_RELEASE`, block
    until released, then restore the original primary server. This is the
    "fork/exec'd launcher waits for the debugger to configure it" flow.

Every early `return` in init is a **hard failure that leaves the library
partially initialized** — there is no unwind. Preserve that contract:
add new failable steps late, and free anything you allocated before
returning.

## `PMIx_tool_finalize`: reference-counted, order-sensitive teardown

Finalize decrements `tool_init_cntr` and only tears down on the last
call. Then, in order: send `PMIX_FINALIZE_CMD` to the server with a
5-second timer guard (`fin_timeout`/`finwait_cbfunc`) and clear
`pmix_globals.connected` however that went, kill any `pfexec`-launched
children (launcher/server only, **before** the progress thread stops
because killing needs it), delete the file-scope events init registered
(stdin read, SIGCONT, keepalive pipe — **while their bases still
exist**), stop the progress thread, dump and destruct the static IOF
sinks, release the client peer/pending lists, stop listening, release the
server clients array, close `pnet`/`pmdl`, `PMIX_LIST_DESTRUCT` every
`pmix_server_globals` list that `pmix_server_initialize` builds, free the
tmpdir strings, then `pmix_rte_finalize`, release `mypeer`/`myserver`,
and `pmix_class_finalize`.

Nothing in that sequence returns early on an error — see the
"must not abandon the teardown" invariant below.

Three subtleties the current code documents inline and you must keep:

- It resets `pmix_globals.init_called = false` so a *subsequent*
  `PMIx_tool_init` in the same process starts fresh instead of
  short-circuiting on the latch.
- It frees and NULLs `pmix_server_globals.tmpdir` / `system_tmpdir`
  because the init-time guards only refresh them when NULL — a surviving
  pointer would silently ignore a changed `PMIX_SERVER_TMPDIR` on the
  next cycle. The many `PMIX_LIST_DESTRUCT`s exist for the same
  re-init-cleanliness reason (a tool may init→finalize→init).
- The event teardown is guarded by file-scope booleans
  (`stdin_setup`, `stdinsig_setup`, `keepalive_fd`) rather than
  re-derived, because `PMIX_FWD_STDIN` and `PMIX_KEEPALIVE_PIPE` were
  directives to *init* and finalize cannot see them.

## The multi-server model

This is the biggest thing that distinguishes a tool from a client. A
client has exactly one server (`pmix_client_globals.myserver`). A tool
can be connected to **several** servers at once and switch which one is
"primary":

- Every server the tool knows is a `pmix_peer_t` held in
  **`pmix_server_globals.clients`** (yes, the *server* globals array — a
  tool reuses it to track its upstream servers). The current primary is
  `pmix_client_globals.myserver`, which also aliases one entry of that
  array (or `pmix_globals.mypeer` when disconnected/self-assigned).
- `PMIx_tool_attach_to_server` / `PMIx_tool_connect_to_server` →
  `pmix_tool_retry_attach`: opens a new connection, adds the peer to the
  clients array, and — if `PMIX_PRIMARY_SERVER` was requested — repoints
  `myserver` and updates the stored `PMIX_SERVER_NSPACE`/`RANK`/`URI`.
- `PMIx_tool_set_server` → `pmix_tool_retry_set`: switches the primary to
  an **already-known** server, optionally polling
  (`PMIX_WAIT_FOR_CONNECTION`, driven by re-arming
  `PMIX_THREADSHIFT_DELAY` and counting down a `PMIX_TIMEOUT`-derived
  budget stored in `cb->status`).
- `PMIx_tool_disconnect` → `disc`: removes a server from the clients
  array; if it was the primary, repoints `myserver` at `mypeer` and marks
  us disconnected.
- `PMIx_tool_get_servers` → `getsrvrs`: snapshots the clients array
  (primary first) into a caller-owned `pmix_proc_t[]`.

All five follow the standard **blocking public API → `PMIX_THREADSHIFT` →
handler on the progress thread → `PMIX_WAIT_THREAD`** shape, because they
mutate the global server arrays and must run on the progress thread. The
handlers (`disc`, `getsrvrs`, `pmix_tool_retry_set`,
`pmix_tool_retry_attach`) are the progress-thread bodies; the public
functions are their thin waiters. `PMIX_ACQUIRE_OBJECT` /
`PMIX_POST_OBJECT` bracket the handler's access to the caddy, per the
memory-ordering rules.

> **Refcount discipline when repointing `myserver` is easy to get wrong.**
> The invariant is: **`myserver` holds its own reference, distinct from
> the clients-array entry's reference.** Every `myserver = X` is paired
> with `PMIX_RETAIN(X)` and a `PMIX_RELEASE` of the outgoing `myserver`;
> finalize releases once per array entry *and* once for the `myserver`
> global. When you touch any of these paths, trace the whole
> attach → set → disconnect → finalize lifecycle rather than reasoning
> about one site.

> **Only a real server goes in the clients array.** On the
> do-not-connect and failed-optional-connect paths `myserver` is a
> stand-in peer for *ourselves*, and init deliberately does **not** cache
> it there (nor take the matching retain). Two reasons, and both bit:
> `PMIx_tool_get_servers` would otherwise report an unconnected tool as
> its own server, where the Standard requires `PMIX_ERR_UNREACH`; and
> that stand-in may carry a **NULL nspace**, which `PMIX_CHECK_NSPACE`
> treats as a **wildcard**, so it matched the first lookup `disc()` or
> `pmix_tool_retry_set()` performed for any name at all. That wildcard is
> the sharpest edge in this file — anywhere you compare a caller-supplied
> `{nspace,rank}` against a peer, an unnamed argument matches
> *everything*. `disc()` now refuses one outright.

> **Pointing `myserver` at yourself means you are NOT connected.**
> `disc()` unsets `pmix_globals.connected` when it drops the primary;
> `pmix_tool_retry_set`'s "switch back to me" branch must do the same.
> It used to *set* the flag, and finalize then sent the finalize
> handshake to our own already-finalized peer, got `PMIX_ERR_UNREACH`,
> and returned it — before doing any teardown at all.

## The tool-to-tool relay (`pmix_tool_ops.c`)

A tool has no clients of its own (it is a tool, not a server), but
another tool can connect to it as that tool's primary server — e.g. a
debugger fork/exec's a launcher and points the launcher at the debugger.
When such a downstream tool sends a command the receiving tool cannot
service itself (a `spawn`), the server switchyard calls
`pmix_tool_relay_op`, which:

1. rejects anything not in the small `relaycmds` allow-list (only
   `PMIX_SPAWNNB_CMD` today) with `PMIX_ERR_NOT_SUPPORTED`;
2. requires we are actually `connected` to an upstream server;
3. copies the original request payload into a fresh buffer and
   `PMIX_PTL_SEND_RECV`s it to *our* `myserver`, stashing the original
   requester's peer + reply tag in a `pmix_shift_caddy_t`
   (`s->ncodes` reuse = the tag);
4. on the reply, `tool_switchyard` copies the answer back and
   `PMIX_SERVER_QUEUE_REPLY`s it to the original downstream tool at its
   saved tag.

If you add a relayable command, extend `relaycmds` **and** make sure the
response path round-trips correctly (the spawn case needs the separate
cbfunc precisely so the response can be intercepted and forwarded).

## PTL receive callbacks (in `pmix_tool.c`)

These are posted during init and fire on the progress thread:

- **`pmix_tool_notify_recv`** (the `rte_init` notification handler):
  unpacks an event, starts a local event chain, and — if the range isn't
  local and the event didn't originate from our own server — relays it
  onward via `pmix_notify_server_of_event`, then hands it to
  `pmix_server_notify_client_of_event`. A zero-byte buffer means the
  connection dropped; return immediately.
- **`tool_iof_handler`** (`PMIX_PTL_TAG_IOF`): unpacks forwarded stdout/
  stderr, dispatches to the registered `pmix_iof_req_t` cbfunc (looked up
  by refid in `pmix_globals.iof_requests`) or writes it to the local
  channel. The default request is installed at index 0 in init.
- **`job_data`**: the blocking `PMIX_REQ_CMD` reply handler; stores job
  info via `PMIX_GDS_STORE_JOB_INFO` and wakes the init waiter.
- **`notification_fn`** + **`evhandler_reg_callbk`**: the one-shot
  debugger-release handler used by the launcher rendezvous path.

The **zero-byte-buffer = lost connection** convention applies to every
one of these — check `PMIX_BUFFER_IS_EMPTY` / `0 == bytes_used` before
unpacking, exactly as the client recv callbacks do.

## Invariants and gotchas

- **A tool's `myserver` may be itself.** On the do-not-connect /
  connect-optional / disconnected paths, `myserver` points at
  `pmix_globals.mypeer` and `pmix_globals.connected` is unset. Guard
  server round-trips on `pmix_atomic_check_bool(&pmix_globals.connected)`
  (as `pmix_tool_relay_op` does), not on `myserver != NULL`.
- **`pmix_server_globals` is live even in a plain tool.** Do not assume
  server structures are absent just because no server module was set.
- **Tools are pinned to the `hash` gds** for server interaction; do not
  "generalize" that selection.
- **The two role booleans that gate server-only work** are
  `PMIX_PEER_IS_LAUNCHER(mypeer)` and `PMIX_PEER_IS_SCHEDULER(mypeer)`
  (listener, pfexec, pnet, hwloc). Keep the same guard on both the
  init-side setup and the finalize-side teardown or you will leak or
  double-free.
- **Init is reference counted and non-unwinding**; finalize is reference
  counted and re-init-safe. Any state you add in init must be reset or
  freed in finalize so an init→finalize→init cycle stays clean — this is
  why finalize destructs so many lists it did not create in this file.
- **Wire compatibility** applies to everything these callbacks pack and
  unpack (the `PMIX_REQ_CMD`/`PMIX_FINALIZE_CMD`/notify/IOF payloads):
  append-only, tolerate `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` on
  trailing reads (see the range unpack in `pmix_tool_notify_recv`), never
  reorder.
- **Prefer a new attribute over a new API**, and give any new API the
  `pmix_info_t info[], size_t ninfo` pair — the tool API already follows
  this (every `PMIx_tool_*` call carries it).
- **`PMIx_tool_init` does not unwind, and a failed one is terminal for
  the process.** Every early `return` leaves the one-time-init latch set
  and the counter bumped, so a caller cannot retry — and cannot finalize
  either, because finalize refuses when `initialized` is false. This is
  why the argument screens are at the top and why a test that wants to
  see init *fail* has to do it in a forked child (see `tool_api.c`).
- **Finalize must not abandon the teardown.** By the time it can fail it
  has already dropped `initialized` and the latch, so a caller that saw
  an error could neither retry nor finalize again — it would simply leak
  the whole library. Record a failure and keep tearing down.
- **`pmix_event_signal_set` is `event_assign`, not `event_add`.** It
  only initializes the event; without a following `pmix_event_add` the
  handler is never installed. The tool's SIGCONT/stdin handler was in
  that state for a long time and silently did nothing.
- **Anything init registers on an event base, finalize has to take
  down** — the stdin read event, the SIGCONT event, the keepalive-pipe
  event. `pmix_rte_finalize` destroys the bases underneath them, and the
  two that are file-scope statics are re-`PMIX_CONSTRUCT`ed by the next
  init. Note the one trap in that teardown: `pmix_iof_read_event_t`'s
  destructor closes whatever fd it holds, and the tool's is the
  application's own **stdin** — clear the field before destructing.
- **`pmix_server_globals.module_set` is a global that no finalize
  resets.** `PMIx_tool_init` clears it where it memsets
  `pmix_host_server`, so the two always agree; without that a cycling
  tool is refused its server module on every init after the first while
  the table it would be called through is the all-zero one.
- **Identity is not only `pmix_globals.myid`.** `PMIx_Get` answers
  `PMIX_PROCID` and `PMIX_RANK` out of `pmix_globals.myidval` /
  `myrankval` when the caller passes `PMIX_GET_POINTER_VALUES`.
  `pmix_rte_init` builds those carrying a NULL nspace and
  `PMIX_RANK_INVALID`; filling them in is the role's job, and the tool
  did not until this review.

## Known issues

Found during review and documented here so they are not "rediscovered"
as intended behavior. The July 2026 items (1-3) are kept because the
invariants they establish are still the ones to reason from.

1. **FIXED — `PMIX_PARENT_ID` freed a static in the launcher-rendezvous
   path.** The code set `kptr->value->data.proc = &myparent;` (a
   file-scope static) on a `PMIX_PROC`-typed kval and then
   `PMIX_RELEASE(kptr)` — whose destructor runs `value_destruct` →
   `proc_free` → `free()` on the static: heap corruption. Now
   heap-allocates the proc. Note `myparent` is still only ever loaded
   with `{NULL, RANK_UNDEF}` and never populated with a real parent — the
   *server* has the same quirk — so the stored parent id is currently
   meaningless; a benign functional gap, not a crash.
2. **FIXED — `mypeer->info` was allocated twice**, orphaning the first
   `pmix_rank_info_t` and discarding the uid/gid/pid stored on it. The
   client is the single-allocation reference.
3. **FIXED — the `myserver`-switch family mismatched the finalize
   teardown's refcount model, causing a double-free.** See the
   multi-server section above for the invariant. Covered by
   `test/simple/tool_server_switch` (`make check` via
   `test/unit/run_toolswitch.pl`).

The August 2026 sweep found the following. Each is fixed; the tests
named are the ones that fail against the unfixed library.

4. **FIXED — the launcher-rendezvous debugger-release registration hung
   and corrupted an object header.** `evhandler_reg_callbk` cast its
   `cbdata` to a `pmix_lock_t`, but the registration machinery invokes it
   with `cd->cbdata`, which the one call site sets to the
   `pmix_rshift_caddy_t` itself. So it wrote the status over the object
   header and took a "mutex" that was really the rest of that header —
   and it left the caddy's own lock, the one `PMIx_tool_init` blocks on,
   untouched, so init never returned. `src/client` (via `mycbfn`) and
   `src/server` both had this right; only `src/tool` did not. The block
   also constructed a `reglock` it never used or destructed, a leftover
   from the shape this was meant to have. **`test/unit/tool_rndz`**;
   against the unfixed library it aborts on the object magic-id assert
   and then hangs.
5. **FIXED — leaks on the multi-server and rendezvous paths.**
   `pmix_tool_retry_attach` leaked the URI `connect_to_peer` hands back
   whenever the new server was not requested as primary;
   `tool_switchyard` leaked its shift caddy — and the peer reference it
   holds — when the payload copy failed; the rendezvous caddy never set
   `infocopy`, so its three directives leaked on every launcher init; the
   `myserver->info` rank-info was allocated a second time on both
   self-assign paths (the same defect as item 2, one object over); and
   `pmix_tool_init_info` released nothing on any of its error paths.
6. **FIXED — `PMIx_Get` of our own `PMIX_PROCID`/`PMIX_RANK` under
   `PMIX_GET_POINTER_VALUES` returned garbage.** The tool never populated
   `pmix_globals.myidval`/`myrankval`. **`test/unit/tool_api`.**
   `pmix_tool_init_info` also stored `PMIX_RANK` as a `PMIX_INT`; it is a
   `pmix_rank_t` and must be `PMIX_PROC_RANK`.
7. **FIXED — `PMIX_WAIT_FOR_CONNECTION` never waited.**
   `PMIx_tool_set_server` loaded its retry budget straight from
   `PMIX_TIMEOUT`, so an absent or zero timeout — which the Standard
   defines as "never time out" — expired on the first retry and reported
   `PMIX_ERR_NOT_FOUND` rather than the documented `PMIX_ERR_TIMEOUT`.
   **`test/unit/tool_api`** and `test/simple/tool_server_switch`.
8. **FIXED — a tool that was not connected reported itself as its own
   server, and `set_server(self)` claimed we were connected.** See the
   two notes in the multi-server section; between them, `set_server(self)`
   followed by `PMIx_tool_finalize` returned `PMIX_ERR_UNREACH` **before
   any teardown ran at all**. Finalize no longer abandons the teardown on
   a failed handshake. **`test/unit/tool_api`.**
9. **FIXED — the SIGCONT handler for forwarded stdin was never
   installed**, because `pmix_event_signal_set` is `event_assign` and
   nothing added the event. A tool that is backgrounded and then resumed
   never reconsidered its stdin. Not unit-testable — it needs a tty — so
   there is no regression test; the sibling defect in
   `src/common/pmix_iof.c` is **still open**, see below.
10. **FIXED — init's file-scope events were never taken down.** The stdin
    read event, the SIGCONT event and the keepalive-pipe event all
    survived finalize; the keepalive pipe's descriptor leaked one per
    init/finalize cycle. **`test/unit/tool_cycle`** now runs a second pass
    with `PMIX_FWD_STDIN` and asserts the library did not close the
    caller's stdin on the way out.
11. **FIXED — `PMIx_tool_set_server_module` was refused on every cycle
    after the first**, because nothing resets
    `pmix_server_globals.module_set`. **`test/unit/tool_api`.**
12. **FIXED — the rendezvous block restored the wrong primary server.**
    A tool that came up with `PMIX_TOOL_DO_NOT_CONNECT` *and* its own
    `PMIX_TOOL_NSPACE`/`PMIX_TOOL_RANK` saves its "current server" from a
    stand-in peer that carries no name, and an empty nspace is a wildcard
    — so the restore matched the debugger and the launcher carried on with
    the debugger as its primary. **`test/unit/tool_rndz`** comes up in
    exactly that configuration.
13. **FIXED — a late finalize reply woke a destroyed lock.** The
    five-second guard timer clears `tev.active` and `PMIx_tool_finalize`
    then destructs the lock, but `finwait_cbfunc` woke it
    unconditionally. `src/client`'s identically-shaped callback guards on
    the flag. No test: it needs a server that answers the finalize
    handshake later than five seconds.
14. **FIXED (consistency, not an observed failure) —
    `pmix_tool_notify_recv` never stored the unpacked range on the chain
    and never ran `pmix_prep_event_chain`.** Those fields are what
    `_notify_complete` builds a *parked* event out of when no handler
    matches, so a parked event would carry `PMIX_RANGE_UNDEF` and no
    affected procs. No arrangement was found that gets a server-forwarded
    event into that branch — the server filters on the tool's registered
    codes and affected procs before forwarding — so treat this as keeping
    the two recv paths saying the same thing.
15. **FIXED — malformed directives were dereferenced.**
    `PMIX_TOOL_NSPACE`, `PMIX_SERVER_TMPDIR` and `PMIX_SYSTEM_TMPDIR` each
    went straight to `strdup()`, so a caller that supplied the key with
    the wrong type segfaulted the library. **`test/unit/tool_api`**, in
    forked children (see the non-unwinding note above).

### Still open

- **`src/common/pmix_iof.c` has the same missing `pmix_event_add` for
  its SIGCONT handler** (`stdinsig_ev`), and never releases
  `stdinev_global` either. It is the same defect as item 9 one directory
  over, but fixing it properly needs a teardown hook in `src/common` that
  this review did not design. Left for a `src/common` change.
- **Whether a *launcher* should relay a spawn is unsettled.**
  `server_switchyard` routes to `pmix_tool_relay_op` on
  `PMIX_PEER_IS_TOOL(mypeer)`, and `PMIX_PROC_LAUNCHER` includes the
  `PMIX_PROC_TOOL` bit — so a launcher with its own server module, and
  with `pfexec` open, still relays `PMIX_SPAWNNB_CMD` upstream instead of
  servicing it. For `prun`, which asks its DVM to spawn, that is
  arguably right; for a launcher that spawns locally it is not. A more
  precise guard would be "we have no host module to service it"
  (`!pmix_server_globals.module_set`), but changing it is a behavioral
  decision about PRRTE's launchers, not a bug fix. Left alone
  deliberately.
- **The relay assumes both peers negotiated the same bfrops module and
  buffer type.** `pmix_tool_relay_op` copies the downstream tool's raw
  payload into a fresh buffer and sends it to `myserver` unchanged, and
  `tool_switchyard` does the reverse. A tool forces the same modules on
  itself and its server, so this holds today — but it is an assumption,
  not something the code checks.

## Testing

`make check` coverage for this directory, all under
[`test/unit`](../../test/unit):

| Program | What it holds down |
|---------|--------------------|
| `tool_cycle` | init→finalize cycling, plus a shorter pass with `PMIX_FWD_STDIN` that asserts the library did not close the caller's stdin |
| `tool_api` | the API surface a single unconnected tool can reach: identity values, `set_server` wait/timeout semantics, the server list of an unconnected tool, malformed directives (in forked children), and a second full cycle that sets a server module again |
| `tool_rndz` | the whole `PMIX_LAUNCHER_RNDZ_URI` flow against a real server, including the restore of the original primary |
| `tool_evcache` | affected-process restriction on events delivered to a tool, end to end with two handlers. Its header records that it does **not** reach the caching branch, and why |
| `tool_nspace` | a connecting tool leaves exactly one namespace object on the server's list |
| `test/simple/tool_server_switch` (via `run_toolswitch.pl`) | the multi-server switch loop against two servers |

The multi-node half is
[`contrib/dockerswarm/run-tool-tests.sh`](../../contrib/dockerswarm/README.md)
(README §20): the tool and at least one of its servers on **different
nodes**, driving `examples/toolswitch.c` through attach/switch/disconnect
across hosts, a remote server dying, IOF relayed to a tool from other
nodes, and the only valgrind pass `src/tool` gets anywhere. Note what it
says it does not cover: nothing, anywhere, exercises the tool-to-tool
relay in `pmix_tool_ops.c`.

**A test that wants to see `PMIx_tool_init` fail must fork.** See the
non-unwinding invariant above.

## Building

`src/tool` compiles straight into `libpmix` via `Makefile.include` (which
appends `pmix_tool.c`/`pmix_tool_ops.c` to `sources` and
`pmix_tool_ops.h` to `headers` — there is no `Makefile.am` here).
Nothing is conditionally compiled, so a change takes effect with a plain
top-level `make` on an already-configured tree; you need
`autogen.pl`/`configure` only if you add or remove a source file. This
code emits **no `show_help` text of its own** (the topics it references —
`tool:no-server`, `module-set`, `listener-thread-start` — live in
`help-pmix-runtime.txt` / `help-pmix-server.txt`), so the
regenerate-the-help-content golden rule does not bite here, unless you
add a new topic to one of those files.

Smoke-test per the top-level guide: `make check` in `test/`, then
`./run_simptest.sh` in `test/simple/`. See the Testing section above for
what specifically covers this directory. Do **not** diagnose functional
failures against an `--enable-test-build` tree — its shimmed components
make functional tests misbehave by design (see the top-level guide).

## When modifying code here

- **Decide which role(s) your change affects** (tool / launcher /
  scheduler / client-tool) and gate it on the matching
  `PMIX_PROC_IS_*` / `PMIX_PEER_IS_*` test, mirroring the existing
  init-and-finalize guard pairs.
- **Match the nearest sibling.** The server-management handlers
  (`disc`, `getsrvrs`, `retry_set`, `retry_attach`) are near-identical
  threadshift bodies; copy one wholesale and adjust rather than inventing
  a new shape, and mirror its `PMIX_ACQUIRE_OBJECT`/`PMIX_POST_OBJECT` /
  `PMIX_WAKEUP_THREAD` discipline.
- **Trace every peer retain/release** across the attach→set→disconnect→
  finalize lifecycle when you touch server switching (see Known issues).
- **Keep init non-unwinding but leak-free**, and keep finalize
  re-init-safe: anything you allocate/construct in init must be
  freed/destructed in finalize.
- **Keep it warning-free and portable** under `--enable-devel-check`; use
  the `__pmix_attribute_*__` / `PMIX_HIDE_UNUSED_PARAMS` wrappers rather
  than bare GCC attributes.
- **Diff against `src/client` and `src/server` before believing a
  difference is intentional.** Three of the four worst defects this
  directory has had were drift: a recv callback the client had already
  fixed, a caddy field the server sets and the tool did not, a guard the
  client has on an identically-shaped callback. When the same function
  exists in two of the three roles, read all of them.
- **Screen a directive's value type before using it.** The recurring
  application-facing crash here is a `strdup()` of
  `info[n].value.data.string` for a key the caller supplied with the
  wrong type. Check `PMIX_STRING == .type` and non-NULL first.
