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
| `pmix_server_fence.c` | The fence collective (barrier + modex data exchange) **and the shared collective-tracker engine** — `pmix_server_get_tracker`, `pmix_server_new_tracker`, `pmix_server_collect_data`, `pmix_server_commit`, plus the two predicates every family consults: `pmix_server_trk_complete` and `pmix_server_set_collective_status`. Also `PMIx_server_collect_job_info`, the public API a host uses to pull packed job-level info for a set of procs. |
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
sibling `pmix_server_fabric_update` did not set `qcd->cbfunc` at all.
Both were latent only because no in-tree `pnet` component answers a
fabric request — `pmix_pnet.register_fabric` returns
`PMIX_ERR_NOT_SUPPORTED` when no active component implements it, so every
arm of `pmix_server_fabric_register` above the host up-call is unreached
today. Read the rest of that function with the same caution: its
`PMIX_SUCCESS` arm answers a component that promised a callback by
`PMIX_WAIT_THREAD`-ing on the switchyard's own thread, so a component
that completed from the progress thread would deadlock the server rather
than merely crash it.

**A callback that only thread-shifts has not consumed your data.** The
`(release_fn, release_cbdata)` pair is not decoration for the host path:
`pmix_server_dist_cbfunc` and `pmix_server_fabric_cbfunc` both park their
payload on a `pmix_shift_caddy_t` and pack the reply from it later, on the
progress thread. `pmix_server_device_dists` computed the distances
locally, passed `(NULL, NULL)` for the pair, and freed the array on the
next line — so every locally answered `PMIx_Compute_distances` packed
freed heap into the reply. A local producer needs a release function
exactly as much as a remote one does; `dist_relfn` in
`pmix_server_fabric.c` is the shape.

**The hwloc unpackers hand back a `source` string even when they hand
back nothing else.** `pmix_hwloc_unpack_topology` and
`pmix_hwloc_unpack_cpuset` both `strdup("hwloc")` unconditionally, and
`pmix_hwloc_destruct_{topology,cpuset}` free it only when they are also
freeing the object it describes. So the two "the client didn't supply
one, use ours" paths in `pmix_server_device_dists` — the common case for
both arguments — each leaked that string on every request: the topology
one because borrowing the global topology skips the destruct, the cpuset
one because `pmix_hwloc_parse_cpuset_string` overwrites the member with a
fresh `strdup`. Free it yourself on any path where you replace or borrow
past one of these.

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

A related rule applies to `pmix_iof_flags_t`, which is copied by
assignment in three places while two of its members are `strdup`ed:
**a shallow copy of the flags owns nothing, so NULL `file` and
`directory` right after it.** The strings belong to whatever parsed
them - for `pmix_server_process_iof` that is the setup caddy, which is
released long before the `pmix_iof_req_t` it copied into. Both client-side
copies (`stash_spawn_iof_flags` and the spawn reply handler in
`pmix_client_spawn.c`) NULL them explicitly; the server-side one did not,
and left two dangling pointers in a long-lived request.

Note which flags the output path actually consults, because it is not
these: `pmix_iof_write_output` takes its formatting flags from
`nptr->iof_flags`, `stand_in_flags()` or `pmix_globals.iof_flags` - the
namespace and the process, never the request. `pmix_iof_req_t::flags` is
written and never read, which is why the dangling pointers were harmless
rather than a use-after-free. A reader added later would need its own
copy of the strings, not the caddy's.

### `pmix_cb_t` borrows its directives, and never owns its `proc`

`pmix_cb_t` carries two info arrays with two different ownership rules,
and one member the destructor deliberately ignores:

- `info`/`ninfo` are freed by `cbdes` only when `infocopy` is set.
- `directives`/`ndirs` are freed only when `dircopy` is set. **Every
  other site in the tree that fills this member borrows it** — it is the
  caller's array, handed down through `PMIx_Process_monitor_nb` or the
  IOF calls — so the flag exists for the one owner: `pmix_server_monitor`,
  which unpacks the directives off the wire. Without it the array leaked
  on every monitor command.
- `proc` is **never** freed by the destructor, because it is as often a
  pointer to a stack `pmix_proc_t` (`pmix_server_refresh_cache` does
  exactly that) as it is an allocation. A handler that
  `PMIX_PROC_CREATE`s one owes it a free on every path that does not
  reach the completion — `pmix_monitor_processing`'s `relcbfunc` is what
  frees it on the paths that do.

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

**Every collective owes `PMIX_TIMEOUT` a local timer, because the host
cannot supply one.** The attribute is documented as bounding the whole
operation, and the directives do reach the host in `trk->info` - but only
once the collective is complete enough to be handed up. Until the last
local participant contributes, the host has never heard of the request,
so the case the attribute exists for (a participant that never arrives)
is exactly the case only we can bound. Fence and connect always armed
one; disconnect did not, and hung its callers forever on a missing
participant. `test/simple/simptimeout.c` covers all three, driven under
`simptest`.

Timers are armed with `PMIX_THREADSHIFT_DELAY(trk, ..._timeout, secs)`
guarded by `!trk->event_active`, and the fence family does **not** add a
retain when arming (the collectives-list reference is the sole
reference; completion cancels the timer before releasing). Follow the
fence pattern precisely — arming a timer with an unbalanced `PMIX_RETAIN`
or without the `event_active` guard leaks the tracker. A timeout handler
that has to tear the tracker down itself (no completion function was ever
attached) is bound by the same rule as everything else here: **unlink
from `collectives` first, then release.**

**Cancel the timer before the host up-call, at every site that makes
one.** `pmix_server_fence` does, and says why at the call site: once the
tracker is in the host's hands our timeout and the host's completion race,
and the host can return a tracker we already released. The other two
places that hand a tracker to `pmix_host_server.fence_nb` —
`pmix_server_execute_collective` in `pmix_server_registration.c` and the
lost-connection sweep in `src/mca/ptl/base/ptl_base_sendrecv.c` — do not
cancel it. Both are reached only after a tracker has been sitting
incomplete (a late registration, a departed participant), which is
precisely when a `PMIX_TIMEOUT` timer is most likely to be armed.

**Read `PMIX_TIMEOUT` into a variable of the width you ask for.**
`PMIx_Value_get_number(value, dest, type)` writes exactly `sizeof(type)`
bytes through `dest`. Handing it `&tv.tv_sec` while asking for
`PMIX_UINT32` therefore fills four bytes of a `time_t` — the low half on
a little-endian host, which happens to work, and the high half on a
big-endian one, which multiplies every timeout by 2^32. Convert through a
`uint32_t` of its own and assign. The fence, connect and get handlers all
do this now; `pmix_server_group` always did.

**The modex blob handed *up* to the host belongs to the host, and nothing
says so in the header.** `pmix_server_fence` unloads the assembled bucket
into a bare `char *data` and passes it to `pmix_host_server.fence_nb`; so
does `pmix_server_execute_collective`. Neither frees it, and that is not
an oversight to "fix": the request direction carries no
`(release_fn, release_cbdata)` pair the way the reply direction does, and
the in-tree reference host parks the pointer on a caddy and reads it later
from another thread (`fencenb_fn` in `test/simple/simptest.c`), so freeing
it on return would be a use-after-free. Ownership transfers on the call.
`simptest` then leaks it — that is a defect in the test host, not in the
library, and it is the reason a naive valgrind run of the simple suite
shows a per-fence leak here.

**`pmix_cb_t::copy` is advisory and no fetch honors it.** Several sites
here set `cb.copy = false` with a comment about letting the GDS return a
pointer into local storage; `gds/hash` marks the parameter unused and
`pmix_hash_fetch` allocates a fresh `pmix_kval_t` for every value it
returns. So `PMIX_LIST_DESTRUCT(&cb.kvs)` is always correct and never
drops a reference the datastore still holds. Do not "optimize" a caller
on the theory that `copy = false` handed back a borrowed pointer.

**`pmix_pending_resolve` always returns `PMIX_SUCCESS`.** Its callers
check the status anyway, which is harmless, but do not build error
handling on it — in particular `pmix_server_commit` returns that status
to the client as the status of its *commit*, which would report one
proc's `PMIx_Commit` as failed because some other proc's parked get could
not be drained.

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

**A tracker handed to the host carries a reference of its own.** Both
sites that call `pmix_host_server.direct_modex` pass the `lcd` as
`cbdata`, and the host holds that pointer until it calls `dmdx_cbfunc` -
which may be long after we have decided the tracker is finished with. A
namespace deregistration for the target retires *every* tracker naming
it (see below), so the bare pointer the host was holding was a dangling
one. Each up-call therefore takes a `PMIX_RETAIN` on the lcd and gives
it back on exactly one of three arms: the host declines the call,
`dmdx_cbfunc` finds the progress thread stopped, or `_process_dmdx_reply`
finishes. The reply handler then has to ask `tracker_is_pending` whether
the tracker is still on `local_reqs` before touching it - our reference
keeps the object alive, it does not put it back on the list, and
resolving an unlinked tracker would `pmix_list_remove_item` it a second
time.

The same reasoning governs the *directives*: the host keeps every input
valid until it calls back, and the `pmix_server_caddy_t` does not live
that long, so what goes up is `lcd->info` - the tracker's own copy, which
`pmix_server_get` augments in place with `PMIX_REQUIRED_KEY` - and never
`cd->info`. `pmix_pending_nspace_requests` always did it this way.

**`_satisfy_request` reports whether it *answered*, not whether it
succeeded.** Once it has data to hand back it invokes `cbfunc`, and that
hands the server caddy to the reply path, which queues the client's
answer and releases it. So every arm past that point must return
`PMIX_SUCCESS` even when the status it passed was an error - both callers
read a non-SUCCESS return as "nobody has answered this requester yet",
and `pmix_server_get` responds by parking the caddy on a fresh dmodex
tracker while `check_req` responds by calling the requester's `cbfunc` a
second time. A failed assemble or a failed pack after the fetch had
already succeeded therefore produced a double reply on a freed caddy.

**A tracker on `local_reqs` whose target has departed must fail its
waiters, not just disappear.** `pmix_server_purge_events` runs when a peer
finalizes or a namespace is deregistered, and any direct-modex tracker
naming that peer is never going to be answered. Use
`pmix_server_fail_local_reqs`, which calls each parked requester's
callback with a status before discarding the tracker: those requesters are
*other* local procs, and nobody else is going to answer them. This is the
same reference-counting trap as the one below - the difference is only
whether the waiters deserve to be told.

**`remote_pnd` is the same trap pointing the other way.** It holds the
*host's* `PMIx_server_dmodex_request` calls (`pmix_server_dmodex.c`),
parked when the local client they name has not committed its data yet -
and `pmix_server_commit` unlinking one is the only thing that ever took a
request back off the list. So a target that departs without ever
committing (it aborted, it never called `PMIx_Put`, its namespace was
deregistered) left the host holding a request it would never hear about
again, and the remote server that asked - along with the client blocked
in `PMIx_Get` behind it - waited forever. `pmix_server_purge_events` now
calls `pmix_server_fail_remote_pnd`, the mirror of
`pmix_server_fail_local_reqs`: answer the host with a status, then
discard. Note the response function's contract while you are there - the
public header says **"The PMIx server will free the data blob upon return
from the response fn"**, which is why `_dmodex_req` frees `data`
immediately after calling it.

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

## The event registration store

`pmix_server_globals.events` is a list of `pmix_regevents_info_t`, one
per event code (plus one keyed `PMIX_MAX_ERR_CONSTANT` for default
handlers), each owning a `peers` list of `pmix_peer_events_info_t` — one
entry per registration message that named the code. Three invariants are
easy to get wrong.

**A peer legitimately holds more than one entry for the same code, so a
deregistration has to clear them all.** The client library packs a
handler's *whole* code list whenever any code in that list is new to this
server, so a client that registers one handler for `{A}` and a second for
`{A, B}` sends both lists and we record `A` twice — deliberately, since
each entry carries that handler's own `PMIX_EVENT_AFFECTED_PROC` filter.
It then sends exactly one deregistration for `A`, when its last handler
for `A` goes away. `pmix_server_deregister_events` stopped at the first
match and left the rest behind for the life of the connection: the server
kept forwarding the code to a peer with no handler for it, and `peers`
never emptied, so `pmix_server_prune_reginfo` never fired and the host was
never told to stop either.

**`reginfo->active` is a promise to every future registrant, so a host
that refuses must have it given back — on both arms.** `mark_activations`
sets it for the codes a request is about to ask the host about, sorting
them to the front of the array so the host is handed just that subset;
`undo_activations` is its inverse, and the only thing that reads the flag
is the next registration deciding whether to ask again. The synchronous
refusal always called `undo_activations`; the asynchronous one did not, so
a host that accepted a registration for processing and then failed it left
the code marked active forever — and every later registrant for it was
told it succeeded and then never saw the event. `pmix_setup_caddy_t`
carries `nactive` for exactly this. The undo runs on the progress thread
(`_failed_registration`), *not* in the host callback, because the store is
progress-thread state — `regevopcbfunc` may be called on the host's own
thread and does nothing but thread-shift.

The peer entries a failed registration added are deliberately *not*
removed. A peer may hold several handlers for one code and nothing in the
request identifies which entry belongs to it; those entries go when the
peer deregisters or departs. Note also that
`pmix_peer_events_info_t::enviro_events` is written and never read
anywhere in the tree — the same shape as `pmix_iof_req_t::flags`.

**The cached-event replay answers with the registration's status, not its
own.** `_check_cached_events` runs after a successful registration to hand
a late registrant anything already in the notification hotel that matches
its codes, its affected-proc filter and the event's range. It used to
report whatever the replay ran into, so failing to pack or queue one stale
event told the client its handler was not registered — while the server
had in fact registered it, leaving an entry the client would never
deregister. Note that all three arms have to attach that filter to the
caddy (`scd->procs`) and the host-completes-asynchronously arm did not, so
it replayed events the registrant had said it did not want; attach it
*before* the up-call, since the host may complete on another thread the
moment it has the caddy.

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
  is left parked on `local_reqs`. It also drives the handler from a
  hand-packed buffer whose info count is a lie - one that wraps the
  allocation and one that merely truncates - and those cases kill an
  unfixed library rather than failing it. Read its header for what it
  pins down and what it deliberately cannot reproduce.
- [`test/unit/server_dmodex.c`](../../test/unit/server_dmodex.c) covers
  `PMIx_Store_internal` and the `remote_pnd` deferral above, including
  the departure drain. It orders itself off `PMIx_Store_internal` rather
  than a sleep: that call blocks on the progress thread, so anything
  queued ahead of it has run by the time it returns.
- [`test/unit/server_control.c`](../../test/unit/server_control.c) drives
  `pmix_server_log` and `pmix_server_job_ctrl` from hand-packed wire
  buffers — the malformed-count screens, the appended `PMIX_LOG_SOURCE` /
  `PMIX_LOG_TIMESTAMP` directives, and the RFC precedence rule that a
  repeated cleanup directory upgrades the entry already on the epilog.
  The bad-directive-count case aborts rather than fails against an
  unfixed library, the same bargain `test/unit/iof_output.c` makes. It
  also drives `pmix_server_query` with mistyped query qualifiers — the
  server unpacks those off the wire, so it cannot rely on the screening
  `PMIx_Query_info_nb` does in the *requestor's* process; a mistyped
  `PMIX_NSPACE` segfaulted the server until `pmix_parse_localquery`
  grew its own check.
- [`test/unit/server_fence.c`](../../test/unit/server_fence.c) drives
  `pmix_server_fence` from hand-packed wire buffers: the two malformed
  counts above, and a well-formed request whose seeded info slots must
  reach the host in the order the completion arms read them back. Its
  helper thread-shifts, because the handler touches
  `pmix_server_globals.collectives` and must not be called from `main()`.
  The info-count case aborts rather than fails against an unfixed
  library. Its host stub *declines* the fence on purpose — accepting
  would leave the completion to the test, and queueing a reply to a peer
  with no socket is not something a single-process program can do.
  `test/unit/tracker_match.c` and `test/unit/trk_complete.c` cover the
  tracker-identity and completion-predicate halves of the same file, and
  [`test/unit/collect_job_info.c`](../../test/unit/collect_job_info.c)
  covers `PMIx_server_collect_job_info`, including the case where one
  named namespace cannot be answered for and must not discard the job
  info collected for the others.
- [`test/unit/server_fabric.c`](../../test/unit/server_fabric.c) drives
  `pmix_server_device_dists` from a hand-packed buffer that carries no
  topology — the "use your own" path — and asserts that a locally
  computed distance array is handed over with a release function rather
  than freed on the spot. Note it reports SKIP rather than PASS on a host
  whose hwloc finds no OS devices (a bare macOS one), because there is
  then no answer to hold the contract against; the leak half of it only
  shows under valgrind.
- [`test/unit/event_chain.c`](../../test/unit/event_chain.c) drives
  `pmix_server_register_events` / `pmix_server_deregister_events` from
  hand-packed wire buffers alongside its client-side event-chain cases —
  the default-handler entry that registration has to create, and the
  duplicate `(peer, code)` entries a deregistration has to clear in full.
  Its `client_evreq()` helper thread-shifts, because both of those touch
  `pmix_server_globals` and must not be called from `main()`.
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
- **An unchecked `PMIX_*_CREATE` followed straight by an unpack is not a
  bug.** `pmix_bfrops_base_unpack` screens `NULL == dst` and returns
  `PMIX_ERR_BAD_PARAM`, so the near-universal shape here — `if (0 < n)
  { PMIX_INFO_CREATE(array, n); cnt = n; unpack into array; }` — fails
  cleanly when the allocation fails, whether from real memory pressure or
  from an absurd count off the wire. Do not sprinkle NULL checks through
  these handlers; it was audited and refuted.
- **What *does* need screening is a wire count used before, or without,
  the unpack.** Every count arrives as a `size_t` and is then consumed
  through the `int32_t` that `PMIX_BFROPS_UNPACK` takes, so a peer can
  send one that truncates to zero or to a negative number. That is
  harmless wherever the only use is sizing an array the unpack
  immediately guards. Two handlers here are not that. `pmix_server_log`
  indexes the directive array to append `PMIX_LOG_SOURCE` *before*
  anything is unpacked into it (a negative index off a NULL array) and
  hands `(info, ninfo)` to `plog` even when the unpack was skipped;
  `pmix_server_register_events` walks its code array `ncodes` times while
  only `cnt` entries were unpacked into it, and that array comes from a
  bare `malloc`, so the tail is uninitialized rather than constructed
  (an info array is safe here precisely because `PMIX_INFO_CREATE`
  constructs every element). Both now reject a count that does not
  survive the round trip. Apply the same test to any new handler that
  reads a count before it reads the array, or that walks the `size_t`
  rather than the `int32_t` afterwards.

  Know *why* an oversized count is dangerous and not merely wasteful:
  `PMIx_Info_create` computes `n * sizeof(pmix_info_t)` with no overflow
  guard and then **constructs every one of the `n` elements**. A count
  large enough to wrap that product therefore yields a short allocation
  whose constructor loop runs straight off the end - so the crash happens
  inside the allocation, before any of the "the unpack screens a NULL
  destination" reasoning gets a chance to apply. `PMIX_PROC_CREATE` and
  the other `_CREATE` macros are built the same way. That is why the
  round-trip screen belongs on any count a peer controls, not only on the
  ones with a visible `+ 2`. `pmix_server_get` needed it for exactly this
  reason, and its wire count is covered by `test/unit/server_get.c`.

  **The collective handlers are the third case, and the worst of them.**
  Fence and connect both size their info array as `ninf + 2` and then
  seed two slots at `info[ninf]` and `info[ninf+1]` — *before* anything
  is unpacked into it. A wire count near `SIZE_MAX` wraps that `+ 2` down
  to a one- or zero-element array (`PMIx_Info_create` answers NULL for
  zero, which is caught, but not for one), and the seeds are then written
  at `info[SIZE_MAX]`. That is an out-of-bounds write a local client
  drives directly, and there is no unpack in front of it to screen
  anything. The proc count in the same message is the mirror image: it is
  multiplied by `sizeof(pmix_proc_t)` to size the proc array, and it is
  the `size_t` — not the `int32_t` the unpack consumed — that the `qsort`
  and the `PMIX_PROC_FREE` afterwards walk. Fence, connect and disconnect
  now all screen both counts with the same round-trip test. Covered by
  `test/unit/server_fence.c` and `test/unit/server_connect.c`, whose
  info-count cases kill an unfixed library rather than failing it. **Any
  new collective handler that seeds slots past the unpacked ones owes the
  same screen** — the group handler builds its info array differently
  (`ninfo = ninf + 1`, `pmix_server_group.c`) and should be read with
  this in mind.

  While you are there, note the layout those two seeds establish, because
  the completion arms depend on it: for the fence and disconnect families
  `PMIX_LOCAL_COLLECTIVE_STATUS` is the **last** element, and
  `pmix_server_fence` reads it positionally as `trk->info[trk->ninfo-1]`
  on both its local-only and its `PMIX_OPERATION_SUCCEEDED` arms. That is
  correct only because connect is the family that appends past the slot —
  which is exactly why `pmix_server_set_collective_status` locates it by
  key instead. Do not copy the positional read into a family that
  appends.
- **`PMIX_LIST_DESTRUCT` is not idempotent.** It drains the list and then
  calls `PMIX_DESTRUCT`, which under `--enable-debug` zeroes the object's
  magic id and asserts on it — so a second destruct of the same list
  aborts the server, and nothing restores the id in between.
  `pmix_server_job_ctrl` destructed its four cleanup lists mid-function
  *and* again at its exit label, under a comment asserting the practice
  was harmless. Destruct each local list exactly once, on the way out.
- **Screen the shape of anything the host hands you before indexing it.**
  A `pmix_info_t` carrying a `PMIX_DATA_ARRAY` is a host-supplied
  structure, and several sites here read `array[0]` and `array[1]`
  positionally on the strength of a comment. Check the array is non-NULL
  and long enough first: `_register_nspace` and `_register_resources` both
  did not, and neither is reachable only from trusted code - a host is
  free to get this wrong.

  **The same applies with more force to anything a *client* handed you.**
  An info array a handler unpacked off the wire carries whatever type tag
  the peer chose, so reading its union without checking that tag is
  reading a pointer the peer picked. `pmix_server_register_events` took
  `PMIX_EVENT_AFFECTED_PROC` and `PMIX_EVENT_AFFECTED_PROCS` straight out
  of it, so a mistyped one had the server copy a `pmix_proc_t` out of a
  scalar or dereference that scalar as a `pmix_data_array_t`; the group
  branch of `pmix_server_event_recvd_from_client` did the same with
  `PMIX_GROUP_ID` and `strcmp`'d the result. Confirm the value's type -
  and, for a data array, its *element* type, or a correctly-tagged array
  of some other type walks past its end. This is the client-side twin of
  the `pmix_parse_localquery` screen.
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
- **The status `pmix_server_refresh_cache` packs is discarded by the
  client, on purpose.** `refcb` in `pmix_client_get.c` unpacks that
  status, then drains kvals until the buffer runs out and overwrites the
  status with `PMIX_SUCCESS`. That is not an oversight to repair from
  this side: `refresh_cache()` is called for its side effect, and its
  return value aborts the whole `PMIx_Get` — so propagating a
  "nothing to refresh" `PMIX_ERR_NOT_FOUND` would fail gets that
  currently succeed. Pack the honest status (it is the right thing on the
  wire), but do not build anything here on the client acting on it.
- **`pmix_server_job_ctrl` creating a namespace for an unknown target is
  deliberate.** A job-control request may name a job this server has not
  been told about yet, and the epilog directives need somewhere to hang;
  `_register_nspace` looks the namespace up by name and reuses the entry
  when registration eventually arrives. It is not a leak and not a
  phantom.
- **Two things in `pmix_server_get.c` look like defects and are not.**
  `get_job_data` returns `PMIX_SUCCESS` with an empty buffer when the
  fetch finds nothing, and its callers pass that empty payload to the
  client as a success. That is safe and should be left alone:
  `_getnb_cbfunc` in `pmix_client_get.c` initializes its reported status
  to `PMIX_ERR_NOT_FOUND` and only overwrites it if a value actually
  turns up, so an empty success degrades to not-found at the requester -
  the same bargain as the `refresh_cache` status above. And the
  `direct_modex` up-call sites treat `PMIX_OPERATION_SUCCEEDED` as a
  refusal rather than handling it as the third arm of the usual
  tri-state. There is no coherent "done now, no callback" for this
  up-call: its entire product is a data blob delivered through the
  callback, and the header describes no such arm.
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
