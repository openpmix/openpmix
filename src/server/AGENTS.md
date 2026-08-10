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
framework.** There is no component structure — twenty `.c` files and
one header (`pmix_server_ops.h`) compiled straight into `libpmix` via
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
| `pmix_server.c` | `PMIx_server_init` / `PMIx_server_finalize` and the machinery they stand up: the `pmix_server_globals` definition, the MCA-parameter registration, the singleton and parent-died plumbing, the server's own event handler, and `PMIx_server_setup_fork` (which passes the modes negotiated at init down to a child). Also `pmix_server_lock_opcbfunc`, the completion callback every blocking public API substitutes when its caller passes no callback. |
| `pmix_server_switchyard.c` | **The switchyard** (`server_switchyard`), `pmix_server_message_handler`, and the generic status-only replies that a dozen arms hand out by name — `op_cbfunc`, `op_cbfunc2`, `resop_cbfunc` and their `_`-prefixed handlers. These stay `static` because the switchyard is in the same file. The canonical reference for the reply-ownership contract below. |
| `pmix_server_op_replies.c` | The host callbacks that complete a specific client operation and carry that operation's own payload: fence modex, direct-modex get, connect, disconnect, spawn, lookup, event registration, IOF register and deregister. |
| `pmix_server_info_replies.c` | The host callbacks that hand host-supplied results back — alloc, query, session control, job control, monitor, credential get and validate, fabric, device distances, resolve peers and node. All eleven share one shape, so diff a new one against its neighbor here. |
| `pmix_server_registration.c` | `PMIx_server_register_nspace` / `deregister_nspace` and `register_client` / `deregister_client`, plus the peer-departure bookkeeping that mirrors them (`remove_client`, `pmix_server_peer_finalized`, `pmix_server_purge_events`) and `pmix_server_execute_collective`, which fires a collective the registration just unblocked. The canonical both-paths (blocking and non-blocking) example. |
| `pmix_server_setup.c` | Everything that prepares a job before its processes run: `PMIx_server_register_resources` / `deregister_resources`, `PMIx_server_setup_application`, `PMIx_server_setup_local_support`, and the helper APIs a host uses to build that information (`PMIx_generate_regex`/`_ppn`, `PMIx_parse_regex2`, `PMIx_server_generate_locality_string`/`_cpuset_string`/`_cpuset`). |
| `pmix_server_dmodex.c` | The data calls the host makes down into us: `PMIx_server_dmodex_request` (modex data for one of our local clients) and `PMIx_Store_internal`. |
| `pmix_server_iof.c` | Standard-I/O forwarding: `pmix_server_iof_handler` (the PTL receive callback for output arriving from our host or another server), `PMIx_server_IOF_deliver`, `PMIx_server_IOF_flow_control`, and the client-driven commands `pmix_server_iofreg` / `iofdereg` / `iofstdin` plus `pmix_server_process_iof`. |
| `pmix_server_inventory.c` | `PMIx_server_collect_inventory` / `PMIx_server_deliver_inventory`. |
| `pmix_server_pset.c` | `PMIx_server_define_process_set` / `PMIx_server_delete_process_set`. |
| `pmix_server_ops.c` | The classic client commands dispatched by the switchyard: abort, publish/lookup/unpublish, spawn (including the fork/exec fall-through and `pmix_server_spawn_parser`). Also defines `pmix_host_server`, the host module the RM handed us at init. |
| `pmix_server_events.c` | The event family: `register_events` / `deregister_events`, the registration store and the bookkeeping that decides when our host must start or stop forwarding a code (`pmix_server_activate_events`, `pmix_server_deactivate_events`, `pmix_server_prune_reginfo`), the cached-event replay for a late registrant, and `pmix_server_event_recvd_from_client`. |
| `pmix_server_control.c` | The directive and service commands: query, log, allocate, job control, monitor, credential get and validate, session control, resource blocks, and the job-data cache refresh. |
| `pmix_server_fabric.c` | `fabric_register` / `fabric_update` and the device-distance computation. |
| `pmix_server_classes.c` | The `PMIX_CLASS_INSTANCE` con/destructors for every type declared in `pmix_server_ops.h` or `pmix_globals.h`, so the ownership rules a destructor encodes can be read in one place. A type private to a single file keeps its class beside it — `grp_block_t`/`grp_trk_t`/`grp_shifter_t` in `pmix_server_group.c`, `rank_blob_t` in `pmix_server_fence.c`, `pmix_dmdx_reply_caddy_t` in `pmix_server_get.c`, `pmix_srvr_epi_caddy_t` in `pmix_server_control.c`. |
| `pmix_server_fence.c` | The fence collective (barrier + modex data exchange) **and the shared collective-tracker engine** — `pmix_server_get_tracker`, `pmix_server_new_tracker`, `pmix_server_collect_data`, `pmix_server_trk_update`, `pmix_server_commit`, plus the two predicates every family consults: `pmix_server_trk_complete` and `pmix_server_set_collective_status`. |
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
(a registered PTL receive callback,
`pmix_server_switchyard.c:727`), which calls `server_switchyard`
(`pmix_server_switchyard.c:258`). The switchyard unpacks the
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

`PMIX_GDS_CADDY` (`pmix_server_ops.h:232`) allocates a
`pmix_server_caddy_t`, stashes the reply `tag` in `cd->hdr.tag`, and
**RETAINs the peer** so the peer object survives until the reply is
sent. The caddy's destructor (`cddes`, `pmix_server_classes.c:125`) releases
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
`op_cbfunc`, `pmix_server_query_cbfunc`, `pmix_server_alloc_cbfunc`, …)
**may run on the host's own thread**, so it does nothing but
re-thread-shift onto the progress thread via a `pmix_shift_caddy_t`,
landing in a `_*cbfunc` handler (`_opcbfunc`, `_qrycbfunc`, …) that packs
the reply and calls `PMIX_SERVER_QUEUE_REPLY` / `PMIX_PTL_SEND_ONEWAY`,
then releases both caddies. Every one of these callbacks opens with a
`pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)` early-out
for the finalize race. When adding a command, copy the nearest existing
`cmd → handler → host_cbfunc → _host_cbfunc` quadruple verbatim and
preserve its release discipline.

**Only the outer callback is visible outside its file.** The two reply
files are named by the switchyard, so each outer callback there carries
the `pmix_server_` prefix and is declared in `pmix_server_ops.h`
(`pmix_server_query_cbfunc`, `pmix_server_modex_cbfunc`, …). The inner
`_*cbfunc` progress-thread handler stays `static` — its only caller is
the outer callback sitting directly above it, and keeping the pair
adjacent is what makes the quadruple readable. Add a new pair the same
way: outer prefixed and declared, inner static and adjacent.

Host up-calls use a tri-state return convention throughout:
`PMIX_SUCCESS` = "accepted, I will call your callback later";
`PMIX_OPERATION_SUCCEEDED` = "done now, I will *not* call back — you must
invoke the completion yourself"; any error = "rejected." Handle all
three on every up-call site.

**A spawn is relayed when we are attached to a server and fork/exec'd
when we are not.** `server_switchyard` hands a command from a downstream
tool to `pmix_tool_relay_op` whenever *we* are a tool, and falls through
to the logic tree on **both** `PMIX_ERR_NOT_SUPPORTED` ("not a command we
relay") and `PMIX_ERR_UNREACH` ("no server to relay it to"). The second
arm is why `pmix_server_spawn` will fork/exec when it has no
`pmix_host_server.spawn` but `pfexec` is open — the same rule
`PMIx_Spawn` applies to a launcher's own requests. Two details make the
gate safe: `pmix_pfexec_base_open()` is called from exactly one place in
the tree (`PMIx_tool_init`, for a launcher or scheduler), so
`pmix_pfexec_globals.initialized` really is the "can I fork/exec"
question and a plain PMIx server is unaffected; and `pmix_tool_relay_op`
returns both statuses **before** rewinding the buffer, so the
fall-through keeps the unpack position the `cmd` read left. The pfexec
arm hands `pmix_pfexec_base_spawn_job` a *second* setup caddy that
borrows the first one's apps and directives with `copied` left false —
pfexec releases the caddy it is given, and the arrays belong to the one
holding the requester's callback. Covered by `test/unit/tool_relay`.

**`PMIX_SUCCESS` from an up-call transfers ownership of the caddy to the
host.** This is the arm most easily lost, because it is usually the one
that needs no code: the handler returns and the host's callback does the
rest. A handler whose error cleanup sits under a bare `exit:` label
therefore has to *return* on the success arm, not fall into it. Both
`pmix_server_iofreg` and `pmix_server_iofdereg` fell through and released
the caddy the host was still holding, so the host's completion ran on
freed memory — and each carried a comment describing the very discipline
the code did not implement. When you add a handler, read the success arm
and the label as one thing.

**A release function belongs to whoever gave it to you, and so does its
argument.** The host hands down a `(relfn, relcbdata)` pair; `relfn` must
be called with *that* `relcbdata` and nothing else. The temptation is to
reach for whichever pointer is in scope — the tracker, the block, our own
caddy — and several sites had. Note the two spellings are not
interchangeable across files: `pmix_server_modex_cbfunc` parks the host's data in
`scd->relcbdata`, while `grpcbfunc` parks it in `scd->cbdata`, so the
correct expression differs between `_mdxcbfunc` and `_grpcbfunc`. Check
which one the *producing* callback set before copying a line.

**Honor the release contract on the paths that "can do nothing".** Every
host callback that receives a `(relfn, relcbdata)` pair owes the host that
call, including on the arm where it has just failed to allocate its own
thread-shift caddy and returns early. Five of them - alloc, query,
session-control, job-control and monitor - returned without it, stranding
the host's data for the life of the host process. `pmix_server_modex_cbfunc` and the
fabric/dist family show the shape to copy.

**An internal completion path must hand the callback the object that
callback casts.** `_fabric_response` exists to answer a fabric request the
`pnet` layer satisfied locally, and it called the switchyard's
`pmix_server_fabric_cbfunc` with `qcd->cbdata` — a `pmix_server_caddy_t` — where that
function casts to `pmix_query_caddy_t` and dereferences the result. The
sibling `pmix_server_fabric_update` never set `qcd->cbfunc` at all. Both
are latent only because no in-tree `pnet` component answers a fabric
request; a component that did would crash immediately.

## The caddy zoo

| Type | Defined | Role |
|------|---------|------|
| `pmix_server_caddy_t` | `pmix_globals.h:612` | The switchyard's per-request caddy. Holds the reply `hdr.tag`, the RETAINed `peer`, and optionally a `trk` back-pointer. |
| `pmix_server_trkr_t` | `pmix_globals.h:572` | Collective tracker (fence/connect/disconnect). See below. |
| `pmix_setup_caddy_t` | `pmix_server_ops.h:43` | The workhorse inner caddy for handlers that need to carry unpacked args across a thread-shift or a blocking wait (setup_app, iof, resblk, fabric, …). Its destructor frees `info`/`apps` **only when `copied==true`**. |
| `pmix_shift_caddy_t` | `pmix_globals.h:628` | Generic thread-shift caddy used to bounce host callbacks back onto the progress thread. |
| `pmix_trkr_caddy_t` | `pmix_server_ops.h:36` | Tiny caddy that carries a tracker into the event base (`PMIX_EXECUTE_COLLECTIVE`). |
| `pmix_dmdx_local_t` / `pmix_dmdx_request_t` / `pmix_dmdx_remote_t` | `pmix_server_ops.h:113/123/107` | Direct-modex deferral bookkeeping (see get.c). |
| `pmix_query_caddy_t`, `pmix_cb_t`, `pmix_inventory_rollup_t` | globals / ops.h | Query/fetch and inventory roll-up carriers. |

### `pmix_setup_caddy_t` does not own its members uniformly

This is the single most error-prone thing in the caddy zoo, because
`scaddes` (`pmix_server_classes.c:182`) frees three groups of member on
three different rules, and the rule is not visible at the assignment site:

| Member | Freed by the destructor? |
|--------|--------------------------|
| `procs`/`nprocs`, `units`/`nunits`, `bo`/`nbo`, `peer`, `flags.file`/`.directory` | **Always** |
| `info`/`ninfo`, `apps`/`napps` | Only when `copied` is true |
| `nspace`, `keys` | **Never** |

So a handler that parks the *caller's* proc array or byte object on a
caddy — `PMIx_server_IOF_deliver` and `PMIx_server_IOF_flow_control` both
do, and must, per the no-copy rule — has to detach it before the caddy is
released, or the destructor calls `free()` on memory the host owns, often
a stack frame. `_iofdeliver` and `_iofflowcontrol` do exactly that in
their "release the caddy" blocks; both of their `PMIX_ERR_WOULD_BLOCK`
early returns did not, so the blocking form called from the progress
thread freed the caller's proc and byte object. `PMIx_server_define_process_set`
shows the same idiom on a stack caddy ("protect the input"). Covered by
`test/unit/iof_output.c`, whose case aborts rather than fails against an
unfixed library.

Note the asymmetry the two free helpers add: `PMIx_Proc_free(p, n)` frees
`p` only when `0 < n`, but `PMIx_Byte_object_free(b, n)` frees `b`
whenever it is non-NULL — so leaving `nbo` at zero does *not* protect a
borrowed `bo`.

The other half of the rule is the "never" row. `nspace` and `keys` are
borrowed as often as they are owned - `PMIx_Register_attributes` parks the
host's own `function` string and `attrs` array there, and the pset stack
caddies park the caller's `pset_name` - so the destructor cannot free
them and every site that *does* own one owes it a `free()` on every path
that discards the caddy. Two forgot: `pmix_server_abort` unpacks the
abort message into `nspace` (an allocation) and leaked it on every abort,
and `PMIx_server_setup_local_support` leaked its `strdup` on the
`PMIX_ERR_WOULD_BLOCK` arm. Do not "fix" this by moving `nspace` into the
destructor - that turns the leak into a free of a string literal.

A related trap sits one layer out: `pmix_server_process_iof` does
`req->flags = cd->flags`, a **shallow** copy of a `pmix_iof_flags_t` whose
`file` and `directory` members are `strdup`ed and owned by the caddy. The
`pmix_iof_req_t` outlives the caddy, so those two pointers dangle the
moment the caddy is released. It is latent only because nothing reads
`pmix_iof_req_t::flags` today (`pmix_iof_process_iof` does not); the day
something does, it needs its own copy.

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
`pmix_lock_t`, substitute the shared `pmix_server_lock_opcbfunc` that
wakes it, thread-shift, then `PMIX_WAIT_THREAD` and read the status back.
`_register_nspace` / `PMIx_server_register_nspace`
(`pmix_server_registration.c`) is the canonical both-paths example.
That waker lives in `pmix_server.c` and is declared in
`pmix_server_ops.h` because every one of these files needs it; do not
re-roll a private copy.
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
`pmix_server_trk_complete` (`pmix_server_fence.c:1189`):

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
- The tracker destructor `tdes` (`pmix_server_classes.c:93`) `PMIX_LIST_DESTRUCT`s
  `local_cbs` — thereby releasing every contributor caddy — but it does
  **not** unlink the tracker from `collectives`.
- Therefore the *only* correct teardown is: cancel any armed timer,
  `pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super)`,
  then `PMIX_RELEASE(trk)` — exactly once. The canonical completion
  paths (`_mdxcbfunc`, `_cnct`/`_discnct` in `pmix_server_op_replies.c`)
  do this.
- **Never `PMIX_RELEASE` a tracker that is still linked in `collectives`**
  (dangling pointer → UAF on the next sweep), and **never release a
  tracker whose `local_cbs` still holds a caddy that the switchyard will
  also release** on a non-SUCCESS return (double free). When a host
  up-call is rejected, remove the current `cd` from `local_cbs` *before*
  returning the error, as the connect host-error path at
  `pmix_server_connect.c:421-434` demonstrates. Those paths also set
  `cd->trk = NULL`, which is a no-op: nothing in the tree ever assigns
  `pmix_server_caddy_t::trk` a non-NULL value, so `cddes`'s release of it
  is unreachable too. Removal from `local_cbs` is what does the work.

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
or without the `event_active` guard leaks the tracker. A timeout handler
that has to tear the tracker down itself (no completion function was ever
attached) is bound by the same rule as everything else here: **unlink
from `collectives` first, then release.**

**Read `PMIX_TIMEOUT` into a variable of the width you ask for.**
`PMIx_Value_get_number(value, dest, type)` writes exactly `sizeof(type)`
bytes through `dest`. Handing it `&tv.tv_sec` while asking for
`PMIX_UINT32` therefore fills four bytes of a `time_t` — the low half on
a little-endian host, which happens to work, and the high half on a
big-endian one, which multiplies every timeout by 2^32. Convert through a
`uint32_t` of its own and assign. The fence, connect and get handlers all
do this now; `pmix_server_group` always did.

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

**That retain is why an lcd cannot be discarded by releasing it.** Each
parked request holds a reference, so `pmix_list_remove_item` +
`PMIX_RELEASE` on the lcd drops only the creation reference and leaves
the object alive and now unreachable — off `local_reqs`, so no later
`pmix_pending_resolve` can ever drain or free it, and with every request
still holding the `pmix_server_caddy_t` the switchyard is about to free.
Use `discard_local_tracker`, which drains `loc_reqs` first. It does not
invoke the requests' callbacks, and that is deliberate: the only
requester on a freshly created lcd is the caller whose error return makes
the switchyard answer it, so calling the callback would answer that
caller twice. If you ever discard an lcd that could have *other*
requesters, they do need their callbacks — follow
`pmix_pending_nspace_requests`, which drains with callbacks.

**A tracker on `local_reqs` whose target has departed must fail its
waiters, not just disappear.** `pmix_server_purge_events` runs when a peer
finalizes or a namespace is deregistered, and any direct-modex tracker
naming that peer is never going to be answered. Use
`pmix_server_fail_local_reqs`, which calls each parked requester's
callback with a status before discarding the tracker: those requesters are
*other* local procs, and nobody else is going to answer them. This is the
same reference-counting trap as the one below - the difference is only
whether the waiters deserve to be told.

**Do not read a `PMIX_LIST_FOREACH` variable after the loop.** A loop
that runs to completion leaves it pointing at the list's sentinel, which
is a bare `pmix_list_item_t`; reading any later field of the element type
out of it is undefined and returns whatever the enclosing object happens
to hold there. `pmix_server_get` did this with `iptr->peerid` on the
"target rank is not one of my local ranks" path — the common case for
every remote get — and used the garbage as an index into the `clients`
array. Note what this class of bug does *not* do: the read stays inside
the enclosing `pmix_namespace_t`, so no sanitizer flags it, and the value
is usually out of range and accidentally produces the right answer. Track
a `found` flag and gate on it.

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

## `pmix_server_resolve.c` is a twin of `src/client/pmix_client_resolve.c`

`pmix_server_locally_resolve_peers()` / `..._node()` here and
`resolve_peers()` / `resolve_nodes()` in
[`src/client/pmix_client_resolve.c`](../client/AGENTS.md) are the same
computation over the same datastore, written twice — the server answers
for a client that round-tripped, the client answers for itself when it
cannot reach a server. **Every defect found in one has so far been
present in the other, in both directions.** Read the twin before you stop
changing either.

The recurring one is that `PMIx_Argv_split()` returns **NULL** for a
string with no tokens rather than an empty array, and every list these
functions read out of the datastore can legitimately be the empty string:
a namespace's map can name a node it placed nothing on, and `store_map()`
records that node's `PMIX_LOCAL_PEERS` as `strdup("")`. The `NULL ==
string` guards throughout both files do not cover that state. Related,
and reachable from the same input: `PMIX_PROC_CREATE(0)` hands back the
same NULL an allocation failure does, so a zero peer count reported as
`PMIX_ERR_NOMEM` where the empty answer was correct.

The one that went the other way is worth keeping: the aggregate walk here
resets `proc.rank` to `PMIX_RANK_UNDEF` per namespace, with a comment
saying why, and the client's `try_fetch()` did not — so its answer
depended on the order the namespaces sat in the list. See the ninth-sweep
entry in the client's `AGENTS.md` for why that was benign against today's
`hash` module, before deciding the reset is unnecessary.

Regression coverage for the shared defects lives on the client side, in
`test/unit/resolve_api.c`; it drives the public APIs in a server process
whose stub host module has no `query`, which is exactly the configuration
that reaches these local computations.

## Peer lifecycle, tombstones, and event purging

`pmix_server_peer_finalized` (`pmix_server_registration.c:460`) and its helper
`remove_client` implement the **tombstone** model documented in the
header comment (`pmix_server_ops.h:387`): a cleanly-finalized local
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

**A tool's finalize is upcalled too** (`PMIX_CAP_TOOL_FINALIZED`). The
`PMIX_FINALIZE_CMD` arm of the switchyard calls
`pmix_host_server.client_finalized` for a peer that is a client *or* a
tool, because for a tool this is the host's only notice that it has
gone: a tool is nobody's child, so no waitpid reaches the host, and the
connection drop that follows raises no `PMIX_ERR_LOST_CONNECTION` —
`pmix_ptl_base_lost_connection` suppresses that for a peer already marked
finalized, and `op_cbfunc2` marks it. A host that skipped the call would
carry the tool's state, and anything it had been granted, for the rest of
its own lifetime; PRRTE stranded the nodes of every command-line elastic
grow that way. `peer->info` is set for a tool peer exactly as for a
client, so the `pname` read is safe; `server_object` is simply NULL for a
peer the host never registered as a client, which hosts already handle
(and is what lets PRRTE tell the two apart).

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
[`Makefile.include`](Makefile.include), which appends the twenty `.c`
files to the top-level `sources` list and `pmix_server_ops.h` to
`headers`. Nothing here is conditionally compiled, so a change takes
effect with a plain top-level `make` on an already-configured tree; you
need `autogen.pl` + `configure` only when adding or removing a source
file. `help-pmix-server.txt` is a `show_help` file — if you add, remove,
or edit any message in it, follow the top-level rule and rebuild the
generated `show_help` content (`rm src/util/pmix_show_help_content.* &&
make`).

## Testing what a single node cannot reach

Most of this directory is only half-exercised by `make check`. The
local-vs-remote decision in `pmix_server_get` has one arm on one node:
every rank is local, every key is already in the local datastore, and the
whole direct-modex engine — `local_reqs`, `remote_pnd`, `dmdx_cbfunc` →
`_process_dmdx_reply` → `pmix_pending_resolve` — never runs. That is
where the deferred-request lifetime bugs live.

Two suites cover the halves:

- [`test/unit/server_get.c`](../../test/unit/server_get.c) drives
  `pmix_server_get` directly against a registered nspace whose job size
  exceeds its local size, and asserts the classification and that nothing
  is left parked on `local_reqs`. Read its header for what it pins down
  and what it deliberately cannot reproduce.
- [`contrib/dockerswarm/run-server-tests.sh`](../../contrib/dockerswarm/run-server-tests.sh)
  is the multi-node half: direct modex, spawn-plus-connect across
  namespaces, group blocks spanning nodes, IOF pull/dereg against a
  persistent DVM, and a valgrind pass on the daemon — the only leak check
  anywhere that runs against the *server* library, since every other
  suite valgrinds a client. **Its stages put one rank per node
  deliberately**; with two ranks on a node a peer's data is answered out
  of local storage and the run can be green with the remote path broken.

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
- **A kval built to carry a *borrowed* payload must give it up before
  release.** The recurring shape here is `PMIX_KVAL_NEW(kptr, KEY)` →
  point `kptr->value` at something → `PMIX_GDS_STORE_KV` →
  `PMIX_RELEASE(kptr)`. That release runs `kvdes`, which runs
  `value_destruct`, which frees the value's payload *outright* — a
  `PMIX_DATA_ARRAY` through `data_array_free`, a `PMIX_STRING` through
  `free`. And the store does not take the payload: `pmix_hash_store`
  deep-copies it. So a value pointed at memory that is not ours hands
  that memory back to the heap while its real owner still holds it.
  Most sites are safe because they build a payload they own
  (`PMIX_PROC_CREATE`, `pmix_asprintf`); `PMIX_ALLOC_MAU` in
  `PMIx_server_init` borrowed the *host's* `pmix_data_array_t` straight
  out of the caller's `info[]`, so init freed it and the host's own free
  of it was a double free. Either copy into the value, or NULL the
  borrowed member before releasing. Covered by
  `test/unit/singleton_register`.
- **The server's own `mypeer->nptr` never reaches `pmix_globals.nspaces`,
  and nothing depends on it.** The block in `PMIx_server_init` that would
  prepend it is guarded on `NULL == mypeer->nptr`, which `pmix_rte_init`
  has already made non-NULL — so it is dead, and the comment above it
  ("ensure our own nspace is first on the list") describes an intent the
  code does not carry out. This looks like it should break the
  `PMIX_LAUNCHER_RNDZ_URI` path, where `job_data` stores our parent's
  reply under our own nspace and `hash_store_job_info` looks that nspace
  up on `pmix_globals.nspaces` before storing anything. It does not:
  `pmix_gds_hash_get_tracker(nspace, true)`, called immediately above
  that lookup, creates and appends the namespace itself. Verified by
  instrumenting both sites. Do not "fix" the dead branch on the theory
  that the lookup can fail.
- **Screen the shape of anything the host hands you before indexing it.**
  A `pmix_info_t` carrying a `PMIX_DATA_ARRAY` is a host-supplied
  structure, and several sites here read `array[0]` and `array[1]`
  positionally on the strength of a comment. Check the array is non-NULL
  and long enough first: `_register_nspace` and `_register_resources` both
  did not, and neither is reachable only from trusted code - a host is
  free to get this wrong.
- **`pmix_server_globals.system_tmpdir` is read by the directory-removal
  guard, so finalize must not free it early.** Only the string is freed
  here; the directory is removed by `pmix_ptl_close()` (inside
  `pmix_rte_finalize()`) and only if that code created it. But the second
  guard, in `pmix_os_dirpath_destroy()`, decides by comparing its path
  against this string, and treats NULL as "the caller has no system
  tmpdir of its own" - the ordinary case for a client or tool, which
  takes the *unconditional* `rmdir` arm. Free it above any
  session-directory cleanup and that guard becomes an `rmdir` of a
  system-defined directory. It sits below `pmix_rte_finalize()` for that
  reason and there is a comment at the site saying so.
- **A public API's non-blocking form still has to clean up when the
  caller passes no callback.** `PMIx_server_setup_application` and
  `PMIx_server_collect_inventory` take a callback and have no blocking
  variant, so a NULL one is legal and simply means "do it and tell me
  nothing". Both assembled a result caddy and then leaked it, because the
  only thing that would have released it was the release function handed
  to a callback that was never called.

## A note on `PMIx_Notify_event` from inside the library

The top-level rule is that back-end code must not call a public `PMIx_*`
API that thread-shifts. Several sites here do call `PMIx_Notify_event`
from the progress thread - `pdiedfn`, `psetdef`, `psetdel` - and they are
correct, which is worth knowing before "fixing" them. That entry point
blocks **only when its callback argument is NULL**; with a callback it
thread-shifts and returns, and queuing an event onto the thread you are
already running on is fine. All three pass a callback.

`pmix_server_group.c` deliberately uses the internal
`pmix_server_notify_client_of_event` instead, and its comment says the
public one "cannot be called from the progress thread". Read that as
shorthand for the blocking form. What all of these *do* owe is the return
value: a refused notification never reaches the release function, so the
payload has to be handed back by hand - `pdiedfn` always did this, and the
two pset handlers now do.
