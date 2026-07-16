# AGENTS.md: The PMIx Event Subsystem

This document orients AI agents and human contributors working in
`src/event`, the core of the PMIx event-notification system. It assumes
you have already read the top-level [`AGENTS.md`](../../AGENTS.md) — the
golden rules (prefix conventions, `pmix_config.h`-first include order,
constant-on-the-left comparisons, brace-everything, warning-free under
`--enable-devel-check`), the **thread-safety / progress-thread model and
the caddy pattern**, the backward-compatibility and wire-format rules,
and the copyright-header requirement all apply here and are not
repeated. This file covers what is specific to `src/event`: the handler
taxonomy and precedence model, the event-chain state machine, the
registration and notification control flows, the two caching mechanisms,
and the invariants that are easy to break.

Like `src/client`, **`src/event` is not an MCA framework.** There is no
component structure — just two `.c` files and one header compiled
straight into `libpmix` via `Makefile.include` (see
[Building](#building)). The code is *role-shared*: the same functions
run inside clients, servers, tools, and launchers, with behavior
branched on `PMIX_PEER_IS_SERVER` / `PMIX_PEER_IS_TOOL` /
`PMIX_PEER_IS_LAUNCHER`. Expect every path you touch to be exercised in
all roles.

## What this directory is

PMIx events are asynchronous, status-code-tagged notifications
(process failures, job state changes, group operations, system events)
that flow between applications, PMIx servers, and the host RM. This
directory owns:

- **Handler registration** (`pmix_event_registration.c`) —
  `PMIx_Register_event_handler` / `PMIx_Deregister_event_handler`, the
  ordering/placement logic, and the bookkeeping that tells the server
  (or host RM) which status codes this process wants to hear about.
- **Notification generation and delivery** (`pmix_event_notification.c`)
  — `PMIx_Notify_event`, the server-side fan-out to registered clients
  (`pmix_server_notify_client_of_event`), the client-side relay to the
  server (`pmix_notify_server_of_event`), and the local
  **event chain** that walks registered handlers in precedence order.
- **The shared structures** (`pmix_event.h`) — `pmix_event_hdlr_t`,
  `pmix_events_t`, `pmix_event_chain_t`, `pmix_rshift_caddy_t`,
  `pmix_active_code_t`, and the `PMIX_REPORT_EVENT` macro.

The state it operates on lives in `pmix_globals`
(`src/include/pmix_globals.h`), not here: `pmix_globals.events` (the
`pmix_events_t` registration lists), `pmix_globals.notifications` (the
cached-notification hotel) with `max_events` / `event_eviction_time`,
and `pmix_globals.cached_events` + `event_window` (the aggregation
timer used by `PMIX_REPORT_EVENT`). Related code you will often touch
in the same change: the server-side registration store
(`pmix_server_globals.events`, filled in `src/server/pmix_server_ops.c`
when clients send `PMIX_REGEVENTS_CMD`), and the notification receive
paths in `src/client/pmix_client.c` (`pmix_client_notify_recv`) and
`src/tool/pmix_tool.c` (`pmix_tool_notify_recv`), which construct
chains from incoming messages and feed them to
`pmix_invoke_local_event_hdlr`.

## Handler taxonomy and precedence

Registered handlers are partitioned by *what codes they subscribe to*,
and each partition is a separate list in `pmix_globals.events`:

| Bucket | Field | Meaning |
|--------|-------|---------|
| first overall | `events.first` | single handler invoked before everything (registered with `PMIX_EVENT_HDLR_FIRST`) |
| single-code | `events.single_events` | handlers registered for exactly one status code |
| multi-code | `events.multi_events` | handlers registered for an array of codes |
| default | `events.default_events` | handlers registered with no codes — see every event |
| last overall | `events.last` | single handler invoked after everything (`PMIX_EVENT_HDLR_LAST`) |

An incoming event walks these in the order shown: first → single →
multi → default → last. Within a list, position is controlled at
registration time by `PMIX_EVENT_HDLR_PREPEND`/`APPEND`,
`FIRST_IN_CATEGORY`/`LAST_IN_CATEGORY`, or `BEFORE`/`AFTER` a *named*
handler; the resolved position is recorded in `pmix_event_hdlr_t.precedence`
as a `PMIX_EVENT_ORDER_*` value. Default handlers are skipped when the
event was marked `PMIX_EVENT_NON_DEFAULT` (tracked as
`chain->nondefault`).

Three per-handler filters gate invocation, and all three must pass:

1. **code match** — the chain's status is in the handler's `codes`
   array (vacuously true for default handlers);
2. **source range** (`pmix_notify_check_range`) — the handler's
   registration-time `rng` (a `pmix_range_trkr_t`: range constant plus
   optional proc array from `PMIX_EVENT_CUSTOM_RANGE`) admits the
   event's *source* proc;
3. **affected overlap** (`pmix_notify_check_affected`) — the handler's
   `PMIX_EVENT_AFFECTED_PROC(S)` interest list intersects the event's
   affected-procs list (vacuously true if either side is absent).

Keep the direction of each straight: **`rng`/targets are about who
receives, `affected` is about whom the event concerns, and the range
check filters on the event's *source*.**

`pmix_events_t.nhdlrs` is a monotonically increasing counter used to
mint the `index` (the "event handler reference") returned to callers
and used for deregistration. Indices are never reused; deregistration
does not decrement the counter. The `actives` list
(`pmix_active_code_t`) reference-counts, per status code, how many
local registrations exist so the process only tells its server about
the *first* registration for a code and only sends a deregistration
when the *last* one goes away.

## The event chain state machine

Local delivery is asynchronous and re-entrant: each handler is invoked
with a completion callback, and the walk resumes only when the handler
calls it. The walk's entire state lives in one heap object, the
`pmix_event_chain_t`.

```
pmix_invoke_local_event_hdlr(chain)          [progress thread]
    → picks the first matching handler, invokes it with
      progress_local_event_hdlr as the completion cbfunc
handler runs (possibly in the host's thread), eventually calls
progress_local_event_hdlr(status, results, ...)
    → stores interim results on the chain, PMIX_THREADSHIFT → cycle_events
cycle_events                                  [progress thread]
    → aggregates results (RFC0018), resumes the list walk from
      chain->evhdlr, invokes the next match … repeat …
    → when exhausted (or PMIX_EVENT_ACTION_COMPLETE, or endchain, or
      the "last" handler ran): fire chain->final_cbfunc, else RELEASE
```

Invariants that keep this machine alive:

- **The two reserved info slots.** Every chain's `info` array is
  allocated with `nallocated = ninfo + 2`; the two trailing slots are
  (re)loaded before *each* handler invocation with
  `PMIX_EVENT_HDLR_NAME` and `PMIX_EVENT_RETURN_OBJECT`, and `ninfo` is
  reset to `nallocated - 2` between handlers. Every code path that
  constructs a chain — including the receive paths in
  `src/client/pmix_client.c` and `src/tool/pmix_tool.c` — must honor
  this. `pmix_prep_event_chain()` assumes the caller already created
  `info`/`nallocated`; the chain destructor frees `info` with
  `nallocated`, not `ninfo`.
- **Result aggregation.** `cycle_events` rebuilds `chain->results` on
  every step: prior results whose key was not NULL'd by the handler,
  plus one `PMIX_STATUS` entry recording the just-completed handler's
  returned status (keyed by handler name), plus the handler's interim
  results. Handlers see the accumulated log of everything before them.
- **Termination.** A handler returning `PMIX_EVENT_ACTION_COMPLETE`
  ends the chain; `chain->endchain` forces single-shot processing (set
  when replaying cached events and when the "last" handler is entered);
  a `oneshot` handler is deregistered after it completes.
- **Completion contract.** If `final_cbfunc` is set, it owns the chain
  (every setter passes the chain itself as `final_cbdata` and releases
  it there); if not, the chain is released when the walk ends. Set one
  or the other, never both semantics.
- **`pmix_invoke_local_event_hdlr` tolerates `chain->info == NULL`**
  (it short-circuits to `complete`), but nothing downstream does —
  don't invoke handlers on a chain without the reserved slots.

Two side effects piggyback on delivery in
`pmix_invoke_local_event_hdlr`: a `PMIX_GROUP_CONSTRUCT_COMPLETE` event
adds the group (id, sorted membership, context id) to
`pmix_client_globals.groups`, and `PMIX_GROUP_LEFT` removes the
departing member — keep these in sync with the group code in
`src/client/pmix_client_group.c` and `src/common/pmix_pgroup.c`.

## Registration flow

`PMIx_Register_event_handler` thread-shifts a `pmix_rshift_caddy_t`
("rshift" = registration-shift; note this directory also uses the
generic `pmix_shift_caddy_t` for other paths) to
`pmix_internal_reg_event_hdlr`, which:

1. parses the directive infos (ordering, name, range, custom range,
   affected procs, oneshot, return object) — everything it does not
   consume is transferred to a side list and forwarded to the server;
2. flags `enviro` if any code is a system event
   (`PMIX_SYSTEM_EVENT()` range — these need host-RM involvement);
3. creates the `pmix_event_hdlr_t`, assigns its index, and inserts it
   into the right list at the right position (this is where the
   BEFORE/AFTER locator search lives);
4. calls `_add_hdlr`, which updates the `actives` refcounts and decides
   whether the registration must leave the process:
   - **client/tool/launcher, connected, server ≥ v2**: pack a
     `PMIX_REGEVENTS_CMD` and `PMIX_PTL_SEND_RECV` it
     (`_send_to_server`); `_add_hdlr` returns `PMIX_ERR_WOULD_BLOCK`
     and the caller's callback fires later from `regevents_cbfunc`;
   - **server with `enviro` codes and a host `register_events`**:
     up-call the host; the ack is supposed to come back through
     `reg_cbfunc` → (threadshift) → `_regcbfunc`;
   - otherwise: purely local, ack immediately.
5. on the ack path (`ack:`), replays any matching cached notifications
   (`check_cached_events`) *before* invoking the registration callback,
   so late registrants still see recently generated events.

`PMIX_RANGE_PROC_LOCAL` registrations never leave the process at all.

Failure handling is "undo": if the server/host rejects the
registration, the callback path removes the handler from its list (or
clears `events.first`/`events.last`) and reports
`PMIX_ERR_SERVER_FAILED_REQUEST` with index `UINT_MAX`. When you touch
these paths, walk both the immediate-failure and deferred-failure
branches — the caddy juggling (`rb` wraps `cd`, `rb->cd` retains it,
the `rsdes` destructor releases it) is the most error-prone spot in the
file.

Deregistration (`pmix_deregister_event_hdlr`) mirrors this: find the
handler by index in first/last/one-of-three-lists, decrement the
`actives` refcounts, and pack into the caller-supplied buffer the codes
whose last local registration just disappeared (the caller then sends
that buffer as a `PMIX_DEREGEVENTS_CMD`). A default-handler
deregistration packs the `PMIX_MAX_ERR_CONSTANT` wildcard instead.

## Notification flow

`PMIx_Notify_event` thread-shifts to `pmix_internal_notify_event`, then
splits by role:

**Server role** → `pmix_server_notify_client_of_event` (also the entry
point the host RM calls, and re-entered by tools relaying downward).
This *copies* the info array into a `pmix_notify_caddy_t` (an explicit,
justified exception to the no-copy caddy rule: the event outlives the
call because it may be cached) and thread-shifts to
`_notify_client_event`, which:

1. caches the notification in the hotel (unless
   `PMIX_EVENT_DO_NOT_CACHE`) so procs that register later can be
   handed it by `check_cached_events`;
2. builds a chain for the server's *own* handlers via
   `pmix_prep_event_chain`;
3. unless the range is `PMIX_RANGE_RM` or `PMIX_RANGE_PROC_LOCAL`,
   walks `pmix_server_globals.events` and sends a `PMIX_NOTIFY_CMD`
   message to every registered peer that passes the
   source/target/affected filters — deduplicated via a `pmix_namelist_t`
   tracker, never echoing to the event's source or to itself. With
   custom targets it counts down `nleft` and evicts the cached entry
   once every target has been notified;
4. if this server is itself the event's source (and the event is not
   `PMIX_RANGE_LOCAL` / `PMIX_EVENT_STAYS_LOCAL`), up-calls
   `pmix_host_server.notify_event` so the host can broadcast beyond
   this node — the caddy is held until the host's callback;
5. runs the local chain.

**Client/tool role** → `pmix_notify_server_of_event`: pack
status/range/info as a `PMIX_NOTIFY_CMD` to the server (the server
does *not* echo it back; note the source is deliberately **not**
packed — the server identifies the sender from the connection, one of
the wire-format details you must not "fix"), cache it locally if
`PMIX_RANGE_PROC_LOCAL`, and run the local chain (`dolocal=true`; tools
relaying a received event pass `dolocal=false` to avoid double
processing). Lost-connection events and self-served peers
(`mypeer == myserver`) skip the send.

The wire format of both message families (`PMIX_REGEVENTS_CMD`,
`PMIX_DEREGEVENTS_CMD`, `PMIX_NOTIFY_CMD`) is ABI across versions —
append-only, per the top-level Version Interoperability rules. The
receive side must tolerate `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER`
for fields newer than the peer (see the range unpack in
`pmix_tool_notify_recv`). Also note the v1 exception: registrations are
never sent to a v1 server (`PMIX_PEER_IS_V1`) because v1's event system
did not interoperate.

## The two caches

Do not confuse them:

- **The notification hotel** (`pmix_globals.notifications`, a
  `pmix_hotel_t` of `pmix_notify_caddy_t`, capacity `max_events`,
  eviction after `event_eviction_time`). Purpose: hold *delivered or
  generated* events so a proc that registers *afterwards* still
  receives them (`check_cached_events` replays matches on every new
  registration, with `endchain` set). `pmix_notify_event_cache` evicts
  the oldest occupant when full. Entries are checked out (and
  released) when replayed, when all custom targets have been notified,
  or on eviction.
- **The `PMIX_REPORT_EVENT` aggregation window**
  (`pmix_globals.cached_events` + the `event_window` timer +
  `pmix_event_timeout_cb`). Purpose: coalesce *internally generated*
  events — in practice `PMIX_ERR_LOST_CONNECTION` / `PMIX_ERROR` raised
  by the PTL when peers die (see `src/mca/ptl/base/ptl_base_sendrecv.c`,
  the only users of the macro) — so a burst of failures within the
  window becomes one chain whose info array is *prepended* with one
  `PMIX_PROCID` per additional source (the two reserved trailing slots
  must stay at the end; that is why the macro prepends). When the timer
  fires, the chain is pushed through the server fan-out (server role)
  or the local chain (everyone else).

## Threading and ownership rules

- Everything here runs on the progress thread. The public entry points
  thread-shift immediately; `progress_local_event_hdlr` exists because
  handler completion callbacks may arrive on the *host's* thread and
  must be shifted back before touching the chain or the lists. Never
  walk `pmix_globals.events` or the hotel off the progress thread.
- Blocking-API emulation follows the standard caddy pattern
  (`mycbfn`/`myopcb` + `PMIX_WAIT_THREAD`); the blocking
  `PMIx_Register_event_handler` returns the handler index *as* the
  status on success — preserve that quirk.
- `pmix_notify_caddy_t` copies its info; `pmix_event_chain_t` copies
  (XFERs) into its own array; the registration caddies do **not** copy
  the caller's info (standard no-copy rule) but *do* copy the codes
  array, precisely so `check_cached_events` can run after the caller's
  callback has already freed its storage.
- Refcount hygiene: the hotel holds its own retain on a cached caddy;
  `_notify_client_event` holds another while the host up-call is
  pending (`holdcd`); `rb->cd` retains the registration caddy until the
  ack destructs it. When adding an early return, recount who holds
  what on that path — most historical bugs here are a missed release or
  a missed completion callback on an error branch.
- Every path that accepts user info arrays must keep the
  targets/affected distinction intact and must preserve the reserved
  two info slots (see the chain invariants above).

## Building

`src/event` compiles straight into `libpmix` via `Makefile.include`
(which appends to the top-level `sources`/`headers` lists — there is no
`Makefile.am` here). Nothing is conditionally compiled, so a change
takes effect with a plain top-level `make` from an already-configured
tree; you need `autogen.pl`/`configure` only if you add or remove a
source file. After a build, smoke-test with `make check` in `test/` and
`./simptest` in `test/simple/` — the event paths are exercised by the
`test/simple` programs (e.g. `simpdyn`, the debugger/tool tests) and by
any spawn/group test. Do not diagnose functional failures against an
`--enable-test-build` tree (see the top-level guide).

## When modifying code here

- **Trace the chain resume logic before touching `cycle_events`.** The
  resume point after each handler (which list, which element, the
  first-overall special cases) is the most delicate code in this
  directory; a wrong `item` seed silently skips whole handler
  categories rather than crashing.
- **Walk every early-return path** in the notification/registration
  flows and confirm three things on each: the caddy/chain is released
  exactly once, the caller's completion callback fires exactly once,
  and any packed buffer is released if the send never happened.
  Dropped completions hang blocking callers; doubled ones corrupt
  caller state.
- **Match the surrounding pattern exactly** — pick the nearest sibling
  branch (client vs. server vs. tool) that already does what you need
  and mirror its ownership discipline.
- **Prefer a new attribute over a new API** (top-level "Role of
  Attributes") — event behavior is extended by adding
  `PMIX_EVENT_*` directives parsed in
  `pmix_internal_reg_event_hdlr`/`pmix_prep_event_chain`, not by new
  entry points. Document new attributes in
  `include/pmix_common.h.in` and keep both parsers in agreement.
- **Respect the wire format** of the three command messages: append
  only, tolerate short buffers from older peers, never reorder.
