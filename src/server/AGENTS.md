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
| `pmix_server.c` | `PMIx_server_init` / `PMIx_server_finalize` and the machinery they stand up: the `pmix_server_globals` definition, `pmix_server_initialize()` and its counterpart `server_teardown()`, the singleton and parent-died plumbing, the server's own event handler, and `PMIx_server_setup_fork` (which passes the modes negotiated at init down to a child). Also `pmix_server_lock_opcbfunc`, the completion callback every blocking public API substitutes when its caller passes no callback. The MCA parameters that fill `pmix_server_globals` are **not** registered here — they live with every other one in [`src/runtime/pmix_params.c`](../runtime/pmix_params.c). |
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
| `pmix_server_fence.c` | The fence collective (barrier + modex data exchange) **and the shared collective-tracker engine** — `pmix_server_get_tracker`, `pmix_server_new_tracker`, `pmix_server_collect_data`, `pmix_server_commit`, plus the two predicates every family consults: `pmix_server_trk_complete` and `pmix_server_set_collective_status`, and the family's lost-connection accounting `pmix_server_trk_peer_lost` (the group family's counterpart lives in `pmix_server_group.c`). Also `PMIx_server_collect_job_info`, the public API a host uses to pull packed job-level info for a set of procs. |
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
`pmix_cmd_t` and dispatches on it.

The receive callback it is registered under is a **wildcard** one, so it
sees every message that no more specific recv claimed — including
messages that are not commands at all. `pmix_server_message_handler`
therefore drops `PMIX_PTL_TAG_HEARTBEAT` before dispatching; see "An
unmatched message" in [`../mca/ptl/AGENTS.md`](../mca/ptl/AGENTS.md) for
why a beat can arrive with no recv of its own, and why letting it reach
`server_switchyard` sent a bogus error reply to a client that was not
listening for one.

**The return value of every command handler encodes who owns the reply
and the caddy:**

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

Three things about the switchyard read as defects and are not. Check them
against this list before "fixing" one:

- **A failed `PMIX_SERVER_QUEUE_REPLY` cannot strand a client.** Several
  arms release the reply and carry on returning `PMIX_SUCCESS`, which
  looks like a client left blocked forever. The macro has exactly one
  failure mode — `peer->finalized`, answered with `PMIX_ERR_UNREACH` —
  so whenever it fails there is no longer a peer waiting for the answer,
  and the synthesized reply the message handler would send instead could
  not be queued either.
- **`PMIX_DEREGEVENTS_CMD` answers nothing, deliberately.** It is the one
  arm that returns `PMIX_SUCCESS` without queuing a reply and without an
  async owner to queue one later. The client sends that command through
  `PMIX_PTL_SEND_RECV` with a **NULL** callback
  (`pmix_internal_dereg_event_hdlr` in
  `src/event/pmix_event_registration.c`), so nothing is waiting on a tag.
- **`PMIX_GDS_CADDY` and `PMIX_SERVER_QUEUE_REPLY` both dereference an
  unchecked `PMIX_NEW`.** Making the macros NULL-safe does not help: the
  handler on the next line dereferences the caddy it was given, so the
  crash moves rather than goes away, and covering it properly would mean
  a check at all thirty-odd dispatch arms for a condition under which the
  library cannot service the request anyway. Left as is, on purpose.

**The outer host callbacks own the caddy on *every* arm that does not
thread-shift.** `op_cbfunc`, `op_cbfunc2` and `resop_cbfunc` each have
two: the `progress_thread_stopped` early-out and the failure to allocate
the `pmix_shift_caddy_t`. All three released the caddy on the first and
dropped it on the second, stranding the `PMIX_RETAIN` it holds on the
requesting peer — and with it the peer and everything hanging off it —
for the life of the server.

**And `resop_cbfunc` is not shaped like the other two.** `op_cbfunc`'s
`cbdata` really is the switchyard's `pmix_server_caddy_t`: the handlers
that name it (`pmix_server_abort`, `_publish`, `_unpublish`, `_log`,
`_iofstdin`) park it on `cd->cbdata` of a caddy of their own and drive
`cd->opcbfunc(status, cd->cbdata)` at the end. `pmix_server_resblk`
instead hands its **own `pmix_setup_caddy_t`** to the host as the
up-call's `cbdata`, with the server caddy nested inside it. So that
callback owns three things — the server caddy, the caddy's `nspace`
string, and the setup caddy — and neither destructor reaches the other
two. It was declared as taking the wrong type, and both of its early
returns released only the outermost object. When you add a callback
here, read the *producing* handler to see which caddy it actually passes.

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

**That includes the `progress_thread_stopped` arm, which is the one it is
easiest to read as exempt.** Every callback in
`pmix_server_info_replies.c` opens with that early-out, in both halves of
the pair - the outer callback that may run on the host's thread and the
inner `_*cbfunc` that runs after the shift. All eighteen of those arms
tore down our own caddies and returned without calling `relfn`. The arm
is not unreachable and it is not a special case: the flag goes up while
`PMIx_server_finalize` is running, which is exactly when an in-flight
host completion lands, and the host process outlives our finalize - so
what is dropped there is stranded for good. Exactly one of the two arms
can fire for a given request (the outer returns before shifting, so the
inner never runs), which is why calling `relfn` in both is right rather
than a double release. The rule is not confined to that file: the two
relfn-carrying pairs in `pmix_server_op_replies.c` - modex and get - had
exactly the same four arms, and both now honor it.

**A payload the release function owns must be disowned on every path that
destructs the buffer holding it.** `PMIX_LOAD_BUFFER` does not copy and
does not allocate: it points the buffer straight at the payload it is
handed and NULLs the source pointer. So a `PMIX_DESTRUCT` of that buffer
frees memory that belongs to whoever gave us the `(relfn, relcbdata)`
pair, which is then freed a second time when the release function runs.
`_getcbfunc` NULLed `base_ptr`/`bytes_used` before destructing on its
success path and *not* on the arm where `PMIX_BFROPS_COPY_PAYLOAD`
failed, so a failed copy was a double free of the host's blob. Disown the
buffer immediately after the copy, before branching on its status - that
way one line covers both arms. The same reasoning is why `_mdxcbfunc`
uses `PMIX_LOAD_BUFFER_NON_DESTRUCT` and carries a comment saying not to
destruct `xfer`.

**A callback that thread-shifts must reach the shift on every path it can
still answer from.** Every one of these eleven callbacks is the *only*
thing that will ever reply to its client: the handler up-called the host
and returned `PMIX_SUCCESS`, so the switchyard let go of the request and
will synthesize nothing. `pmix_server_cred_cbfunc` copied the host's
credential unconditionally — and the copy screens a NULL source and
reports `PMIX_ERR_BAD_PARAM`, while a host that cannot issue one answers
with an error status and *no* credential, which is exactly what
`pmix_credential_cbfunc_t`'s contract says and exactly what
`_cred_cbfunc` is written for (it packs the credential only under a
success status). So the ordinary refusal took an error return that
released the caddies and queued nothing, and the client sat in
`PMIx_Get_credential` for good. Record the failure in `scd->status` and
fall through to the shift; `pmix_server_validate_cbfunc`'s two info-copy
arms had the same shape. Covered by `test/unit/server_control.c`.

**And when it genuinely cannot shift, it still owes the caddies.** The
`PMIX_NEW(pmix_shift_caddy_t)` failure arm is the twin of the
`progress_thread_stopped` arm right above it, and nine of the eleven
released nothing there — stranding a `pmix_server_caddy_t`, the
`PMIX_RETAIN` it holds on the requesting peer, and (where there is one) a
`pmix_query_caddy_t` with its queries and info arrays, for the life of
the server. `pmix_server_fabric_cbfunc` and `pmix_server_dist_cbfunc`
always had it right; diff a new one against those. Note that the explicit
`PMIX_QUERY_FREE`/`PMIX_INFO_FREE` some of these arms do before
`PMIX_RELEASE(qcd)` is belt-and-braces rather than required — `qdes`
frees both, and the macros NULL what they free, so doing it twice is not
a double free.

**A callback signature with no release function has to *copy* what it
parks.** `pmix_credential_cbfunc_t` and `pmix_validation_cbfunc_t` -
`pmix_server_cred_cbfunc` and `pmix_server_validate_cbfunc` - carry no
`(relfn, relcbdata)` pair, so nothing the host passes survives the return
of the call. Both of them only thread-shift, and pack the reply later on
the progress thread. Validate always copied its info array and said why;
cred copied the credential (`PMIX_BFROPS_COPY`) and then parked the info
array *by pointer* beside it, so a host that answered from a stack frame
- the natural thing for a one-element reply - had the server pack that
frame after it was gone. Copy into the caddy and set `infocopy`, rather
than freeing explicitly at the end: the flag also covers the early
returns above the completion, which is where validate leaked its copy.
The regression case is in `test/unit/server_control.c`, whose host stub
destructs and overwrites its array the instant the callback returns.

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
today. Read the rest of that function with the same caution.

**That same unreachability had hidden a progress-thread block, and it is
now gone.** `pmix_server_fabric_register`'s `PMIX_SUCCESS` arm — the
framework's way of saying "I will call you back" — used to answer it by
`PMIX_WAIT_THREAD`-ing on the switchyard's own thread. Every component in
this tree does its deferred work by posting an event to the progress
thread, which is precisely the thread that would have been sitting in
that wait, so the first `register_fabric` module anyone wrote would have
hung the whole library rather than merely crashing it. Two more things
were wrong with the arm besides: it handed the module the address of a
`pmix_fabric_t` on `pmix_server_fabric_register`'s own stack, which dies
when the function returns, and it then discarded the status it had
waited for and answered the client `PMIX_SUCCESS` regardless. So the arm
now refuses the deferral with `PMIX_ERR_NOT_SUPPORTED` and the up-call
passes `(NULL, NULL)` for the callback pair. **Making
`register_fabric` answer asynchronously means designing that path
first** — a heap-allocated fabric object owned by the caddy, and a
completion that thread-shifts into `_fabric_response` — not restoring
the wait.

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
| `pmix_query_caddy_t`, `pmix_cb_t` | globals / ops.h | Query/fetch carriers. (There is no inventory roll-up caddy: `pmix_server_inventory.c` carries its request on a plain `pmix_shift_caddy_t`.) |

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

### What `scdes` does *not* free

`pmix_shift_caddy_t` is the generic bounce caddy, and its destructor
frees only `key`, `peer`, `pname.nspace`, `kv`, and the two info arrays
under `infocopy`/`dircopy`. Everything else parked on it is the
handler's to free by hand, on **every** path — `pdata` most of all, since
`pmix_server_lookup_cbfunc` builds a whole `PMIX_PDATA_CREATE`d array
there (it must: `pmix_lookup_cbfunc_t` carries no release function, so
nothing the host passes survives the return). `_lkupcbfunc` freed it only
on its success arm, so every pack failure and the finalize-race early-out
leaked the array and every value in it. Free it at the common label, not
beside the pack that consumed it.

A related trap sits one level down: **a stack `pmix_cb_t` must be
destructed on every path, not only where the fetch succeeded.** `cbcon`
constructs a lock unconditionally and `cbdes` is the only thing that
takes it down, and `pthread_mutex_init`/`pthread_cond_init` are allowed
to allocate. `_spcb` had the `PMIX_DESTRUCT(&cb)` inside its
`PMIX_SUCCESS == rc` arm, and a fetch that finds nothing is the ordinary
case for a job this server has not been told about yet.

### A blocking entry point that never reads its lock's status

The blocking pattern below is only half done if the handler wakes the
lock directly. `PMIx_server_define_process_set` and
`_delete_process_set` both set `cd.opcbfunc = pmix_server_lock_opcbfunc`
and `cd.cbdata = &cd.lock` — and then their handlers called
`PMIX_WAKEUP_THREAD(&cd->lock)` themselves and the entry points returned
a literal `PMIX_SUCCESS`. The waker was dead code and every failure the
handler could have reported was invisible to the host. Call
`cd->opcbfunc(rc, cd->cbdata)` on every arm out of the handler, and read
`cd.lock.status` back **before** `PMIX_DESTRUCT` takes the lock down.

Related: these two are public `PMIx_server_*` entry points, so the host
can hand them anything, and neither screened its arguments. A NULL pset
name reaches `strdup`, `strcmp` and `PMIX_INFO_LOAD`; a NULL member
array reaches `memcpy`; and a **zero** member count is the one that does
not look like a bug — `PMIx_Data_array_create` answers NULL for a
zero-element array exactly as it does for a failed allocation, and the
next line read `darray->array`. All three segfaulted the progress
thread. `test/unit/progress_threads.c` covers them, and its zero-count
case kills an unfixed library rather than failing it.

Any struct handed to `PMIX_THREADSHIFT` **must** carry a `pmix_event_t ev`
as its thread-shift member (the caddy contract from the top-level guide).
Do not stack-allocate a caddy that outlives its creating function — the
one deliberate exception is the blocking process-set define/delete path,
which uses a stack caddy that is safe *only* because `PMIX_WAIT_THREAD`
blocks the creator until the handler wakes it.

## Coming up and going down

`PMIx_server_init` and `PMIx_server_finalize` are a matched pair, and the
thing that keeps them matched is `server_teardown()`: `PMIx_server_finalize`
is little more than the reference count, the flags, and a call to it.

**Every failure past `pmix_rte_init()` unwinds through that same
function.** The alternative — returning the status and leaving the process
as it stands — is not a smaller version of the same thing, it is a wedge.
`PMIx_server_init` latches `pmix_globals.init_called` with
`pmix_atomic_test_and_set` before it does anything, and the already-latched
arm answers `PMIX_ERR_INIT` whenever `pmix_globals.initialized` is clear.
So a bare `return rc` left the caller unable to retry *and* unable to
finalize — `PMIx_server_finalize` declines for the same reason, the
initialized flag was never set — for the life of the process. A host that
would quite reasonably correct a directive and try again (a different
`PMIX_SERVER_TMPDIR` after the listener could not bind, a corrected
`PMIX_SINGLETON`) got one answer forever. Failures land on one of two
labels: `errout` for everything past the runtime, `errout_early` for the
directive screens above it, and both give back the reference count and the
latch, in that order. `test/unit/singleton_register.c` drives a rejection
on each side of the runtime and then inits again.

The one deliberate exception is `pmix_rte_init()` itself, which does not
give back what it built before failing; that arm returns latched, and says
so. The same reasoning governs `PMIx_Init` in
[`src/client`](../client/AGENTS.md).

**`pmix_server_initialize()` therefore runs immediately after
`pmix_rte_init()`, before anything else that can fail.** Until it runs,
every list and pointer array in `pmix_server_globals` is only *statically*
initialized — and `PMIX_LIST_STATIC_INIT` leaves the sentinel's `next`
pointer NULL rather than pointing at itself. `PMIX_LIST_DESTRUCT` survives
that (it is gated on the length), but `PMIX_LIST_FOREACH` does not: it
starts at NULL, compares unequal to the sentinel's address, and
dereferences. The teardown walks two such lists (`pmix_iof_flush_residuals`
and the `local_reqs` drain), so the constructor has to be upstream of every
`goto errout`. Do not move it back down beside the code it used to sit
with.

**Finalize must not be called from the progress thread.** The teardown
waits on that thread and then stops it, so from the thread itself this is
not a deadlock, it is the thread ending itself mid-callback — a host
tearing down from inside a PMIx event handler or a server-module
completion is exactly there. `pmix_progress_thread_check_blocking` screens
it, and answers `PMIX_ERR_WOULD_BLOCK` after putting the reference back. A
host driving our event base with `PMIX_EXTERNAL_PROGRESS` is *not* caught
by this: the owning thread is recorded only for the duration of a
`PMIx_Progress()` pass, so an ordinary call from the host's main loop gets
through, and there is no engine to stop.

**Two file-scope statics in `pmix_server.c` are worth knowing about.**

- `pmix_server_globals.module_set` is what decides whether a module handed
  to `PMIx_server_init` is taken, so `server_teardown()` clears it. Left
  set across a cycle, the second init silently kept the first cycle's host
  module and discarded the one it was given.
- `myparent` is filled from the `pmix_cb_t` that
  `pmix_tool_retry_attach` hands back, and that is the **only** place the
  parent's identity ever appears — the caddy carrying it is released on
  the next line. Nothing assigned it before, so the `PMIX_PARENT_ID` the
  `PMIX_LAUNCHER_RNDZ_URI` block stores under our own procid was a
  namespace of `""` at `PMIX_RANK_UNDEF`: a `PMIx_Get` of that key
  succeeded and answered with nobody. `PMIx_tool_init` carries the same
  block and had the same defect; `test/unit/tool_rndz.c` now holds the
  stored value against the server it actually rendezvoused with.

**Init's tail runs on the caller's thread, and two things keep that
safe.** `pmix_rte_init()` ends by starting the progress thread, so
everything `PMIx_server_init` does afterwards is on the caller's thread.
`gds/hash` holds no lock of any kind — it is correct only because
everything that touches it is meant to be on one thread — so anything
init writes there is a race with whatever the progress thread is doing.

- **Nobody outside can get onto that thread during the window.** The
  listener's accept event is armed by `pmix_ptl_base_start_listening()`
  at the *foot* of init, separately from
  `pmix_ptl_base_create_listener()`, which binds the socket and publishes
  the URI where it always did. A peer that connects in between waits in
  the kernel's backlog rather than being refused. See "The listener" in
  [`../mca/ptl/AGENTS.md`](../mca/ptl/AGENTS.md) for why the split is
  shaped that way and why moving the whole call would not do.
- **The stores init still makes go through `store_internal()`**, which
  thread-shifts and blocks. The accept split does not cover a server we
  connected *out* to — the `PMIX_LAUNCHER_RNDZ_URI` and
  `connect_directed` arms both have a live peer whose receive is posted
  the moment `connect_to_peer` returns, and a message from it reaches the
  switchyard and the datastore on the progress thread. That helper runs
  inline under `PMIX_EXTERNAL_PROGRESS` or on the progress thread itself,
  the same bargain `PMIx_server_setup_fork` makes.

So: **do not add a bare `PMIX_GDS_STORE_KV` to init**, and do not move
either listener call. What is left on the caller's thread and not covered
is `pmix_ptl.connect_to_peer()` on the `connect_directed` arm; it is the
only connection in existence at that point, and the store that follows it
is shifted.

**`pmix_server_globals.genvars` is read and never written.**
`setup_fork_body` replays it into every child's environment, and nothing
anywhere in the tree puts anything in it.
`pmix_server_globals.tool_connections_allowed` is the same shape: declared
here, initialized to false, and never read or written. The knob that
actually decides whether we answer a tool is `pmix_ptl_base.tool_support`,
set from `PMIX_SERVER_TOOL_SUPPORT` in `ptl_base_listener.c`. It is a hook for a "pass these
envars to all my clients" directive that was never wired up; see
`docs/todo.rst`. Do not read the read site as evidence that a writer
exists somewhere.

**What the host hands `PMIx_server_setup_fork` is unscreened.** It is a
public entry point and both arguments are dereferenced — the `proc` by the
verbose line, whose arguments are evaluated whether or not the channel is
open, and the `env` by everything the body sets. Same rule as the helper
APIs in `pmix_server_setup.c`.

**And what the *environment* hands init is unscreened too.** Both
`PMIX_SERVER_RANK` and `PMIX_KEEPALIVE_PIPE` were read with a bare
`strtol` whose result was never inspected, and `strtol` answers 0 for a
string it cannot read at all. So an empty or non-numeric `PMIX_SERVER_RANK`
made this server rank 0 — a rank another server in the namespace very
likely holds — and a malformed `PMIX_KEEPALIVE_PIPE` made us watch
descriptor 0 for readability, treat stdin becoming readable as our parent
dying, and `close(0)` at finalize. Both now reject the value with a
`show_help` message rather than guessing.

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

**`PMIX_RELEASE` NULLs the pointer it is given** when that release drops
the last reference — both the debug and the optimized spelling end with
`object = NULL` (`src/class/pmix_object.h:543`). So a handler that
releases a caddy on an error arm and then falls into a shared exit label
that tests `NULL == fcd` is correct, not a use-after-free, and an
`x = NULL` written after a release is redundant rather than load-bearing.
`_setup_app` reads exactly that way and was audited as a UAF twice before
this was written down. The one case where it does *not* hold is an object
someone else still holds a reference to — there the pointer survives the
call, which is the intended meaning.

## Preparing a job (`pmix_server_setup.c`)

Everything here runs before a job's processes do. Three things in it are
easy to get wrong and were.

**A payload that lives in the caller's info array must be borrowed, not
loaded.** `PMIX_LOAD_BUFFER` does not copy: it points the buffer at the
payload it is handed and then NULLs the source pointer and zeroes the
size. `_register_resources` used it on the `PMIX_GROUP_JOB_INFO` byte
object, which sits inside `cd->info` — the host's own array, which the
host owns and keeps valid until our callback fires. So the handler
emptied the host's info element (a second registration with the same
array then silently found no job info) and leaked the blob, because the
buffer is never destructed — and destructing it would have been worse,
freeing memory the host still owns. Use
`PMIX_LOAD_BUFFER_NON_DESTRUCT` and do not destruct, the same bargain
`_mdxcbfunc` makes with `xfer`.

**An unpack loop that ends on `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER`
must not report it.** Running the buffer dry is how the job-info walk
*finishes*, and that status was still in `rc` when the handler drove the
completion — so every registration carrying job info told the host it had
failed, and the blocking form returned the error instead of
`PMIX_OPERATION_SUCCEEDED`. The handler now keeps the status it reports
in a variable of its own that records the *first* failure, since the
transient `rc` of the last sub-operation is not the result of the call
either: a later success used to erase an earlier error just as readily.

**The global data cache appends, so deregistration has to clear every
match.** `_register_resources` adds a `pmix_kval_t` to
`pmix_server_globals.gdata` for any key that is not one of the four group
keys, without checking whether that key is already there — so a host that
re-registers a key to update its value leaves two entries, and
`gds/hash` walks the list in order when it seeds a namespace, which makes
the *later* one the one in effect. `_deregister_resources` stopped at the
first match, so it removed the shadowed entry and left the live one: the
deregistration silently did nothing. It now clears every entry carrying
the key, which is what the man page ("each matching entry") requires.

**A request element carrying a data array is a qualified removal, and the
two kinds of member in it do different things.** `PMIX_NODEID` and
`PMIX_HOSTNAME` are *identifiers*: they choose which entries the request
applies to, and an array carrying none applies to every entry with that
key. Anything else is a *target*: it chooses elements to remove from
inside those entries, matching an element directly or matching one that
contains it — which is how a `PMIX_FABRIC_DEVICE`, whose own array
carries the device's name and id, is named by either of them. An array
with identifiers and no target removes the whole entry. Removing the last
element an entry described removes the entry too, rather than leaving a
husk naming a node and nothing else, which would seed every namespace
registered afterwards with it. Before this, the walk matched on the key
alone, so the man page's own example — one node's fabric device — deleted
every node entry in the cache.

**What no version of this can do is retract data already handed out.**
`gdata` is copied into a namespace's hash tables once, when that
namespace is first registered (`hash_cache_job_info`, guarded by the
per-namespace `gdata_added`), and nothing re-reads it afterwards. So a
deregistration governs the namespaces registered *after* it and leaves
every running client's copy in place. Closing that needs a
delete-a-key entry point the `gds` module struct does not have, and it
cannot be built the obvious way for `shmem3`, whose lock-free read path
is the reason it is fast. See `docs/todo.rst`.

**The helper APIs are pass-throughs, so the public entry point is the
only place their arguments get screened.** `PMIx_generate_regex` /
`_ppn` hand `input` and `regexp` straight to a `preg` component, and
`preg/raw` `strncmp`s the one and writes through the other without
looking; `PMIx_server_generate_locality_string` / `_cpuset_string` hand
theirs to hwloc, which reports a bad cpuset *by writing NULL through the
output pointer* and therefore cannot be the code that screens it. All
four took the library down on a NULL. The `regex2` pair always screened
its arguments here; the others now match it. (`PMIx_server_generate_cpuset`
is the exception that needs nothing: `pmix_hwloc_parse_cpuset_string`
screens both of its arguments and says so.)

**And the group arrays a host registers need the same shape screen the
group collective applies to the ones a client sends.** The
`PMIX_GROUP_INFO` / `PMIX_GROUP_INFO_ARRAY` and `PMIX_GROUP_ENDPT_DATA`
arms read `value.data.darray` — and, inside an endpoint array,
`value.data.proc` and `value.data.scope` — on the strength of the key
alone. `pmix_server_valid_darray` is that screen, shared with
`pmix_server_group.c` through `pmix_server_ops.h`; positional element
types still need checking by hand, since the array being of the right
element type says nothing about what a given element carries.

One thing to know before writing a host against this: the shape
`PMIX_GROUP_ENDPT_DATA` carries here is the one a client builds. The
array is `get_endpts()`'s in
[`src/client/pmix_client_group.c`](../client/pmix_client_group.c) — the
contributor's `PMIX_PROCID`, then the `PMIX_DATA_SCOPE` its remaining
elements are to be stored at — contributed to the construct as
`PMIX_PROC_INFO_ARRAY` and relabelled by the host when it hands each
contribution back through this API. `pmix_common.h` described the
attribute as a `pmix_byte_object_t` until August 2026; that was the
shape of a group-construct exchange the library stopped performing
several releases ago, and nothing in the tree had produced or consumed
it since. `PMIX_GROUP_JOB_INFO` immediately below it *is* a byte object
and is read as one.

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

**`departed` means abnormally terminated, not gone.** The two loss
entry points — `pmix_server_trk_peer_lost` and
`pmix_server_grp_peer_lost`, both called from `lost_connection` — return
immediately for a peer with `finalized` set. A rank that dropped its
socket by calling `PMIx_Finalize` is still expected: `nlocalprocs` is
deliberately *not* decremented for it and its peer object is tombstoned
rather than retired, so it can `PMIx_Init` again and contribute. Counting
it in `departed` as well counts the rank twice and lets a collective
complete without it — which is how a client cycling init/finalize (MPI
Sessions) desynchronized its fence sequence by whole cycles (#4113).

**`pmix_server_trkr_t::lock` is dead.** `tcon` constructs it and `tdes`
destructs it, and nothing anywhere in the tree waits on it or wakes it —
the header comment calling it a "flag for waiting for completion"
describes an arrangement that does not exist. A collective's participants
are answered through the caddies on `local_cbs`, never by a lock. It is
the same shape as `pmix_peer_events_info_t::enviro_events` and
`pmix_iof_req_t::flags`: written (here, constructed) and never read. Do
not build anything on it, and do not read its presence as evidence that a
blocking waiter exists somewhere.

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

**A completion loop owes a reply to *every* caddy on `local_cbs`, and the
status it packs must survive the loop.** The three fence-family
completions (`_mdxcbfunc`, `_cnct`, `_discnct`) walk `local_cbs` packing
one reply per participant, and both halves of that were wrong:

- **Leaving the loop on a per-participant failure hangs the rest.** The
  timer was cancelled at the top and the tracker release at the foot
  frees the remaining caddies without a reply, so nothing in the tree
  will ever answer them. This is the fence-family twin of the group rule
  below ("detaching only the caller's and releasing answers that one
  client and silently frees the other N-1"). Serve the next participant
  instead: `continue` where the reply cannot be built, and — in `_cnct`,
  where the failure is per-participant job-info assembly rather than the
  status pack — hand *that* client the error and carry on.
- **`PMIX_GDS_MARK_MODEX_COMPLETE` assigns the variable it is given.**
  `_mdxcbfunc` passed it the same local that held the collective's
  status, one line after packing it, so every participant after the
  first was told a failed fence had succeeded. `gds/hash`'s entry point
  is a no-op returning `PMIX_SUCCESS`, so this fired on every ordinary
  server rather than in some corner. Keep the collective's status in a
  variable nothing else writes; use the pack-status local for the macro.
  Covered by `test/unit/server_fence.c`, whose two-participant case
  reads back what was queued for each.

The completion status is recorded into the tracker's info array by
`pmix_server_set_collective_status`, which locates the
`PMIX_LOCAL_COLLECTIVE_STATUS` slot **by key, never by position** —
connect appends per-participant endpoint info and job-level info *after*
that slot, so a positional write would clobber it.

**Finishing a tracker's definition is per-tracker, and adds to the
count.** `_register_nspace` and `_register_client` walk every incomplete
tracker when a registration lands, and both halves of that walk were
shared state when they should not have been. `all_def` - "are all of this
tracker's namespaces registered" - was initialized above the loop, so one
tracker naming a namespace we had not been told about left every tracker
behind it in the list marked incomplete, and whether a collective
completed depended on its position in `collectives`. And the
`PMIX_RANK_WILDCARD` arm *assigned* `trk->nlocal` where
`pmix_server_new_tracker` accumulates it, so a fence naming two wildcard
namespaces kept only the last-registered one's count: it went up to the
host as soon as that many local procs had contributed, and the rest
arrived to find the tracker gone and hung on a fresh one nobody would
complete. The count is now accumulated, guarded against double-counting by
the namespace's *previous* `nlocalprocs` - `SIZE_MAX` there is exactly the
question "could `new_tracker` have counted this one already?", since it
counts a namespace only when the count is known and every tracker on the
list predates this call. Both are covered by `test/unit/server_fence.c`.

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
and the host can return a tracker we already released. That cancel is
about *ordering* and is still owed at every up-call site;
`pmix_server_execute_collective` does it.

**The destructor is the backstop, not the ordering rule.** `tdes` deletes
an armed timer before freeing the tracker, so a path that tears one down
without completing it can no longer strand the event. It has to: the
teardown path does not complete what it destroys.
`PMIx_server_finalize` runs `PMIX_LIST_DESTRUCT` over
`pmix_server_globals.collectives` outright, and the event base outlives
that by a good distance — `PMIx_Progress_thread_stop` at the top of
`server_teardown()` only stops the loop, and the base is not freed until
`pmix_progress_thread_stop()` drops the last reference inside
`pmix_rte_finalize()`, several stanzas further down. So between those two
points libevent's timer heap held a pointer into a freed tracker, and
`event_base_free()` walks that heap calling `event_del()` on every entry
it finds. A host that finalizes while a `PMIX_TIMEOUT` collective is
still waiting on a participant reached it directly. The same backstop
covers the lost-connection sweep in
`src/mca/ptl/base/ptl_base_sendrecv.c`, which has never cancelled and is
reached precisely when a timer is most likely to be armed.

This is why `tcon` `memset`s `t->ev`: the guard on `event_active` is what
keeps the destructor from handing libevent an event that was never
assigned, and zeroing the member is what keeps that guard from being the
only thing standing between us and a garbage `ev_base` pointer.

**A handler that creates a tracker owes the collective a completion on
every path out.** Returning an error after `pmix_server_new_tracker` has
run — but before this caddy has been appended to `local_cbs` — answers
this caller through the switchyard and leaves the tracker parked on
`pmix_server_globals.collectives` with nothing in the tree that will ever
complete it. Any participant that contributed ahead of the failure waits
in its collective forever, and a later call over the same participant set
finds and reuses the stranded tracker along with whatever half-assembled
info was left on it. `pmix_server_disconnect` has no such window: it
appends the caddy on the line after it obtains the tracker.
`pmix_server_connect` does, because the optional endpoint and job-info
blocks were threaded in between the two, and its four error arms there
now drive `cbfunc(rc, trk)` — which replies to every caddy on
`local_cbs`, unlinks the tracker and releases it. There is no detach to
do first, precisely because this caddy never joined. Covered by
`test/unit/server_connect.c`, whose case drives the handler with
`pmix_server_cnct_cbfunc` rather than an inert stub — a stub cannot tear
a tracker down, so it cannot tell the two behaviors apart — and holds the
length of the collectives list against itself across the call.

**Driving a tracker's completion claims it — `trk->completion_fired` says
so.** `host_called` covers only the handoff to the host. A collective the
host never sees — a strictly local fence is the *ordinary* case under
`fence_localonly_opt`, and two error paths deliberately set `host_called`
back to false — is finished by calling the tracker's own completion
function, and every call site says the same reassuring thing: "the
modexcbfunc thread-shifts the call prior to processing, so it is okay to
call it directly from here." That is true about re-entrancy and
misleading about lifetime. The shift means the tracker is still on
`pmix_server_globals.collectives` when the call returns, and still
answers `pmix_server_trk_complete()` — `local_cbs` is drained by the
handler and by nothing earlier. So for that window the tracker looks, to
anything walking the list, exactly like a collective that is complete and
unclaimed.

Two handlers for one tracker is a double free: each unlinks it from
`collectives` and releases it, so the second reads and re-releases freed
memory and unlinks through freed links. The list corruption outlives the
event, and the abort usually lands much later — commonly in
`PMIX_LIST_DESTRUCT(&pmix_server_globals.collectives)` inside
`PMIx_server_finalize`, which is why openpmix#4112 reads as a teardown
bug. The trigger is routine: four ranks finalize right after a fence, so
a dropped socket reaches the sweep while the fence's own completion is
still queued.

The flag is set in `pmix_server_modex_cbfunc`, `pmix_server_cnct_cbfunc`
and `pmix_server_discnct_cbfunc` — one choke point per family, right
before the thread-shift — rather than at the dozen sites that drive a
completion, so a new site cannot forget it. It is set only once the shift
is certain: the `PMIX_NEW` failure returns above it leave the tracker
unclaimed, which is what lets a later sweep still rescue it. Everything
that can reach a tracker honors it: the lost-connection sweep, both
collective timeouts (a timer whose event is already queued fires even
though every handoff deletes it), and `pmix_server_get_tracker`, which
must refuse to join a new contributor to a dying tracker — that caddy
would be freed with the tracker and never answered, hanging a client
whose only mistake was to call its next collective promptly.

**`pmix_server_execute_collective` is the fourth up-call site, and it owes
everything the other three do.** It is what fires a collective that a late
`register_nspace` or `register_client` has just unblocked, and it had none
of the discipline its siblings carry: it never cancelled the timer, never
set `host_called`, never checked the host entry point was non-NULL (a
comment claimed `pmix_server_new_tracker` had done so - it does not), and
discarded the host's return value entirely, so a refusal left every
participant blocked forever on a tracker stranded on the `collectives`
list. Its own pack failures returned the same way. A collective reached
through this path has no switchyard caddy to hand back - every caddy on
`local_cbs` is already the tracker's - so the way to fail it is to drive
the completion function with the error, which replies to each participant
and tears the tracker down. That is what `fail_collective` is for.

**Never build the modex bucket anywhere but `pmix_server_collect_data`.**
`pmix_server_execute_collective` used to assemble it inline, and the copy
had drifted: it shipped the bare collect-type byte plus rank blobs with
neither the compression flag nor the enclosing byte object that
`pmix_gds_base_store_modex` unpacks, and it packed with the first
participant's `bfrops` module rather than `pmix_globals.mypeer`'s. So
every fence that had to wait on a registration handed the host a blob no
receiving server could parse - invisible to `make check`, because a
single-node run never defers a fence. The clone de-duplication that copy
carried now lives in `pmix_server_collect_data`, where both callers get
it: a fork/exec'd clone shares its parent's `pmix_rank_info_t`, so
identity of `&peer->info->pname` is what tells the two apart.

**A tracker handed to `PMIX_EXECUTE_COLLECTIVE` crosses an async hop, so
the caddy takes a reference.** `pmix_trkr_caddy_t` used to carry a bare
pointer, and anything that ran between `pmix_event_active` and the
handler - a departing participant completing the collective, a timeout -
could release the tracker underneath it. The caddy now retains and its
destructor releases. As with the dmodex tracker, **the reference keeps the
object alive but does not put it back on the `collectives` list**, so
`pmix_server_execute_collective` still asks `collective_is_pending` before
acting on it, exactly as `_process_dmdx_reply` asks `tracker_is_pending`.

**Read `PMIX_TIMEOUT` into a variable of the width you ask for.**
`PMIx_Value_get_number(value, dest, type)` writes exactly `sizeof(type)`
bytes through `dest`. Handing it `&tv.tv_sec` while asking for
`PMIX_UINT32` therefore fills four bytes of a `time_t` — the low half on
a little-endian host, which happens to work, and the high half on a
big-endian one, which multiplies every timeout by 2^32. Convert through a
`uint32_t` of its own and assign. The fence, connect and get handlers all
do this now; `pmix_server_group` always did.

**The modex blob handed *up* to the host belongs to the host.**
`pmix_server_fence` unloads the assembled bucket into a bare `char *data`
and passes it to `pmix_host_server.fence_nb`; so does
`pmix_server_execute_collective`. Neither frees it, and that is not an
oversight to "fix": the request direction carries no
`(release_fn, release_cbdata)` pair the way the reply direction does, and
a host is entitled to park the pointer and read it later from another
thread — `fencenb_fn` in `test/simple/simptest.c` does exactly that — so
freeing it on return would be a use-after-free. Ownership transfers on
the call, and `pmix_server_fencenb_fn_t` in `include/pmix_server.h` now
says so, as does the
[`pmix_server_module_t(5)`](../../docs/man/man5/pmix_server_module_t.5.rst)
man page. It did not until August 2026, and both in-tree hosts leaked it:
PRRTE still does ([prrte#2649](https://github.com/openpmix/prrte/issues/2649)),
and `simptest` was the reason a naive valgrind run of the simple suite
showed a per-fence leak here. `simptest` now hands it back through the
release function the modex callback takes — which is the point worth
carrying: `pmix_server_modex_cbfunc` only thread-shifts, so a host cannot
free the blob when that callback returns either.

**`pmix_cb_t::copy` is advisory and no fetch honors it.** Several sites
here set `cb.copy = false` with a comment about letting the GDS return a
pointer into local storage; `gds/hash` marks the parameter unused and
`pmix_hash_fetch` allocates a fresh `pmix_kval_t` for every value it
returns. So `PMIX_LIST_DESTRUCT(&cb.kvs)` is always correct and never
drops a reference the datastore still holds. Do not "optimize" a caller
on the theory that `copy = false` handed back a borrowed pointer.

**`cb.info` is inert on a fetch too**, and that is what makes
`check_req` safe: it re-runs `_satisfy_request` against a stack
`pmix_server_caddy_t` holding nothing but `pmix_globals.mypeer`, so the
directives the requester sent are not carried into the second fetch.
Neither `gds/hash` nor `gds/shmem3` reads the member. Sites set it
because the signature has it, not because anything downstream looks. A
component that starts honoring a fetch qualifier would have to fix the
deferred path at the same time — `req->lcd->info` is where those
directives still live.

**`pmix_pending_resolve` always returns `PMIX_SUCCESS`.** Its callers
check the status anyway, which is harmless, but do not build error
handling on it — in particular `pmix_server_commit` returns that status
to the client as the status of its *commit*, which would report one
proc's `PMIx_Commit` as failed because some other proc's parked get could
not be drained.

### A contribution can be a delta

With `pmix_server_fence_delta_modex` set, a server contributes only what
its local processes have committed **since they last took part in a
collecting fence**, rather than each one's whole published set. Three
things make that sound, and each is easy to undo:

- **The participant set has to match.** A delta is only correct for a
  fence over the same participants: contributing one to a fence over some
  *other* set leaves every server holding only that set's procs never
  learning the keys it left out - two sub-communicators fencing
  independently is enough to reach it. Each `pmix_rank_info_t` carries a
  digest of the set it last contributed to (`participant_signature`), and
  anything but an exact match falls back to the full set. Equality rather
  than containment is deliberate: it can only cost an unnecessary
  cumulative contribution, never a short one.
- **The watermark moves only once the host has the bucket.**
  `pmix_server_modex_contributed` is called after the `fence_nb` up-call
  is accepted, never from `pmix_server_collect_data` - that runs earlier,
  and its caller has three arms that discard the bucket. Draining there
  loses the deltas for good, because the datastore still holds the values
  but nothing else remembers which ones this rank had yet to send.
- **`pmix_server_commit` is not the only writer.** Remote-scope data also
  reaches our store through `PMIx_server_register_resources`
  (`pmix_server_setup.c`) and the group collective
  (`pmix_server_group.c`), neither of which goes through the commit path
  and so neither of which is on the pending list. Both call
  `pmix_server_modex_resync` to force that proc's next contribution to be
  cumulative. **Any new such writer owes the same call.**

The contribution is marked `PMIX_MODEX_DELTA` in the envelope's
per-server flag byte so the receiving datastore knows it is not
self-contained. The whole bucket is one kind or the other - that byte
describes the server's contribution as a whole - so a delta is used only
when *every* local participant qualifies.

### A deletion has to survive the collection, not just reach it

The modex is additive, so a contribution that merely stops carrying a
key removes nothing at the far end — a deletion has to be *stated*, as
an entry whose value is `PMIX_UNDEF`. That is what
`pmix_rank_info_t::pending_deletes` is for, and
`pack_pending_deletes()` appends it to the rank's blob.

**The trap is that the rank with a deletion to announce is exactly the
rank the collection is most likely to skip.** `pmix_server_collect_data`
builds a rank's blob around a `PMIX_REMOTE` fetch from the datastore,
and that fetch answers `PMIX_ERR_NOT_FOUND` once the last remote key the
rank published has been deleted — indistinguishable from a rank that
never published anything. Building the blob only on a successful fetch
therefore dropped the deletion, and dropped it permanently:
`pmix_server_modex_contributed` drains `pending_deletes` as soon as the
bucket reaches the host, so nothing re-announces it and every other
server goes on serving the key for the life of the job. The blob is now
built when there is **either** data or a pending deletion. Note this was
the default path — `fence_delta_modex` is off unless asked for, and the
delta arm has always packed the deletes unconditionally. Covered by
`test/unit/server_fence.c`.

### Telling the clients a key is gone

`pmix_server_notify_deleted()` is how a removal reaches the copies this
server has already handed out. A client caches what it reads about other
processes and holds the job-level data it was given at init, so removing
a key here corrects only our own store.

Four things about it are deliberate:

- **Its own PTL tag** (`PMIX_PTL_TAG_DATA_DELETE`). A peer too old to
  know the tag never posted a receive for it, so it simply never gets
  one - the same reasoning IOF flow control uses for its tag. `PMIx_Put`
  refuses a delete against such a server up front, which is where the
  caller learns about it.
- **Every local client except the requester**, which applied the removal
  to its own store before sending. Not restricted to the affected
  namespace: a process may have cached data belonging to any namespace it
  asked about, and one that never held the key removes nothing.
- **One message per key.** Deletions are rare; this keeps the wire format
  a single-key statement rather than a list whose length has to be
  screened on receipt.
- **`_deregister_resources` calls it too**, through
  `retract_from_namespaces()`, which is what finally makes that call take
  information back from a running job. Note what that helper deliberately
  does *not* cover: the arm that prunes elements out of an entry rather
  than removing it. There the host asked for part of a value to go, so a
  deletion would take more than was asked; the right propagation is the
  pruned value, and there is no update push.

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

**And that includes finalize, where a `PMIX_LIST_DESTRUCT` of
`local_reqs` does nothing at all.** Destructing the list drops only each
tracker's *creation* reference, and every request parked on it holds one
of its own - so the tracker, its info array, each request, the server
caddy each request carries and the peer that caddy retains all survive,
now unreachable. Both roles that own the list (`PMIx_server_finalize` and
`PMIx_tool_finalize`) therefore drain it with
`pmix_server_fail_local_reqs` first, above the point where the peers are
released. `PMIx_Progress_thread_stop` has already run by then, so
`pmix_server_get_cbfunc` takes its `progress_thread_stopped` arm and
simply releases the caddy rather than trying to reply to a client on a
listener that is down. This is why `dmrqdes` does not release
`req->cbdata` itself and carries a comment saying so: the single
reference on that caddy travels with the request only until the request
is answered, and the two paths that discard one unanswered
(`discard_local_tracker`, which leaves it to the switchyard, and
`pmix_server_fail_local_reqs`) own it explicitly. Covered by the last
case in `test/unit/server_get.c`.

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
discard.

**And, exactly as for `local_reqs`, that includes finalize.** The trap
there is a different one and is easy to miss for that reason: a bare
`PMIX_LIST_DESTRUCT` of `remote_pnd` really does free everything on it -
`dmdes` releases the `pmix_setup_caddy_t` each entry carries - so it
leaks nothing and looks correct. What it drops is the
`pmix_dmodex_response_fn_t` the host supplied, which is the only thing
that will ever tell the host what became of its request. So the outcome
is a hang rather than a leak, on the far side of the machine: the remote
server that asked for the data, and the client blocked in `PMIx_Get`
behind it, wait forever - and the host process outlives our finalize, so
nothing later corrects it. Both roles that own the list drain it with
`pmix_server_drain_remote_pnd` first, and both do so after
`PMIx_Progress_thread_stop` has run, which is what makes touching the
list from the finalizing thread safe. Covered by the last two cases in
`test/unit/server_dmodex.c`. Note the response function's contract while you are there - the
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

**Exactly one of the two up-call sites gets to ask for any given
tracker.** `pmix_server_get` up-calls immediately when it cannot place the
target — the unknown-namespace path and the remote `refresh_cache` path
both land there — while `pmix_pending_nspace_requests` up-calls the
trackers that were parked *before* the local picture was complete. Those
two populations overlap: a request for a namespace we had never heard of
is up-called at once, and is still sitting on `local_reqs` when that
namespace finally registers, at which point the registration sweep finds
its target is not one of the new locals and asks the host a second time.
The refcounting survives that (each up-call takes its own reference, and
the second reply finds the tracker retired and simply gives it back), so
the symptom is only a wasted round trip and a host asked twice for one
proc — but a host that de-duplicates by target cannot tell the second
request from the first. `lcd->requested` records that the target is
already with the host; the sweep skips those.

**An invalid namespace off the wire is not inert — it is a wildcard.**
`PMIX_CHECK_NSPACE` short-circuits to `true` when *either* name is
invalid, and an empty string is invalid, so a `PMIx_Get` naming one
matches every tracker it is compared against. `create_local_tracker`
would join it to whatever tracker carried the same rank (answering that
requester with an unrelated proc's data), and — the worse direction —
every later request for that rank in a real namespace would join the
bogus tracker in turn. `pmix_server_get` therefore screens the unpacked
namespace with `PMIx_Nspace_invalid` before anything compares against it.
The same reasoning applies to any handler that takes a namespace off the
wire and then uses `PMIX_CHECK_NSPACE` on it.

When data arrives the host calls `dmdx_cbfunc` (off-thread) →
thread-shift → `_process_dmdx_reply` stores the returned data into GDS
then calls `pmix_pending_resolve` to drain every parked requester. Two
public re-entry points feed this engine from the registration flow:
`pmix_pending_nspace_requests` (fires deferred `direct_modex` once all
locals are known) and `pmix_pending_resolve` (drains trackers when data
lands or on commit). `get_timeout` fails a single waiting requester with
`PMIX_ERR_TIMEOUT`.

**`PMIX_LOAD_BUFFER` consumes the pointer it is handed**, NULLing the
source and zeroing its length — that is what distinguishes it from
`PMIX_LOAD_BUFFER_NON_DESTRUCT`. `_process_dmdx_reply` walks the returned
blob once per *unique requester namespace*, so the destructive form meant
the first pass consumed `caddy->data` and every later pass saw NULL and
fell into the neighbouring arm — the one that assumes the data arrived
through `register_nspace` — quietly transferring job-level data instead
of storing the modex the host had just handed us. Nothing failed: the
missing store is masked by `_satisfy_request`'s fall-back to the target
namespace's own gds module, which finds the copy the first pass did
write. Use the non-destructive form in any loop that re-reads one
payload, and disown the buffer (`pbkt.base_ptr = NULL`) before
destructing it, since it still points at memory the release function
owns.

**The OOM early-outs in `_process_dmdx_reply` must fail the waiters, not
just let go.** The reply is the only thing that was ever going to free
the clients parked on that tracker, so a path that cannot reach
`pmix_pending_resolve` — failing to create the landing-zone namespace, or
to name it — has to drain the tracker with `pmix_server_fail_local_reqs`
first. Releasing our reference and returning leaves the tracker on
`local_reqs` with every requester still blocked in `PMIx_Get`. The one
early-out that must *not* do this is the `tracker_is_pending` arm: those
waiters have already been told.

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
to `account_departed`.

Releasing a block destructs its `grp_trk_t`s and their `local_cbs`,
which is why the fence-family instinct — detach the current `cd`, then
release — reads as correct here. **It is not, once the local phase is
complete.** By the time the block is handed to the host, every local
participant has contributed and its caddy is on the block; detaching
only the caller's and releasing answers that one client and silently
frees the other N-1 replies, hanging those clients. Every such path —
the host refusing the up-call, `aggregate_info` failing, and the two
mirrors of both inside `account_departed` — instead hands the whole
block to `grpcbfunc` with the error, which replies to every participant
and then tears the block down. The caller then returns `PMIX_SUCCESS`,
because `grpcbfunc` has taken ownership of its caddy too. The one place
the detach-and-release shape is still right is the follower/bootstrap
arm, whose block is created for a single caller and holds exactly one
caddy.

For the same reason `_grpcbfunc` must never return early. Its block has
`host_called` set, so nothing else in the tree will ever complete it —
returning on a missing context ID or a `pmix_server_process_grpinfo`
failure left the block on `grp_collectives` for the life of the server
with its participants waiting forever. Record the failure in
`scd->status` and fall through to the reply loop.

Read that reply loop's `break`s carefully: each one abandons the reply
for the participant being served, and they are all directly inside the
`local_cbs` loop except the group-info pack, which sits one level
deeper. A bare `break` there escaped only the info loop and fell through
to `PMIX_SERVER_QUEUE_REPLY` on the buffer it had just released.

**A group id off the wire can be NULL, and everything here treats it as a
string.** The string unpacker spells "zero length" as a NULL pointer and
reports success, so `PMIx_Group_construct("")` from any local client
reached `get_tracker`, which `strcmp`s the id against every block on
`grp_collectives` and then `strdup`s it into a new one. The client
library screens a NULL pointer, which an empty string is not — and a
screen there would not help anyway, since the value arrives from a peer.
`pmix_server_group` rejects it before anything else looks at it. Same
class as the invalid-namespace screen in `pmix_server_get.c`.

**Driving a completion claims the block, and `host_called` is what says
so.** The name is now narrower than the meaning: the flag marks the local
phase frozen, whether it was frozen by handing the block to the host or
by driving `grpcbfunc` ourselves. It has to cover both, because
`_grpcbfunc` runs a thread-shift later — and until it does, the block is
still on `grp_collectives` looking complete and unclaimed. The internal
drives (the two `PMIX_OPERATION_SUCCEEDED` arms and the two
`aggregate_info` failures) did not set it, so a participant call landing
in that window found the block, appended its caddy, judged the local
phase complete and handed the *same block* to the host a second time —
which then completed on memory `_grpcbfunc` had freed. It is set in
`grpcbfunc` itself, one choke point rather than six call sites, and only
once the thread-shift is certain: the `PMIX_NEW` failure above it leaves
the block unclaimed and rescuable. This is the group family's spelling of
the fence family's `completion_fired`.

**`check_definition_complete` gives up on an unknown namespace, so
something has to call it again.** It returns the moment it meets a
participant namespace this server has not been told about, and for a long
time the *only* thing that ever called it again was the arrival of
another local participant. So a group naming a namespace that registers
late — or one that places no procs here at all, and is therefore
registered only when the job it belongs to is set up — was never marked
definition-complete once its last local participant had already
contributed. The block sat on `grp_collectives` for the life of the
server with every one of those participants blocked in
`PMIx_Group_construct`. `pmix_server_grp_check_pending()` is the group
family's counterpart to the collective walk `_register_nspace` and
`_register_client` already do for the fence family, and it is called from
the same two places. The completion tail it shares with
`account_departed` lives in `forward_to_host()`; **a third path that
discovers a block is locally complete must call that, not copy it.**

Two things here are safe and look as though they should not be. A block
handed to the host survives the call without any reference of its own —
unlike the dmodex tracker above — because `host_called` is set before
the up-call and `account_departed` skips any block carrying it, so
`_grpcbfunc` is the only code that frees a block. And
`notify_local_members_of_loss` hands a stack `pmix_info_t[2]` to
`pmix_server_notify_client_of_event` and destructs it on the next line:
that entry point deep-copies the array into its own caddy *before*
thread-shifting, so nothing is borrowed across the shift.

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
`{A, B}` sends both lists and we record `A` twice — each entry carrying
that handler's own `PMIX_EVENT_AFFECTED_PROC` filter. **That filter does
not gate delivery**, and must not: a handler registered later for an
already-active code sends no message at all, so the entries we hold do
not describe every handler the peer has. `_notify_client_event` forwards
on the code alone and the receiver filters per handler — see the fan-out
section of [`src/event/AGENTS.md`](../event/AGENTS.md). What still reads
the recorded filter is `_check_cached_events`, replaying against the
registration that is in front of it.
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

**The replay runs once per registration *message*, not once per peer, so
delivery has to be idempotent per peer.** The same fact that makes a peer
hold several entries for one code — the client sends another
`PMIX_REGEVENTS_CMD` for every handler naming a code new to us or
carrying a directive — means the same peer walks the whole notification
hotel several times, and the live dispatch may have delivered an event to
it before any of that. Nothing recorded what a peer had already been
sent: each replay re-sent the event, so the client's handler fired again,
and each replay decremented the event's `nleft` again, so an event
targeted at several procs was evicted from the cache before the rest of
them ever saw it. `pmix_notify_caddy_t` now carries a `notified` list and
`pmix_notify_mark_notified()` (in `src/event/pmix_event_notification.c`)
both queries and extends it. **Any path that hands a local peer a
notification on behalf of one caddy must go through it** — there are
three, and they live in three different subsystems: the live dispatch in
`_notify_client_event`, this replay, and the one a newly-connected tool
triggers in `src/mca/ptl/base/ptl_base_connection_hdlr.c`. The `trk`
namelist in the live dispatch dedups only within a single dispatch and is
not a substitute. The `nleft` decrement is also guarded on `0 < nleft`
now: it is a `size_t`, so the unguarded form wrapped to `SIZE_MAX` and
pinned the event in the hotel rather than evicting it. Covered by
`test/unit/server_events.c`, whose replay cases fail an unfixed library.

**The range is the last field of a `PMIX_NOTIFY_CMD` and both cache
replays used to omit it.** The live dispatch has always appended it; the
two replays stopped after the info array. A client ignores the trailer,
but a tool reads it to decide whether the event has to go on to its own
host and **defaults a missing one to `PMIX_RANGE_LOCAL`** — so every
event a tool picked up from the cache silently lost its range. All three
producers now agree. Appending a field at the end is the only wire change
the interop rules allow, and it costs an older reader nothing; see
[Wire format and interoperability](#wire-format-and-interoperability).

**The acknowledgement is queued before the replay, and it takes two
events to arrange that.** A client must have its registration's
completion callback — and with it the handler's reference index, which
is what an application keys per-handler state on and what it needs to
deregister a one-shot from inside itself — before any event the replay
digs out of the cache. The two arms that complete locally get it for
free: they return `PMIX_OPERATION_SUCCEEDED`, so the switchyard's caller
packs and queues the reply inline before the shifted
`_check_cached_events` can run at all. The host-async arm does not: there
the reply *is* `scd->opcbfunc`, and that callback
(`pmix_server_events_cbfunc`) thread-shifts once more before it queues
anything, so sent from inside the replay it landed behind every relay.

So `_check_cached_events` now does nothing but answer the requestor and
post `_replay_cached_events` as a second event. Both activations happen
on the progress thread, which is the only thread that drains the event
base and drains activations in the order they were made — the single
progress thread is a design invariant of the library, not an incidental
property of libevent, so this ordering is as solid as the "a shifted
event runs after we return" argument used everywhere else here.

Two consequences for anyone editing this. **The replay is one event-loop
turn later than it used to be** on all three arms, which is why
`test/unit/server_events.c` needs `settle()` (two barriers) rather than
one wherever it waits for a replay — a single barrier passes or fails on
timing. And **the reply must stay in `_check_cached_events`, not move
into the replay**: putting it back at the end of the replay is exactly
the defect, and the ordering case in that test fails when it does.

## IOF forwarding

Standard-I/O forwarding is buffered through `pmix_server_globals.iof`
(cache of `pmix_iof_cache_t`) with a `max_iof_cache` bound and an
`iof_residuals` list for partial (pre-newline) bytes. `PMIx_server_IOF_deliver`
is the public push; `pmix_server_iofreg` / `iofdereg` / `iofstdin` are
the client-driven pull/registration commands. Note `pmix_server_process_iof`
drains the cache into a newly registered request so a late subscriber
still sees recently produced output.

**A spawned job inherits its parent's forwarding, and the reason it has
to is worth understanding before touching `pmix_server_spawn_parser`.**
That parser used to default the output channels on only for a **tool**,
so a spawn issued by an ordinary client subscribed to nothing and the
child's output matched no request, was cached, and aged out unseen
([openpmix#4120][i4120]). The fix is *not* to forward the child's output
to the spawning client: a client has nowhere to put it — every place
that would arm local writing is deliberately tool-only, and
`pmix_globals.iof_flags.local_output` is false for a non-singleton
client — so that merely moves the drop one hop later.

What the child gets instead is whatever the *parent job* has.
"The parent's settings" are not stored anywhere as settings; what exists
is the set of live subscriptions covering the spawning process's job,
which is the same thing seen from the other end — **whoever receives the
parent's output should receive the child's**. `inherit_parent_iof()`
clones each such request onto the child's namespace, keeping the
requestor, channels, formatting *and `remote_id`*, so the child's output
lands at the requestor under the handler id the parent's already uses.

The ordering is: a channel named on the spawn wins, else the parent's
subscriptions. **There is deliberately no third level.** If nothing is
watching the parent there is nowhere for the child's output to go, and
falling back to forwarding it to the spawner would be a loopback rather
than a fallback — output is never forwarded to an application process,
which would only emit it again on its own stdout for the runtime to
collect and forward a second time. (An `iof_spawn_default` MCA parameter
was written for that slot and removed before merge for this reason;
PRRTE's existing `prte_rmaps_default_inherit` is the place to control
inheritance from, and layering a second one would only confuse users.)
`pmix_server_spawn_parser` reports the first of those through its
`inherit` out-parameter — **naming any one
output channel turns inheritance off for all of them**, including naming
one `false`, which is how a spawn asks for silence from a job whose
parent is being watched. A tool is not a member of a job, so it has no
parent and keeps its own "forward everything to me" default; `prun`
depends on that.

Two things fall out that are easy to get wrong:

- **`drain_cache()` is driven by namespace, not by request.** A job can
  inherit more than one watcher, and a cache entry is consumed the moment
  it is forwarded — so draining per-request would hand the cached head of
  the stream to whichever watcher was cloned first and the rest of the
  stream to all of them.
- **The clone is subject to the same screens `pmix_iof_process_iof`
  applies before forwarding** (non-NULL requestor, non-NULL
  `requestor->info`, not finalized). A request it would refuse is not
  worth inheriting, and the `info` read is load-bearing rather than
  defensive because the verbose line dereferences it.

**Inheritance is decided in two places, and the second is the one that
works on a multi-node DVM.** `pmix_server_process_iof()` runs on the
server that *processed the spawn* — the one hosting the spawning
process. The watching tool's subscription lives on the server the *tool*
attached to. On a single-node DVM or under `prterun` those are the same
server and the spawn-time clone is the whole story; on a multi-node DVM
they are not, and the clone gets built where the output never arrives.

So `_iofdeliver` carries the other half: when a chunk of output matches
no subscription — the point at which it would otherwise go into the
cache to age out — `inherit_from_ancestry()` walks the source
namespace's `PMIX_PARENT_ID` chain and clones the subscriptions covering
the first ancestor anybody here is watching. That decision is taken on
the server the output *reaches*, which for every PRRTE launcher is the
HNP: every prted forwards to the HNP, and the HNP hands everything it
receives to its own PMIx server, where a `prun`/`prterun` tool's
subscription is. Four things about it are load-bearing:

- **It clones rather than delivering the one chunk.** Every later chunk
  from that namespace then takes the ordinary walk above, so the
  datastore is consulted once per namespace instead of once per line of
  a watched job's output. The clone is made by the same
  `clone_iof_reqs()` the spawn-time path uses and has the same lifetime,
  so this adds no lifetime question that was not already there.
- **`PMIX_PARENT_ID` is per-process job data**, which is why the lookup
  works on a server that had nothing to do with the spawn: PRRTE adds it
  in `prte_pmix_server_register_nspace()` for every daemon that
  registers the namespace. `pmix_pfexec` records it for the job as a
  whole instead, so `get_parent_id()` tries the specific rank and then
  the wildcard.
- **The walk is transitive and bounded** (`PMIX_IOF_MAX_ANCESTRY`). A
  grandchild of a watched job is watched; the chain is host-supplied
  data, so the bound is what stops a malformed or circular one from
  spinning the progress thread.
- **The two halves are independent.** Neither needs the other to have
  run, and where both apply the second finds the subscription the first
  created and does nothing.

**The host gets the final say through `PMIX_IOF_INHERIT`, and where it is
read follows from the two halves being on different daemons.** The
attribute is a boolean the host places in the *job-level information of
the spawned namespace*; absent means "inherit", so a host sends it only
to say no. Nothing about it is read by the spawned processes — it is
server-side state that happens to be keyed by their namespace, read out
of the local datastore by `iof_inherit_allowed()` exactly as
`PMIX_PARENT_ID` is. That channel is what makes it reach the server the
output arrives at, which may have had nothing to do with the spawn; a
directive carried with the spawn request, or a server-wide default at
`PMIx_server_init`, would reach one of the two sites and not the other
(and the latter cannot express a per-job answer at all, while PRRTE's
`inherit` is per-job).

**At the spawn-time site, "the host said nothing" and "we cannot ask yet"
must not be conflated** — it is the one place they are distinguishable.
That site runs from the spawn *completion*, and whether the child's
namespace has been registered with this server by then is the host's
business: a daemon hosting none of the child's processes may never
register it. A fetch that found nothing would read as "inherit", which is
the wrong way to be wrong — it would clone against the host's wishes and
nothing takes a clone back. So `inherit_parent_iof()` looks the namespace
up in `pmix_globals.nspaces` first and returns without cloning when it is
absent, leaving the decision to the delivery-time half, which cannot run
before the namespace exists. Deferring is never behaviorally wrong; it
only postpones. That is also why `test/unit/iof_inherit.c` registers its
child namespace before driving the spawn-time cases — without that they
exercise the deferral rather than the inheritance.

What is still PRRTE's is a tool attached to a **non-HNP** daemon, and a
tool that attaches to a running DVM and `PMIx_IOF_pull`s a job's output:
the first never receives another node's output at all (the HNP relays
elsewhere only by `jdata->originator`), and the second is not recorded
against the job anywhere. Both need PRRTE to keep a *set* of interested
daemons; see `src/mca/iof/AGENTS.md` in PRRTE for the relay path, and
[`docs/how-things-work/iof_inheritance.rst`](../../docs/how-things-work/iof_inheritance.rst)
for the whole design.

Coverage: [`test/unit/iof_inherit.c`](../../test/unit/iof_inherit.c) for
both halves — note its delivery cases deliberately use a channel the
watcher did not subscribe to, so `pmix_iof_process_iof()` returns at its
channel test and the identity-only peer stubs never reach a pack. The
multi-daemon half is
[`contrib/dockerswarm/run-spawn-iof.sh`](../../contrib/dockerswarm/run-spawn-iof.sh),
whose case 3 puts the tool, the spawning rank and the child job on three
different daemons — the geometry in which nothing about the answer can
be local.

[i4120]: https://github.com/openpmix/openpmix/issues/4120

**The three client-driven commands own the arrays they unpack, and the
`pmix_setup_caddy_t` will not believe it without being told.** They are
the mirror image of the two public push APIs, which park the *caller's*
proc array and byte object on the same members and must detach them
before releasing (see the caddy-zoo section). Here the arrays are ours,
so the caddy needs `copied` set for `info` and a non-zero `nbo` for the
byte object — and both were missing. Note the two halves fail
differently: `pmix_server_iofreg` and `iofstdin` leaked only on their
error returns, because their completions (`_iofreg`, `stdcbfunc`) free
the array explicitly, while `pmix_server_iofdereg` leaked on *every*
deregistration, since `_iofdereg` has no such free. And `nbo` left at
zero frees the byte object's container while leaking its payload —
`PMIx_Byte_object_free` runs its destruct loop `n` times and then frees
the array, so zero means "free the box, keep the contents."

**A two-caddy handler's completion owes the switchyard caddy its
release.** `pmix_server_iofreg` and `iofdereg` park the switchyard's
`pmix_server_caddy_t` on `cd->cbdata` of their own `pmix_setup_caddy_t`
and then return `PMIX_SUCCESS`, so the switchyard lets go of it — and
`scaddes` does not reach `cbdata`, so releasing the setup caddy releases
nothing else. `_iofdreg` gets this right with three explicit releases;
`_iofreg` released only the setup caddy, so the server caddy and the
`PMIX_RETAIN` it holds on the requesting peer leaked on **every** IOF
pull registration — pinning that peer, and everything hanging off it, for
the life of the server. Both halves of the pair owe it: the outer
callback's `progress_thread_stopped` arm had the same hole, and so did
`pmix_server_spcbfunc`, the spawn completion, which nests the same way.
Whenever a handler nests one caddy inside another, write down which one
releases which; the destructors will not do it for you.

**A registration the host refuses has to come back out of
`pmix_globals.iof_requests`.** `pmix_server_iofreg` adds the
`pmix_iof_req_t` — which takes a `PMIX_RETAIN` on the requestor — before
the up-call, because the refid has to be in hand to report. `_iofreg`
removes it again when the host fails the request *asynchronously*; the
synchronous refusal did not, leaving an entry that pinned the peer for
the life of the server and went on matching output for a pull that was
never granted. Deregistration is deliberately not symmetric: it drops
the local entry *before* the up-call and does not restore it if the host
refuses, because the client asked to stop and the refid it would be
handed back could not be the same one.

**Every count these handlers read off the wire needs the round-trip
screen**, for the reasons set out under "What *does* need screening"
below. `pmix_server_iofdereg` is the `+ 1` shape: it sizes its array as
`ninfo + 1` so it can seed `PMIX_IOF_STOP` in the last slot itself, so a
count near `SIZE_MAX` wraps that sum to zero and writes the seed at
`info[SIZE_MAX]`. `pmix_server_iof_handler`, `iofreg` and `iofstdin`
are the plainer shape, but `iofreg` also copies its proc array again
afterwards walking the `size_t` rather than the `int32_t` the unpack
consumed. Covered by `test/unit/iof_output.c`, whose wrap case kills an
unfixed library rather than failing it.

## Inventory collection

`pmix_server_inventory.c` is short and reads as though several things in
it could be wrong. They are not, and the reasons are worth knowing
before you "fix" one:

- **An inventory sweep on a server with no `pnet`/`pgpu` components is a
  success, not a failure.** Both entry points in the `pmix_pnet` /
  `pmix_pgpu` API tables are always non-NULL (they are the `_base_`
  functions, wired up statically in each framework's `*_base_frame.c`),
  and each walks its `actives` list and returns `PMIX_SUCCESS` when that
  list is empty. So the `goto report` arms mean a component really
  failed. What that reasoning assumes is that the framework was
  *opened* - see the next entry, which is the one thing here that really
  was wrong.
- **"The library is initialized" is not the question these entry points
  have to ask; "the *server* library is initialized" is.** Both of them
  fan out to `pmix_pnet` and `pmix_pgpu`, and those frameworks are opened
  in exactly one place in the tree: `PMIx_server_init`. Until then
  `pmix_pnet_globals.actives` and its `pgpu` twin are only
  `PMIX_LIST_STATIC_INIT`, whose sentinel `next` is NULL - so the
  `PMIX_LIST_FOREACH` in each base fan-out dereferences NULL rather than
  finding the list empty. `pmix_globals.initialized`, which is what both
  entry points screened on, is set just as readily by `PMIx_Init` and
  `PMIx_tool_init`; so a tool that called either API was answered
  `PMIX_SUCCESS` and then took a SIGSEGV on the progress thread, where
  the man page's `PMIX_ERR_INIT` ("the PMIx server library has not been
  initialized") is the documented answer. The screen is
  `pmix_server_globals.initialized`, set at the foot of `PMIx_server_init`
  and cleared at the top of `server_teardown()`. **The process type is not
  a usable stand-in**: `PMIX_PROC_LAUNCHER` is defined as
  `PMIX_PROC_TOOL | PMIX_PROC_SERVER | PMIX_PROC_LAUNCHER_ACT`, so a
  launcher that has come up through `PMIx_tool_init` and has not yet
  called `PMIx_server_init` reads as a server while none of this state
  exists. Covered by `test/unit/server_inventory.c`, whose refusal
  cases run in a forked child because against an unfixed library the
  crash lands after the call has already returned.

  **Every other public `PMIx_server_*` entry point screens the same wrong
  flag**, and `pmix_server_globals` has the same static-initialization
  problem its lists do. The remaining entry points are listed in
  `docs/todo.rst`; each one needs checking against its real callers
  before the screen is added, because some of them (the `preg` and hwloc
  helpers in `pmix_server_setup.c`, for instance) work perfectly well in
  a client and must keep doing so.
- **`PMIx_Info_list_convert` copies.** It `PMIx_Info_xfer`s every element
  into a freshly created array, so `clct` owns what it hands back *and*
  still owes the list a `PMIX_LIST_DESTRUCT`. Doing both is not a double
  free. `PMIX_ERR_EMPTY` from it means "nothing collected" and is mapped
  to success with a NULL array.
- **`cirelease` is handed to the *host*, so it can run on the host's
  thread.** That is safe only because it touches nothing global — it
  frees the converted array and releases the caddy. Do not grow it into
  something that walks `pmix_server_globals`; thread-shift instead.
- The blocking arm of `PMIx_server_deliver_inventory` returning
  `PMIX_OPERATION_SUCCEEDED` rather than `PMIX_SUCCESS` is the
  convention every blocking `PMIx_server_*` form here follows (see
  `pmix_server_setup.c` and `pmix_server_registration.c`), not a slip.

## Resolving peers and nodes

`pmix_server_resolve.c` answers the two "convenience" queries. Both entry
points have the same shape: unpack the request, build a one-element
`pmix_query_t` on `cd->query` describing it, offer it to
`pmix_host_server.query` because the host has the more global view, and
only if that comes back with anything other than `PMIX_SUCCESS`
thread-shift to a local handler that answers out of our own GDS. The two
local handlers are also the fall-back target of the *reply* path: the
error arms of `pmix_server_respeers_cbfunc` / `_resnodes_cbfunc` shift the
same caddy to them when the host accepted the query and then failed it.
So the local handlers may be entered from two places, and both rely on
`cd->query->qualifiers` still carrying what the entry point put there —
two elements for peers (nspace, hostname), one for node (nspace).

**`docs/how-things-work/resolve.rst` is the contract, and it is not the
one you would guess.** Read it before changing any status these handlers
return; several answers that look like unreported failures are what the
document requires:

- A namespace that is known but currently has no nodes assigned answers
  `PMIX_SUCCESS` with a **NULL** node list. That is a successfully
  executed request, not a miss, and `PMIx_Resolve_nodes` says exactly the
  same thing when it resolves out of the client's own store — so a server
  that handed the raw fetch status back made one server answer one
  question two different ways depending on which side computed it, and
  cost the caller a documented success. Only `PMIX_ERR_NOT_FOUND` gets
  this treatment; `PMIX_ERR_INVALID_NAMESPACE` (we have never heard of
  it) and `PMIX_ERR_DATA_VALUE_NOT_FOUND` (the host never supplied the
  data) are real answers and go back as they are.
- The peers side is asymmetric on purpose. A node that is not on the
  namespace's list is `PMIX_SUCCESS` with zero procs, but a node that
  *is* on the list with no `PMIX_LOCAL_PEERS` recorded against it is
  `PMIX_ERR_DATA_VALUE_NOT_FOUND` — the document spells out both. Do not
  "make these consistent."

**The tri-state does not apply to the `query` up-call here.** Both entry
points treat anything other than `PMIX_SUCCESS` — including
`PMIX_OPERATION_SUCCEEDED` — as "the host is not answering this one" and
fall through to the local handler. That is right rather than a missed
arm: the whole product of this up-call is an info array delivered through
the callback, so there is no coherent "done now, no callback" result to
receive, and the host having declared itself done means it will not call
back and cannot double-answer the client. Same reasoning as the
`direct_modex` up-call in `pmix_server_get.c`.

**The aggregate peer walk counts in one pass and fills in another, and
the two have to agree.** A request naming no namespace walks
`pmix_globals.nspaces`, builds one `"<nspace>:<peerlist>"` string per
namespace with `pmix_asprintf`, and accumulates the peer count as it
goes; it then sizes a `pmix_proc_t` array from that count and walks the
strings *again*, splitting each back apart to fill it. The split used
`strchr`, i.e. the **first** colon — but a namespace is an arbitrary
string the host assigned and nothing forbids a colon in it, so a
namespace such as `"res:ut,x"` yielded three comma-separated fields on
the way back where two peers had been counted on the way out, and the
third was written past the end of the array. Split from the right
(`strrchr`): the delimiter we inserted is the last colon, because the
peer list is a comma-delimited list of ranks. And bound the fill by the
allocated count anyway — a peer list is host-supplied data and the two
passes agreeing is not something this function can prove. Note also that
the array must be freed with the count that was *allocated*, not the
count that was filled, on every path out.

The whole file is covered by
[`test/unit/server_resolve.c`](../../test/unit/server_resolve.c), which
registers a colon-bearing namespace precisely to hold the two passes
against each other.

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
  grew its own check. Its `get_credential` case is the one place in the
  suite that reads a *reply* back: `PMIX_SERVER_QUEUE_REPLY` never arms
  the send event for a peer whose `sd` is negative, so the message comes
  to rest on `peer->send_msg` and a single process can unpack what the
  server actually packed. Use that idiom rather than a host stub's
  arguments when what you need to pin down is what crossed the wire.
- [`test/unit/server_group.c`](../../test/unit/server_group.c) drives
  `pmix_server_group` from hand-packed wire buffers against a host stub
  that declines every request — declining is what makes each case leave
  `grp_collectives` empty, since the refusal arm answers every
  participant and tears the block down. Its empty-group-id case takes an
  unfixed library down with SIGSEGV rather than failing it, and its
  late-registration pair is the only thing in the suite that reaches
  `pmix_server_grp_check_pending`: the block parks, the namespace
  registers, and the block must then go up to the host. Its driver
  thread-shifts, because the handler touches
  `pmix_server_globals.grp_collectives` and must not be called from
  `main()`.
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
  A further case builds three trackers in a known list order and drives
  a `PMIx_server_register_nspace` across them, pinning both halves of the
  tracker-update walk above: that a registration adds to a hybrid
  tracker's local count rather than replacing it, and that a tracker
  naming an unregistered namespace does not mark the trackers behind it
  incomplete.
  A separate case drives the *completion*, `pmix_server_modex_cbfunc`,
  over a hand-built two-participant tracker with a failing status and
  reads back what was queued for each — the shape that catches both a
  reply loop that abandons participants and a status the loop clobbers.
  It reads the replies off `send_msg`/`send_queue`, per the
  `test/unit/server_control.c` idiom below.
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
- [`test/unit/server_setup.c`](../../test/unit/server_setup.c) covers the
  job-preparation calls above, driving every one of them through its
  public entry point in the blocking form — which makes the ordering
  deterministic without a single sleep, since a blocking
  `PMIx_server_*` call does not return until its handler has run. It
  builds its `PMIX_GROUP_JOB_INFO` blob with
  `PMIx_server_collect_job_info`, which is what produces that format in
  the first place, and then asserts both halves of the borrow rule: that
  the call reports success rather than end-of-buffer, and that the
  caller's byte object still has its bytes afterwards. It pairs every
  mistyped array with a well-formed one, so the screens are held to
  accepting what they should. Three groups of case there — the argument
  screens and both mistyped arrays — take an unfixed library down rather
  than failing it; that was checked by reverting each screen in turn, and
  a regression therefore looks like an empty log and exit 139, not a FAIL
  line.
- [`test/unit/server_resolve.c`](../../test/unit/server_resolve.c) drives
  `pmix_server_resolve_peers` / `pmix_server_resolve_node` from
  hand-packed wire buffers against a host module with no `query` entry
  point, so every request takes the local handler. It registers three
  namespaces — a plain one, one whose *name* carries the `:` delimiter
  the aggregate walk inserts, and one with no node map at all — and reads
  the replies back off `send_msg`, per the `test/unit/server_control.c`
  idiom. The colon-bearing namespace is what holds the aggregate walk's
  counting pass and its filling pass against each other; against an
  unfixed library that case reports the wrong peer count and may take the
  process down on the out-of-bounds write first. The bare namespace pins
  the "known namespace, no nodes assigned" answer that
  `docs/how-things-work/resolve.rst` requires to be a success.
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
- **Every framework `PMIx_server_init` opens, `PMIx_server_finalize` must
  close.** `pmix_rte_finalize` closes the frameworks `pmix_rte_init`
  opened and no others, so the four this file opens — `pmdl`, `psensor`,
  `pnet`, `pgpu` — are ours to give back. `pmdl` was missing for as long
  as it has existed, and the shape of the damage is worth knowing because
  a static build hides all of it: the framework's reference count never
  reaches zero, so its close function never runs, so
  `pmix_pmdl_globals.actives` keeps the modules it selected *and*
  `selected` stays true. A second `PMIx_server_init` then finds the
  framework already open, rebuilds nothing and re-selects nothing, and
  walks a list of modules that live inside component DSOs the repository
  unloaded during the first finalize. Under `--enable-mca-dso` that is a
  segfault on the first `parse_file_envars`; statically linked it is
  merely a leak, which is why it survived. Covered by
  `test/unit/runtime_init.c`'s second cycle, but **only** in the CI
  `mca-dso` job — check a new open/close pair by hand against
  `grep -c framework_open` / `_close`.
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
  same screen**. The group handler was the fourth: it builds its array as
  `ninfo = ninf + 1` and seeds `PMIX_LOCAL_COLLECTIVE_STATUS` at
  `info[ninf]` before the unpack, so a count near `SIZE_MAX` wrapped that
  to a zero-element array — `PMIx_Info_create` answers NULL, which
  nothing checked — and wrote the seed at `info[SIZE_MAX]`. Its proc
  count had the mirror problem. Both now carry the round-trip screen.

  **The two event handlers were another.** `pmix_server_register_events`
  screened its code count and not its info count, and
  `pmix_server_event_recvd_from_client` screened neither - and the
  latter has the `+ 1` shape, sizing its array as `ninfo + 1` so the
  `PMIX_SERVER_INTERNAL_NOTIFY` marker can be seeded in the last slot.
  With `sizeof(pmix_info_t) == 552`, the count `2^61` wraps the product
  to exactly zero; `malloc(0)` hands back a live pointer, so the
  constructor loop then walks `2^61` elements off the end of it. Both
  now carry the screen, and `test/unit/server_events.c` crashes an
  unfixed library on either one.

  **The two fabric handlers were another.** `pmix_server_fabric_register`
  sizes the query caddy's info array from the wire count, and
  `pmix_server_device_dists` sizes the *server* caddy's - and `qdes` and
  `cddes` both walk that `size_t` when they free. Neither screened it,
  and neither checked what `PMIX_INFO_CREATE` handed back. Covered by
  `test/unit/server_fabric.c`, whose two count cases crash an unfixed
  library.

  **The classic commands in `pmix_server_ops.c` were the last group
  without it.** Publish, lookup and unpublish all size their array as
  `ninfo + 1` so they can seed `PMIX_USERID` in the last slot, and spawn
  sizes an info array and an app array straight from the wire —
  `PMIx_App_create` multiplies and constructs exactly as
  `PMIx_Info_create` does, and `scaddes` then walks the `size_t` when it
  frees. All five counts now carry the screen. The key counts in lookup
  and unpublish deliberately do **not**: each key is unpacked one at a
  time inside the loop, so an absurd `nkeys` simply runs the buffer dry
  and fails on the first short read — it never reaches an allocator.

  **What none of the three collective families screens is the count the
  unpack came back with**, and that was examined and left alone.
  `PMIX_BFROPS_UNPACK` fills `min(packed, provided)` and reports success,
  so a client that declares 100 procs and sends 2 gets a tracker whose
  `pcs` tail is 98 default-constructed entries with an empty namespace.
  Nothing corrupts: the namespace walk in `pmix_server_new_tracker`
  compares with `strcmp`, so an empty name matches no registered
  namespace rather than wildcarding onto every one, and each such entry
  merely marks the tracker non-local. The collective then never completes
  and the client hangs itself. Fence, connect and disconnect all behave
  this way, so a screen added to one would be an asymmetry rather than a
  fix; if you add one, add all three.

  **That decision turns on "nothing corrupts", so do not carry it to a
  handler where the tail becomes state somebody else reads.**
  `pmix_server_job_ctrl` walks its declared `ntargets` and *creates* a
  `pmix_namespace_t` for any target namespace it does not recognize, so
  each default-constructed entry appended one named `""` to
  `pmix_globals.nspaces` for the life of the server — and unlike
  `pmix_server_new_tracker`'s `strcmp`, most of the list walks in this
  directory use `PMIX_CHECK_NSPACE`, which reports an empty name as
  matching *every* namespace and would stop at the phantom. It also
  handed the host that many `pmix_proc_t`s of which most were never
  sent. It now trusts the count the unpack came back with — and frees
  the array with the count it was *allocated* with when that count is
  zero, since `PMIX_PROC_FREE` does nothing for `n == 0`. The test for
  which rule applies is not "is this a collective" but "does anything
  outlive the request".

  Note also what the unpack itself protects you from, so you screen for
  the right reason. `pmix_bfrops_base_unpack` reads the count the
  *packer* wrote and unpacks `min(packed, provided)`, reporting success
  whenever the storage was big enough. So handing it a `cnt` larger than
  the array on the wire is harmless — `pmix_server_publish` passed
  `ninfo + 1` where its two siblings pass `ninfo`, and that over-read
  never did anything. What it did do was make the guard around the
  unpack (`0 < cd->ninfo`, always true) fire for a publish carrying no
  info objects at all, where the buffer is already exhausted; gate on
  the wire count, not on the array size that carries your extra slot.

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

  **The same rule covers a field that is missing rather than mistyped.**
  `pmix_server_query` hands its unpacked queries to `pmix_parse_localquery`,
  which walks each one's `keys` array to a NULL terminator — and the
  `PMIX_QUERY` unpacker leaves `keys` at the NULL its constructor set
  whenever the peer declared zero of them, reporting success. So a query
  naming no keys at all was a NULL dereference on the progress thread.
  `PMIx_Query_info_nb` had always screened it, which buys a server
  nothing: that check runs in the *requestor's* process. The screen now
  sits in `pmix_parse_localquery`, beside the qualifier one and for the
  same reason — put it where every reader passes, not at one entry point.
- **The registration entry points build global state, so a failed
  allocation must not be left on a global list.** `_register_nspace` and
  `_register_client` both `strdup` the namespace name straight into a
  freshly created `pmix_namespace_t` and appended it unchecked - and every
  lookup in this directory `strcmp`s that member, so a transient failure
  became a permanent segfault of the progress thread. `_register_client`
  also trusted the "called only once per rank" contract in its own
  comment: the "have we got everyone" test is an exact equality against
  the length of `nptr->ranks`, so a second entry for a rank pushes that
  list permanently past `nlocalprocs`, `all_registered` is never set, and
  every collective involving the namespace hangs with nothing reporting
  why. It now says so with `PMIX_ERR_DUPLICATE_KEY` instead.
- **The `nlocalprocs < 0` arm of `_register_nspace` is the *update* path,
  and it has its own rules.** It stores each directive through the gds
  module by hand rather than going through `PMIX_GDS_ADD_NSPACE`. Two
  things there are easy to get wrong: a `PMIX_VALUE_XFER` whose status is
  discarded hands the datastore a value that was never populated (and the
  store's own status then overwrites the transfer's), and the
  `job_info_recvd` guard only works if something sets the flag - only the
  full-registration path did, so a second update carrying
  `PMIX_JOB_INFO_ARRAY` re-cached the whole array.
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

  What the client *does* report is a failure to **store** what arrived,
  which is a different thing and is not a legitimate outcome the way a
  not-found answer is. Do not read the two as one rule.
- **`pmix_server_job_ctrl` creating a namespace for an unknown target is
  deliberate.** A job-control request may name a job this server has not
  been told about yet, and the epilog directives need somewhere to hang;
  `_register_nspace` looks the namespace up by name and reuses the entry
  when registration eventually arrives. It is not a leak and not a
  phantom — for a target the peer actually sent. See the note above on
  why the walk must use the count the unpack came back with: the same
  code reached with a declared count larger than the packed array
  created a namespace named `""`, which is a phantom and does corrupt.
- **An epilog list's duplicate scan must read the list it appends to.**
  `pmix_server_job_ctrl` applies three of them per target — ignores,
  cleanup directories, cleanup files — and they are near-copies of one
  another. The ignores loop was cloned from the files loop and kept its
  scan list, so it asked whether the path was already registered for
  *cleanup* and then appended to `ignores` regardless of the answer.
  Both halves of that were wrong, and neither is visible from the call
  site: a repeat of the same `PMIX_CLEANUP_IGNORE` was never recognized,
  so every job-control request carrying one appended another copy to a
  list that lives as long as the namespace or peer and that
  `dirpath_destroy` walks once per file; and an ignore naming a path
  already registered for cleanup was dropped, so the epilog deleted a
  file the client had asked it to leave alone. Note that `ignores` is
  read *only* by `dirpath_destroy` (`src/include/pmix_globals.c`), which
  consults it for the directory itself and again for each file inside
  it — so an entry there is meaningful whether or not the same path is
  also on `cleanup_files`.
- **A conflicting cleanup request is applied in part before it fails.**
  The ignores are registered first, and the directory and file loops
  then error with `PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES` when a path
  they were asked to clean is on the ignore list — by which point this
  request's ignores, and any directories accepted ahead of the
  conflicting one, are already on long-lived epilogs. The client is told
  the request failed and the server keeps half of it. What survives is
  the conservative half (a path is ignored rather than deleted), which
  is why this is recorded rather than repaired: making it atomic means a
  validate-then-apply split, and the exact semantics of a partially
  conflicting request is a decision for the Standard rather than for
  this file. See `docs/todo.rst`.
- **Two things in `pmix_server_get.c` look like defects and are not.**
  `get_job_data` returns `PMIX_SUCCESS` with an empty buffer when the
  fetch finds nothing, and its callers pass that empty payload to the
  client as a success. That is safe and should be left alone:
  `_getnb_cbfunc` in `pmix_client_get.c` initializes its reported status
  to `PMIX_ERR_NOT_FOUND` and only overwrites it if a value actually
  turns up, so an empty success degrades to not-found at the requester -
  the same bargain as the `refresh_cache` status above.
- **`PMIX_OPERATION_SUCCEEDED` is not a permitted return from `spawn`,
  `direct_modex`, `get_credential` or `validate_credential`, and all
  four now say so.** The tri-state's third arm means
  "done now, no callback — you invoke the completion yourself", which
  works only where the operation's whole result is a status. These four
  produce a result the return code cannot carry — the namespace of the
  job that was launched, the requested data blob, and the credential or
  its validation — and the callback is its only channel. The two
  credential up-calls are the sharpest case of it, because
  `pmix_credential_cbfunc_t` and `pmix_validation_cbfunc_t` carry no
  `(relfn, relcbdata)` pair either, so there is no second channel even in
  principle; left alone, the third arm fell into the error return and the
  client read the synthesized status-only reply as a success carrying no
  credential. Both `direct_modex` sites here already treated
  the status as a refusal; they now emit the
  `atomic-completion-unsupported` diagnostic naming the operation, as
  `pmix_server_spawn` and `PMIx_Spawn_nb` do, so a host learns why its
  request was failed rather than having it silently degrade. **The
  library always supplies a non-NULL `cbfunc` to both**, so a host that
  finished the work immediately invokes the callback — it may do so
  before returning — and returns `PMIX_SUCCESS`. Every other up-call in
  this directory can express its result in the status, and takes the
  third arm normally. This is documented for hosts on the
  [`pmix_server_module_t(5)`](../../docs/man/man5/pmix_server_module_t.5.rst)
  man page.
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
