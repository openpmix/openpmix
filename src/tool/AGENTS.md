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
5-second timer guard (`fin_timeout`/`finwait_cbfunc`), kill any
`pfexec`-launched children (launcher/server only, **before** the progress
thread stops because killing needs it), stop the progress thread, dump
and destruct the static IOF sinks, release the client peer/pending
lists, stop listening, release the server clients array, close
`pnet`/`pmdl`, `PMIX_LIST_DESTRUCT` every `pmix_server_globals` list that
`pmix_server_initialize` builds, free the tmpdir strings, then
`pmix_rte_finalize`, release `mypeer`/`myserver`, and
`pmix_class_finalize`.

Two subtleties the current code documents inline and you must keep:

- It resets `pmix_globals.init_called = false` so a *subsequent*
  `PMIx_tool_init` in the same process starts fresh instead of
  short-circuiting on the latch.
- It frees and NULLs `pmix_server_globals.tmpdir` / `system_tmpdir`
  because the init-time guards only refresh them when NULL — a surviving
  pointer would silently ignore a changed `PMIX_SERVER_TMPDIR` on the
  next cycle. The many `PMIX_LIST_DESTRUCT`s exist for the same
  re-init-cleanliness reason (a tool may init→finalize→init).

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
> `pmix_tool_retry_set` `PMIX_RETAIN`s the peer before aliasing it into
> `myserver` (the peer is already owned by the clients array); the init
> path `PMIX_RETAIN`s the initial server before adding it to the array
> (line ~838); but `pmix_tool_retry_attach` adds a freshly-`PMIX_NEW`'d
> peer and aliases `myserver` to it **without** a matching retain. When
> you touch any of these paths, trace the whole
> attach → set → disconnect → finalize lifecycle: the array release in
> finalize and the `PMIX_RELEASE(peer)` in `disc` must exactly balance
> the retains taken when the peer was installed. This asymmetry is a
> known audit hazard (see "Known issues").

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

## Known issues

Found during review and documented here so they are not "rediscovered"
as intended behavior.

1. **FIXED — `PMIX_PARENT_ID` freed a static in the launcher-rendezvous
   path.** In `PMIx_tool_init` (the `PMIX_LAUNCHER_RNDZ_URI` block) the
   code set `kptr->value->data.proc = &myparent;` (a file-scope static)
   on a `PMIX_PROC`-typed kval and then `PMIX_RELEASE(kptr)` — whose
   destructor runs `value_destruct` → `proc_free` → `free()` on the
   static: heap corruption. Now heap-allocates the proc
   (`PMIX_PROC_CREATE` + `PMIx_Proc_load`), matching
   `src/server/pmix_server.c`. Note `myparent` is still only ever loaded
   with `{NULL, RANK_UNDEF}` and never populated with a real parent — the
   *server* has the same quirk — so the stored parent id is currently
   meaningless; that is a separate, benign functional gap, not a crash.
2. **FIXED — `mypeer->info` was allocated twice.** `PMIx_tool_init`
   `PMIX_NEW`d `pmix_globals.mypeer->info` once (setting `realuid`/`uid`/
   `realgid`/`gid`/`pid`), then a second `PMIX_NEW` later overwrote the
   pointer — orphaning the first `pmix_rank_info_t` (leak) and discarding
   the identity fields (leaving them zeroed). Now the second allocation
   is removed; the existing object's `pname` is (re)set in place, so the
   uid/gid/pid survive. The client (`src/client/pmix_client.c`) is the
   single-allocation reference.
3. **FIXED — the `myserver`-switch family mismatched the finalize
   teardown's refcount model, causing a double-free.** The intended
   invariant (see the `PMIx_tool_init` comment at the
   `PMIX_RETAIN(myserver)` + clients-array-add, and the finalize teardown
   that releases *once per clients-array entry* **and** *once for the
   `myserver` global*) is: **`myserver` holds its own reference, distinct
   from the clients-array entry's reference.** Three sites violated it:
   - `pmix_tool_retry_attach` (the `cb->checked`/`PMIX_PRIMARY_SERVER`
     branch) set `myserver = peer` with **no `PMIX_RETAIN`** and **no
     release of the outgoing `myserver`**. The new primary then had one
     reference but two owners (clients array + `myserver`), so
     `PMIx_tool_finalize` released it twice → **double-free / UAF**, while
     the previous primary's `myserver` reference leaked. Reachable from
     `PMIx_tool_attach_to_server` / `PMIx_tool_connect_to_server` with
     `PMIX_PRIMARY_SERVER`, and from the internal `PMIX_LAUNCHER_RNDZ_URI`
     path.
   - `pmix_tool_retry_set` retained the incoming peer but never released
     the outgoing `myserver` → leaked one reference per switch; its
     "switch back to me" branch set `myserver = mypeer` with neither a
     retain of `mypeer` nor a release of the outgoing server.
   - `disc` released only the clients-array reference when disconnecting
     the primary, never the outgoing `myserver` reference.

   Now every `myserver = X` reassignment is paired with `PMIX_RETAIN(X)`
   and a `PMIX_RELEASE` of the outgoing `myserver` across all three
   functions (in `disc` the departing primary drops both its `myserver`
   and clients-array references, via a separate handle since
   `PMIX_RELEASE` NULLs its argument). Smoke-tested with `simptest`,
   `test/unit/tool_cycle` (1200 init/finalize cycles), and
   `test/simple/simptoolcycle` (connect+finalize cycles). The **actual
   multi-server switch path** (attach-as-primary → finalize) has no
   dedicated automated test yet; when touching this code, add one or
   validate under valgrind against a tool/debugger attach→finalize flow.

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
`./simptest` in `test/simple/`. The tool paths specifically are exercised
by the tool/debugger programs under `test/simple` (e.g. the
attach/launcher and IOF tests). Do **not** diagnose functional failures
against an `--enable-test-build` tree — its shimmed components make
functional tests misbehave by design (see the top-level guide).

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
