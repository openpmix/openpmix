# AGENTS.md: The PMIx Server Subsystem

This document orients AI agents and human contributors working in
`src/server`, the RM-facing (server-role) heart of `libpmix`. It assumes
you have already read the top-level [`AGENTS.md`](../../AGENTS.md) — the
golden rules (prefix conventions, `pmix_config.h`-first include order,
constant-on-the-left comparisons, brace-everything, `#define` logical
macros to `0`/`1`, warning-free under `--enable-devel-check`), the
**thread-safety / progress-thread model and the caddy pattern**, the
backward-compatibility and wire-format rules, the "never call a
thread-shifting `PMIx_*()` from inside the library" rule, and the
copyright-header requirement all apply here and are not repeated. This
file covers what is specific to `src/server`: the switchyard dispatch
and its reply-ownership contract, the caddy zoo, the collective-tracker
machinery, the direct-modex and group engines, and the invariants that
are easy to break.

Like `src/client` and `src/event`, **`src/server` is not an MCA
framework.** There is no component structure — seven `.c` files and one
header (`pmix_server_ops.h`) compiled straight into `libpmix` via
[`Makefile.include`](Makefile.include). But this code *drives* the MCA
frameworks heavily: nearly every command handler up-calls a `pmix_gds`,
`pmix_bfrops`, `pmix_ptl`, `pmix_pnet`, `pmix_psec`, or `pmix_preg`
module through the `PMIX_GDS_*` / `PMIX_BFROPS_*` / `PMIX_PTL_*` macros.

`src/server` is **role-aware but not role-shared** the way `src/event`
is: these functions run in a process that has taken the *server* role
(a resource manager, launcher daemon such as PRRTE, or a tool acting as
a server). A PMIx server almost always **also acts as a client** of
itself — hence the `#include "src/client/pmix_client_ops.h"` at the top
of `pmix_server.c` and the frequent `pmix_globals.mypeer` GDS fetches.

## What this directory is

A PMIx server sits between local client processes (and connected tools)
on one side and the **host environment** (the RM/launcher that
instantiated the server via `PMIx_server_init` and supplied a
`pmix_server_module_t`) on the other. Its job: accept wire commands from
clients/tools, satisfy them locally where it can, up-call the host
module where it cannot, and queue replies back. The file map:

| File | Owns |
|------|------|
| `pmix_server.c` | The public `PMIx_server_*` API, init/finalize, nspace & client registration, `setup_fork`/`setup_application`, dmodex request, IOF delivery, process-set define/delete, **the switchyard** (`server_switchyard`) and message handler, and the large family of host-callback → reply functions. The canonical reference for the caddy/thread-shift model. |
| `pmix_server_ops.c` | The per-command handlers dispatched by the switchyard (abort, commit, publish/lookup/unpublish, spawn, register/deregister events, notify-from-client, query, log, alloc, job_ctrl, monitor, credentials, IOF reg/dereg/stdin, fabric/device/session/resblk/refresh), plus **all** the server-side `PMIX_CLASS_INSTANCE` con/destructors and the two tracker helpers `pmix_server_trk_complete` / `pmix_server_set_collective_status`. |
| `pmix_server_fence.c` | The fence collective (barrier + modex data exchange) **and the shared collective-tracker engine** — `pmix_server_get_tracker`, `pmix_server_new_tracker`, `pmix_server_collect_data`, `pmix_server_trk_update`, `pmix_server_commit`. |
| `pmix_server_connect.c` | The connect / disconnect collectives (built on the same tracker engine). |
| `pmix_server_group.c` | The group collectives (construct/destruct/leave/invite) — a **two-level** block/tracker engine distinct from the fence tracker, plus the peer-lost / member-left fault paths. |
| `pmix_server_get.c` | Server-side `PMIx_Get` and direct modex (dmodex): the local-satisfy-vs-remote-fetch decision tree, the `local_reqs` / `remote_pnd` deferred-request lists, and the registration-completion re-entry points. |
| `pmix_server_resolve.c` | `resolve_peers` / `resolve_node` — prefer-the-host, else answer from local GDS on a thread-shifted local handler. |

The state all of this operates on is `pmix_server_globals` (defined at
the top of `pmix_server.c`, declared in `pmix_server_ops.h`): the
`clients` pointer-array, the `nspaces` list, the `collectives` and
`grp_collectives` lists, the `remote_pnd` / `local_reqs` dmodex lists,
`events` (client event registrations), `iof` / `iof_residuals`, `psets`,
`gdata`/`genvars`, and a bank of per-subsystem verbosity channels.

## The switchyard and the reply-ownership contract

This is the single most important thing to understand before touching
anything here. Inbound messages land in `pmix_server_message_handler`
(a registered PTL receive callback, `pmix_server.c:6252`), which calls
`server_switchyard` (`pmix_server.c:5805`). The switchyard unpacks the
`pmix_cmd_t` and dispatches on it. **The return value of every command
handler encodes who owns the reply and the caddy:**

- **Return `PMIX_SUCCESS`** ⇒ "I have taken ownership. I will (via my
  async callback) queue the client's reply and release the caddy."
  The switchyard/message-handler does nothing further.
- **Return an error status** ⇒ "I did not take ownership." The switchyard
  releases the server caddy (`PMIX_RELEASE(cd)`), and
  `pmix_server_message_handler` synthesizes a reply carrying that status
  so the client's blocked call is answered.
- **Return `PMIX_OPERATION_SUCCEEDED`** ⇒ same as the error case for
  ownership (switchyard releases the caddy), but the message handler
  maps it to `PMIX_SUCCESS` in the synthesized reply. It means "done
  atomically, nothing async pending."

Most cases follow the identical shape:

```c
if (PMIX_SOMETHING_CMD == cmd) {
    PMIX_GDS_CADDY(cd, peer, tag);          // alloc pmix_server_caddy_t, RETAIN peer
    if (PMIX_SUCCESS != (rc = pmix_server_something(peer, buf, some_cbfunc, cd))) {
        PMIX_RELEASE(cd);                    // handler declined -> we free the caddy
    }
    return rc;
}
```

`PMIX_GDS_CADDY` (`pmix_server_ops.h:226`) allocates a
`pmix_server_caddy_t`, stashes the reply `tag` in `cd->hdr.tag`, and
**RETAINs the peer** so the peer object survives until the reply is
sent. The caddy's destructor (`cddes`, `pmix_server_ops.c:3129`) releases
that peer retain plus `trk`, `info`, `query`, and `key`. So: **every
error path in a handler that returns non-SUCCESS must NOT release the
server caddy itself** (the switchyard does it); **every success path must
guarantee the caddy is released exactly once by the terminating async
callback.** Getting this wrong is the dominant bug class in this
directory (leaks when a success path forgets to release, double-frees
when an error path releases a caddy the switchyard will also release).

### The host-callback → reply pattern

For any command the local server cannot answer itself, the handler
up-calls the host module (`pmix_host_server.<fn>`), passing an internal
completion callback and the caddy as `cbdata`. That callback (e.g.
`op_cbfunc`, `query_cbfunc`, `alloc_cbfunc`, …) **may run on the host's
own thread**, so it does nothing but re-thread-shift onto the progress
thread via a `pmix_shift_caddy_t`, landing in a `_*cbfunc` handler
(`_opcbfunc`, `_qrycbfunc`, …) that packs the reply and calls
`PMIX_SERVER_QUEUE_REPLY` / `PMIX_PTL_SEND_ONEWAY`, then releases both
caddies. Every one of these callbacks opens with a
`pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)` early-out
for the finalize race. When adding a command, copy the nearest existing
`cmd → handler → host_cbfunc → _host_cbfunc` quadruple verbatim and
preserve its release discipline.

Host up-calls use a tri-state return convention throughout:
`PMIX_SUCCESS` = "accepted, I will call your callback later";
`PMIX_OPERATION_SUCCEEDED` = "done now, I will *not* call back — you must
invoke the completion yourself"; any error = "rejected." Handle all
three on every up-call site.

## The caddy zoo

| Type | Defined | Role |
|------|---------|------|
| `pmix_server_caddy_t` | `pmix_globals.h:612` | The switchyard's per-request caddy. Holds the reply `hdr.tag`, the RETAINed `peer`, and optionally a `trk` back-pointer. |
| `pmix_server_trkr_t` | `pmix_globals.h:572` | Collective tracker (fence/connect/disconnect). See below. |
| `pmix_setup_caddy_t` | `pmix_server_ops.h:43` | The workhorse inner caddy for handlers that need to carry unpacked args across a thread-shift or a blocking wait (setup_app, iof, resblk, fabric, …). Its destructor frees `info`/`apps` **only when `copied==true`**. |
| `pmix_shift_caddy_t` | `pmix_globals.h:628` | Generic thread-shift caddy used to bounce host callbacks back onto the progress thread. |
| `pmix_trkr_caddy_t` | `pmix_server_ops.h:36` | Tiny caddy that carries a tracker into the event base (`PMIX_EXECUTE_COLLECTIVE`). |
| `pmix_dmdx_local_t` / `pmix_dmdx_request_t` / `pmix_dmdx_remote_t` | `pmix_server_ops.h:112/122/106` | Direct-modex deferral bookkeeping (see get.c). |
| `pmix_query_caddy_t`, `pmix_cb_t`, `pmix_inventory_rollup_t` | globals / ops.h | Query/fetch and inventory roll-up carriers. |

Any struct handed to `PMIX_THREADSHIFT` **must** carry a `pmix_event_t ev`
as its thread-shift member (the caddy contract from the top-level guide).
Do not stack-allocate a caddy that outlives its creating function — the
one deliberate exception is the blocking process-set define/delete path,
which uses a stack caddy that is safe *only* because `PMIX_WAIT_THREAD`
blocks the creator until the handler wakes it.

## Blocking vs. non-blocking public APIs

The public `PMIx_server_*` setters follow the standard pattern: when the
caller supplies a callback they thread-shift a caddy and return
immediately; when `cbfunc == NULL` they construct a local
`pmix_lock_t`, substitute an internal `opcbfunc` that wakes it, thread-
shift, then `PMIX_WAIT_THREAD` and read the status back. `_register_nspace`
/ `PMIx_server_register_nspace` is the canonical both-paths example.
Never touch `pmix_server_globals` state before the thread-shift — do it
inside the `_worker(int sd, short args, void *cbdata)` handler that runs
on the progress thread.

## The collective-tracker engine (fence / connect / disconnect)

All three of these collectives share `pmix_server_trkr_t` and the engine
in `pmix_server_fence.c`. A tracker is keyed either by an `id` string or
by the tuple `{sorted participant set, cmd type}`.
`pmix_server_get_tracker` brute-force-searches `pmix_server_globals.collectives`;
`pmix_server_new_tracker` creates one, **copies** the participant array
into `trk->pcs` (it does not take ownership of the caller's `procs`),
appends the tracker to `collectives`, and walks the participating
nspaces to compute `nlocal` (expected local contributors), `local` (all
participants are local), and `def_complete` (every participating nspace
has been registered, so `nlocal` is final).

Each contributing client's `pmix_server_caddy_t` is appended to
`trk->local_cbs` (with **no** extra retain — the list borrows the
switchyard's reference). Completion is the single predicate
`pmix_server_trk_complete` (`pmix_server_ops.c:3083`):

```
def_complete && (len(local_cbs) + len(departed)) >= nlocal
```

`departed` holds local participants whose connection dropped **before**
contributing; the `>=` is deliberately tolerant of over-count from
fork/exec'd clones. Participation is tracked **by identity**: a peer that
already contributed (is on `local_cbs`) is never moved to `departed`, so
its loss can neither complete the collective early nor discard its data.

**The tracker lifecycle contract (memorize this — it is the source of
the trickiest bugs):**

- A tracker's `PMIX_NEW` reference is the *only* reference; being on the
  `collectives` list is **not** a refcount.
- The tracker destructor `tdes` (`pmix_server_ops.c:3052`) `PMIX_LIST_DESTRUCT`s
  `local_cbs` — thereby releasing every contributor caddy — but it does
  **not** unlink the tracker from `collectives`.
- Therefore the *only* correct teardown is: cancel any armed timer,
  `pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super)`,
  then `PMIX_RELEASE(trk)` — exactly once. The canonical completion
  paths (`_mdxcbfunc`, `_cnct`/`_discnct` in `pmix_server.c`) do this.
- **Never `PMIX_RELEASE` a tracker that is still linked in `collectives`**
  (dangling pointer → UAF on the next sweep), and **never release a
  tracker whose `local_cbs` still holds a caddy that the switchyard will
  also release** on a non-SUCCESS return (double free). When a host
  up-call is rejected, remove the current `cd` from `local_cbs` (and set
  `cd->trk = NULL`) *before* returning the error, as the connect
  host-error path at `pmix_server_connect.c:421-434` demonstrates.

The completion status is recorded into the tracker's info array by
`pmix_server_set_collective_status`, which locates the
`PMIX_LOCAL_COLLECTIVE_STATUS` slot **by key, never by position** —
connect appends per-participant endpoint info and job-level info *after*
that slot, so a positional write would clobber it.

Timers are armed with `PMIX_THREADSHIFT_DELAY(trk, ..._timeout, secs)`
guarded by `!trk->event_active`, and the fence family does **not** add a
retain when arming (the collectives-list reference is the sole
reference; completion cancels the timer before releasing). Follow the
fence pattern precisely — arming a timer with an unbalanced `PMIX_RETAIN`
or without the `event_active` guard leaks the tracker.

### Data collection

`pmix_server_collect_data` bundles the modex: for `PMIX_COLLECT_YES` it
fetches each local proc's `PMIX_REMOTE`-scope kvs from GDS, packs per-rank
blobs, prepends a collect-type byte (so a receiver can detect a mismatched
`PMIX_COLLECT` flag across participants — see the `collection-mismatch`
help message), optionally compresses, and packs the whole thing as one
byte object. `pmix_server_commit` is the reverse ingest and then wakes
any pending `remote_pnd` and `local_reqs` direct-modex requests.

## Direct modex (`pmix_server_get.c`)

`pmix_server_get` runs on the progress thread and decides, in order,
whether it can satisfy a `PMIx_Get` locally or must defer/fetch:
PSET_NAMES special-case → unknown-nspace (ask host unless `localonly`) →
WILDCARD (job-level data) → `!all_registered` (park until registration
completes) → local-vs-remote determination → key-availability probe →
`_satisfy_request` (final local attempt) → `request:` (defer). Deferred
requests are parked on two list types:

- `pmix_dmdx_local_t` — one per target `{nspace, rank}`, on
  `pmix_server_globals.local_reqs`; owns its `info` array and a
  `loc_reqs` list.
- `pmix_dmdx_request_t` — one per interested local requester, on the
  lcd's `loc_reqs`; holds a **PMIX_RETAIN on its lcd** and caches the
  requester's `cbfunc`/`cbdata` (the `cd`).

When data arrives the host calls `dmdx_cbfunc` (off-thread) →
thread-shift → `_process_dmdx_reply` stores the returned data into GDS
then calls `pmix_pending_resolve` to drain every parked requester. Two
public re-entry points feed this engine from the registration flow:
`pmix_pending_nspace_requests` (fires deferred `direct_modex` once all
locals are known) and `pmix_pending_resolve` (drains trackers when data
lands or on commit). `get_timeout` fails a single waiting requester with
`PMIX_ERR_TIMEOUT`.

Note the recurring byte-object idiom: `PMIX_UNLOAD_BUFFER(&buf, bo.bytes,
bo.size)` transfers the buffer's memory into `bo.bytes`; packing that
`bo` as a `PMIX_BYTE_OBJECT` **copies** the bytes into the reply, so
`bo.bytes` must then be freed (`PMIX_BYTE_OBJECT_DESTRUCT(&bo)` — see
`_satisfy_request`, which does this correctly). Forgetting the destruct
leaks on every fetch.

## Group collectives (`pmix_server_group.c`)

Groups use a **two-level** engine distinct from the fence tracker. A
`grp_block_t` (one per group id, on `pmix_server_globals.grp_collectives`)
owns a list `mbrs` of `grp_trk_t` (one per participating
`pmix_server_group()` call); each `grp_trk_t` owns a `local_cbs` list of
participant caddies and a non-owning `blk` back-pointer (deliberately not
retained to avoid a refcount cycle — see `gtdes`). `blk->departed`
records members lost before contributing. Completion runs through
`grpcbfunc` → thread-shift → `_grpcbfunc`, which stores any group info
(tagged with the context id assigned by the *host*, not here), replies to
every participant caddy, optionally synthesizes `PMIX_GROUP_MEMBER_FAILED`
events for departed members, then unlinks and releases the block. The
fault paths `pmix_server_grp_peer_lost` (connection dropped) and
`pmix_server_grp_member_left` (voluntary `PMIx_Group_leave`) both funnel
to `account_departed`. The same release-before-detach discipline as the
fence tracker applies: releasing a block destructs its `grp_trk_t`s and
their `local_cbs`, so remove the current `cd` from `local_cbs` before
releasing on a host-error return, or the switchyard double-frees it.

## Peer lifecycle, tombstones, and event purging

`pmix_server_peer_finalized` (`pmix_server.c:1495`) and its helper
`remove_client` implement the **tombstone** model documented in the
header comment (`pmix_server_ops.h:350`): a cleanly-finalized local
client whose socket dropped is left in place as an inert finalized peer
at its `clients` slot (its `info->peerid` unchanged, slot not nulled),
to be reclaimed at the next reconnect for that rank or at namespace
deregistration. Only a *stranded* peer that a newer connection has
already displaced is freed immediately. This deferral keeps concurrent
spawn/connect/disconnect collectives and direct-modex gets — which
resolve ranks through `info->peerid` — from racing peer teardown. Do not
"simplify" this by freeing peers eagerly. `pmix_server_purge_events`
scrubs a departing peer from the event-registration store and pending
notifications.

## IOF forwarding

Standard-I/O forwarding is buffered through `pmix_server_globals.iof`
(cache of `pmix_iof_cache_t`) with a `max_iof_cache` bound and an
`iof_residuals` list for partial (pre-newline) bytes. `PMIx_server_IOF_deliver`
is the public push; `pmix_server_iofreg` / `iofdereg` / `iofstdin` are
the client-driven pull/registration commands. Note `pmix_server_process_iof`
drains the cache into a newly registered request so a late subscriber
still sees recently produced output.

## Wire format and interoperability

Everything crossing the socket obeys the top-level Version
Interoperability rules: append-only message layouts, tolerate short
buffers from older peers, never reorder. Concrete patterns you must
preserve:

- **V1 gating.** Several replies are conditioned on `!PMIX_PEER_IS_V1(peer)`
  (e.g. the commit ack). A v1 client must see exactly the bytes v1
  expected.
- **Version-gated fields** are unpacked defensively — e.g. `log`
  version-gates its timestamp unpack on `PMIX_PEER_IS_EARLIER(peer,3,0,0)`.
- **The command enum `pmix_cmd_t` is a wire value** (in
  `src/include/pmix_globals.h`). New commands append at the end; the
  switchyard grows a new `if (PMIX_NEW_CMD == cmd)` block; never
  renumber. `PMIX_GDS_FALLBACK_CMD` is the reference for how a command
  was added.
- A GET/collect byte-object payload is packed as an opaque
  `PMIX_BYTE_OBJECT` precisely so peers that don't understand its
  interior can still relay it.

Prefer a new attribute over a new command or API (top-level "Role of
Attributes"): most server-side extension is a new `PMIX_*` directive
parsed inside an existing handler, not a new switchyard case.

## Building

`src/server` compiles straight into `libpmix` via
[`Makefile.include`](Makefile.include), which appends the seven `.c`
files to the top-level `sources` list and `pmix_server_ops.h` to
`headers`. Nothing here is conditionally compiled, so a change takes
effect with a plain top-level `make` on an already-configured tree; you
need `autogen.pl` + `configure` only when adding or removing a source
file. `help-pmix-server.txt` is a `show_help` file — if you add, remove,
or edit any message in it, follow the top-level rule and rebuild the
generated `show_help` content (`rm src/util/pmix_show_help_content.* &&
make`).

After building, smoke-test per the top-level guide: `make check` in
`test/`, then `./simptest` in `test/simple/`. The server paths are
exercised by essentially every `test/simple` program — `simptest`
(fence/get), the connect/disconnect and group tests, the tool/debugger
tests (`simpdmodex`, `gwtest`, the pub/lookup tests), and the IOF tests.
Several server behaviors also have dedicated unit tests under
`test/unit` wired into `make check` (e.g. `collective_status.c` shares
`pmix_server_set_collective_status`). Prefer extending those over manual
checks. Do **not** diagnose functional failures against an
`--enable-test-build` tree (the shimmed components make functional tests
misbehave by design).

## When modifying code here

- **Trace the reply-ownership contract on every path.** For each command
  handler and its callbacks, confirm three things on every early-return
  and completion branch: the server caddy is released **exactly once**
  (by the switchyard on a non-SUCCESS return, or by the terminating
  callback on SUCCESS — never both, never neither); the client's reply is
  queued exactly once; and any locally allocated inner caddy / buffer /
  proc array / info array is freed on the paths that don't hand it off.
  Success-path leaks and error-path double-frees are the two dominant bug
  classes in this directory.
- **Respect the tracker lifecycle:** cancel timer → unlink from
  `collectives`/`grp_collectives` → release, exactly once; and detach the
  current `cd` from `local_cbs` before releasing a tracker/block on a
  host-error return.
- **Free every unloaded byte object** after packing it (`PMIX_UNLOAD_BUFFER`
  + pack `PMIX_BYTE_OBJECT` copies, so the unload allocation is yours to
  free).
- **Handle all three host up-call return codes** (`PMIX_SUCCESS`,
  `PMIX_OPERATION_SUCCEEDED`, error) at every `pmix_host_server.*` site.
- **Never touch `pmix_server_globals` off the progress thread** — thread-
  shift first; re-thread-shift host callbacks that may arrive on the
  host's thread.
- **Match the nearest sibling handler exactly.** With ~20 near-identical
  `cmd → handler → *_cbfunc → _*cbfunc` quadruples, the safest way to add
  or change one is to diff it against a working neighbor and mirror its
  ownership discipline line for line.
- **Preserve wire compatibility:** V1/version gating, append-only layouts,
  tolerate-short-buffer unpacks.
