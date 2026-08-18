<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSTAT Framework

This document orients AI agents and human contributors working in the
`pstat` (**P**rocess **Stat**istics) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety model, and MCA concepts
described there all apply here and are not repeated. This file covers what
is specific to `pstat`: what the framework is for, how a monitoring
request flows through it, the shared data structures and base helpers
every component leans on, and the contract a component must honor. Each
component subdirectory (`plinux/`, `pmacos/`, `test/`) carries its own
`AGENTS.md` with component-specific detail.

For a wider, API-oriented tour of how this framework implements the
`PMIx_Process_monitor` family of calls, see
[`docs/how-things-work/pstat.rst`](../../../docs/how-things-work/pstat.rst).

## What PSTAT does

`pstat` is the back end that samples **resource-utilization statistics**
from the local operating system and returns them as `pmix_info_t` data.
It answers four broad questions about the local node:

- **Per-process** usage — CPU time, %CPU, priority, thread count, virtual
  size, RSS, PSS, process state, etc. (for processes the local PMIx
  server is hosting).
- **Node** usage — load averages and the various memory/swap totals.
- **Disk** usage — read/write counters from the block devices.
- **Network** usage — per-interface byte/packet/error counters.

It is the engine underneath the `PMIx_Process_monitor` /
`PMIx_Process_monitor_nb` public APIs (see
[`src/common/pmix_monitor.c`](../../common/pmix_monitor.c)). It only ever
runs **inside a PMIx server** — clients and tools relay their monitor
requests to their server, which is the process that actually reads the
OS. The framework was originally written to feed the old `ompi-top`
utility (see the header comment in [`pstat.h`](pstat.h)); today it services
any caller of the monitor API.

`pstat` is a **single-select** framework: exactly one component is active
per process. On a Linux node with a readable `/proc`, that is `plinux`
(priority 80); on macOS (Darwin) it is `pmacos` (priority 80), which reads
the same categories through the mach/libproc/sysctl/IOKit interfaces.
Only one of these ever builds on a given host, so their equal priorities
never collide. The `test` component (priority 20) returns deterministic
canned values and exists so the monitor code path can be exercised on
platforms without a native reader and in CI. If no component can run, the base
installs an "unsupported" module whose every entry point returns
`PMIX_ERR_NOT_SUPPORTED` — so the monitor API degrades cleanly rather
than crashing.

## Directory layout

```
src/mca/pstat/
├── pstat.h                 Framework public API: module & component types
├── base/                   Framework infrastructure
│   ├── base.h              Internal base API: globals, op object, stat-spec
│   │                       structs, the parse helpers, and the op macros
│   ├── pstat_base_frame.c  open/close/register, framework decl, op class
│   ├── pstat_base_select.c single-component selection + init
│   └── pstat_base_fns.c    the four "parse the request" helpers
├── plinux/                 Linux /proc reader (the real component)
├── pmacos/                 macOS reader (mach/libproc/sysctl/IOKit)
└── test/                   canned-data component for CI / unsupported hosts
```

## Core data structures (`pstat.h`)

`pstat.h` is the framework's public header — the contract every component
compiles against. It is deliberately tiny:

- **`pmix_pstat_base_module_t`** — the runtime instance. Three function
  pointers:
  - `init` (`pmix_pstat_base_module_init_fn_t`) — one-time setup; returns
    `PMIX_SUCCESS`.
  - `query` (`pmix_pstat_base_module_query_fn_t`) — **the workhorse**.
    This is where all the OS reading happens. Signature:
    ```c
    pmix_status_t (*query)(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t error,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults);
    ```
  - `finalize` (`pmix_pstat_base_module_fini_fn_t`) — teardown.

- **`pmix_pstat_base_component_t`** — just a typedef of the standard
  `pmix_mca_base_component_t`. `pstat` components carry no extra
  configuration state, so neither component wraps it in a larger struct;
  each provides only a `pmix_mca_query_component` that hands back its
  module and a priority.

- The three **`PMIX_MCA_pstat_*_VERSION`** macros — the framework's
  interface version, which `PMIX_MCA_BASE_VERSION(pstat)` stamps into
  every component struct.

- The global **`pmix_pstat`** — the selected module, the single symbol
  the rest of the library calls through (`pmix_pstat.query(...)`).

### The `query` contract

`query` is **synchronous and runs on the progress thread** — it does not
take a callback. It returns a freshly allocated `pmix_info_t` array
through `*results` / `*nresults`; ownership passes to the caller
(`pmix_monitor_processing` in `pmix_monitor.c`). The caller has already
determined that the request involves the local node before calling
`query`, so a component may assume "this is for us."

The `monitor` argument's **key** names *what kind* of statistics are
wanted; its **value** (a `PMIX_DATA_ARRAY` of `pmix_info_t`) names the
specific fields. The recognized monitor keys are:

| Monitor key | Meaning |
|-------------|---------|
| `PMIX_MONITOR_PROC_RESOURCE_USAGE` | per-process stats |
| `PMIX_MONITOR_NODE_RESOURCE_USAGE` | node load/memory (may nest net + disk) |
| `PMIX_MONITOR_DISK_RESOURCE_USAGE` | disk counters |
| `PMIX_MONITOR_NET_RESOURCE_USAGE`  | network counters |
| `PMIX_MONITOR_CANCEL` | stop a previously-started periodic monitor |

The `directives` array qualifies the request. The keys `pstat` components
act on directly:

| Directive | Effect |
|-----------|--------|
| `PMIX_MONITOR_ID` | caller-supplied string handle for this monitor, needed later to cancel it |
| `PMIX_MONITOR_RESOURCE_RATE` | sample **periodically** every N seconds (uint32) rather than once |
| `PMIX_MONITOR_TARGET_PROCS` | array of `pmix_proc_t` restricting which processes to sample |
| `PMIX_MONITOR_TARGET_PIDS` | array of `pmix_node_pid_t` (node+pid) restricting the sample |

(Other monitor directives — `PMIX_MONITOR_LOCAL_ONLY`,
`PMIX_MONITOR_TARGET_NODES`, `PMIX_MONITOR_PROXY`, `PMIX_SEND_HEARTBEAT`,
heartbeat handling — are interpreted *above* the framework in
`pmix_monitor.c` and never reach `query`.)

## How a monitoring request flows

1. **Public API.** A process calls `PMIx_Process_monitor` (blocking) or
   `PMIx_Process_monitor_nb` (in
   [`src/common/pmix_monitor.c`](../../common/pmix_monitor.c)). A client
   or tool packs the request and sends it to its server
   (`PMIX_MONITOR_CMD`); a server handles it locally. Either way it lands
   in `pmix_monitor_processing()` on the server's progress thread (the
   server-side unpack is `pmix_server_monitor()` in
   `src/server/pmix_server_control.c`).

2. **Scope resolution.** `pmix_monitor_processing()` inspects the target
   directives to decide whether the request involves **local** processes,
   **remote** nodes, or both. Remote participation is handed to the host
   RM via `pmix_host_server.monitor`; there is nothing `pstat` can do off
   the local node.

3. **Local sampling.** For any local participation, the monitor code calls
   `pmix_pstat.query(...)` — i.e. the selected component's `query`. This
   is the boundary into the framework.

4. **Result delivery.** `query` returns an info array; the monitor code
   either returns it to the caller (one-shot) or — for a periodic monitor
   — the component keeps an *op* alive that re-samples on a timer and
   pushes each update out as a PMIx **event** (`PMIx_Notify_event`)
   targeted at the requestor.

That last point is the framework's most important structural idea, so it
gets its own section.

## One-shot vs. periodic monitoring: the `pmix_pstat_op_t` object

A single request can be either a **one-shot** query (return the numbers
now) or a **periodic** monitor (`PMIX_MONITOR_RESOURCE_RATE` given: keep
sampling every N seconds and deliver each sample as an event until
cancelled). Both paths are driven by the same per-request state object,
`pmix_pstat_op_t`, defined in [`base/base.h`](base/base.h):

| Field | Purpose |
|-------|---------|
| `super` | `pmix_list_item_t` — lets a periodic op live on `pmix_pstat_base.ops` |
| `requestor` | the `pmix_proc_t` to target event notifications at |
| `id` | the caller's `PMIX_MONITOR_ID` string — the cancel handle |
| `ev` / `tv` | libevent timer + interval for periodic re-sampling |
| `active` | true once the periodic timer is armed |
| `rate` | seconds between samples (0 ⇒ one-shot) |
| `eventcode` | the status code to raise on each periodic update |
| `peers` | `pmix_peerlist_t` list of the local processes to sample |
| `disks` / `nets` | argv of specific disk / network IDs to report (NULL ⇒ all) |
| `pstats` / `dkstats` / `netstats` / `ndstats` | which individual fields to collect |
| `cb` | non-NULL during a **synchronous** collection pass (see below) |

The op's constructor/destructor live in `pstat_base_frame.c`
(`PMIX_CLASS_INSTANCE(pmix_pstat_op_t, ...)`); the destructor deletes an
armed timer and frees the id/disks/nets/peers, so releasing an op is the
clean way to tear a monitor down.

### The `op->cb` duality (read this before touching a component)

Each component has an `update()` timer handler that does the actual
collection. It is invoked in two very different modes, distinguished
solely by whether `op->cb` is set:

- **`op->cb != NULL` — synchronous collection.** `query` sets `op->cb` to
  point at a stack `pmix_cb_t` whose `cbdata` holds an *info-list builder
  handle* (`PMIx_Info_list_start()`), then calls `update()` directly (not
  via a timer). `update()` appends its results into that existing list and
  returns; the `noreset` flag is set so it does **not** re-arm a timer.
  `query` then converts the list to the `*results` array. This is how the
  immediate/one-shot answer is produced — and also how the *first* sample
  of a periodic monitor is produced.
- **`op->cb == NULL` — periodic timer fire.** When the libevent timer
  later fires, `op->cb` is NULL. `update()` builds its own fresh
  info-list, converts it, and delivers it asynchronously via
  `PMIx_Notify_event(op->eventcode, ...)` scoped to `op->requestor`; then
  it re-arms the timer (`!noreset`).

So `query` always calls `update()` once synchronously to get the initial
numbers, and *additionally* — if a rate was given — appends the op to
`pmix_pstat_base.ops` and arms the timer with `PMIX_PSTAT_OP_START` so the
same `update()` will fire again periodically in event mode. A one-shot op
is `PMIX_RELEASE`d immediately after the synchronous pass.

### Cancellation

A `PMIX_MONITOR_CANCEL` request carries the `PMIX_MONITOR_ID` string in
its value. `query` walks `pmix_pstat_base.ops`, matches the id, removes
the op from the list and `PMIX_RELEASE`s it (which deletes the timer).
Cancelling an unknown id is deliberately **not** an error.

## The shared stat-spec structs and parse helpers (`base/`)

To keep components from re-parsing the request, the base defines four
small all-`bool` "which fields do you want" structs and one
argv-collector per category, plus a helper that fills each from the
caller's `pmix_info_t` array. These are the reusable heart of the
framework.

### The four spec structs (`base.h`)

- **`pmix_procstats_t`** — per-process flags: `cmdline`, `pctcpu`,
  `state`, `time`, `pri`, `nthreads`, `cpu`, `vsize`, `pkvsize`, `rss`,
  `pss`.
- **`pmix_dkstats_t`** — disk flags: read/write completed/merged/sectors/
  millisec, plus `ioprog`, `ioms`, `ioweight`.
- **`pmix_netstats_t`** — network flags: received/sent bytes/packets/errors.
- **`pmix_ndstats_t`** — node flags: load averages (`la`, `la5`, `la15`)
  and memory/swap totals.

Each has `PMIX_*_INIT(a)` (zero it — collect nothing) and `PMIX_*_ALL(a)`
(set all — collect everything) convenience macros built on `memset`.
Because these are plain `bool` structs, a component tests "did the caller
ask for *anything* in this category?" cheaply with a single `memcmp`
against a zeroed instance — that idiom appears throughout `update()`.

**Keep these structs all-`bool`, and keep them flat.** Three separate
things depend on it and none of them will complain if you break it:
`PMIX_*_ALL` is a `memset` to `1`, which only produces a valid `true` in
every member if every member is a one-byte `bool`; the `memcmp`-against-
zero idiom only gives the right answer if the struct has no padding for
the memcmp to trip over; and neither would fail loudly — you would just
get fields silently collected, or silently not. Adding an `int` or a
pointer member breaks all three at once. If a category ever needs a
non-boolean qualifier, put it beside the spec struct rather than in it.

### The four parse helpers (`pstat_base_fns.c`)

These translate the caller's requested-fields info array into the spec
structs. **Key convention:** if the incoming `info` pointer is `NULL`
they select *everything* (`PMIX_*_ALL`); otherwise they start from
"nothing" and flip on exactly the fields whose attribute keys appear.

| Helper | Fills | Also collects |
|--------|-------|---------------|
| `pmix_pstat_parse_procstats` | `pmix_procstats_t` | — |
| `pmix_pstat_parse_dkstats` | `pmix_dkstats_t` | `PMIX_DISK_ID` values into a `disks` argv |
| `pmix_pstat_parse_netstats` | `pmix_netstats_t` | `PMIX_NETWORK_ID` values into a `nets` argv |
| `pmix_pstat_parse_ndstats` | `pmix_ndstats_t` | — |

The disk/net helpers do double duty: an ID key (`PMIX_DISK_ID` /
`PMIX_NETWORK_ID`) names a *specific device to report*, while every other
key turns on a *field to report for the chosen devices*. An empty
`disks`/`nets` argv (`NULL`) means "all devices." The ID list is a **set**
— it is built with `PMIx_Argv_append_unique_nosize`, so a caller that
names the same device twice gets it reported once, the same way
`PMIX_PSTAT_APPEND_PEER_UNIQUE` dedups the op's peer filter.

Note the asymmetry between the two "nothing was specified" cases, which
is deliberate but easy to get backwards: a **NULL** `info` pointer means
*collect everything*, while a **non-NULL but zero-length** array means
*collect nothing* (the loop simply never flips a flag on). A component
that hands these helpers `darray->array` / `darray->size` therefore turns
an empty data array into an empty result, not a full one.

Field-key matching uses `PMIx_Check_key`. Despite the name, that is not
an alias-aware lookup — it is a `strncmp` bounded at `PMIX_MAX_KEYLEN`
that returns false if either side is `NULL`. So a field is recognized
only under its canonical `PMIX_*` string; there is no registered-alias
resolution anywhere on this path.

**The info array is untrusted.** For a request that came from a client it
is the data array unpacked off the wire, and nothing between the unpack
and these helpers validates it. `PMIx_Check_key` only compares the key —
it says nothing about `value.type`. Any helper that *reads* a value (as
the disk/net helpers do for the ID keys) must check the declared type
first: reading `value.data.string` out of a value the sender typed as an
integer hands a wild pointer to `strdup`. The ID collection in
`pstat_base_fns.c` does this check; a component that reads a value out of
this array directly must do the same.

### The op macros (`base.h`)

- **`PMIX_PSTAT_OP_START(p, s, cb)`** — arm the periodic timer: bind
  `cb` (the `update` handler) to `p->ev` on the framework event base, set
  the interval to `s` seconds, mark the op active, and add the timer. Note
  the `PMIX_POST_OBJECT(p)` memory barrier before the op is handed to the
  event engine.
- **`PMIX_PSTAT_APPEND_PEER_UNIQUE(pl, pr)`** — append a `pmix_peer_t` to
  an op's `peers` list only if it is not already present (dedup), wrapping
  it in a `pmix_peerlist_t`.

## Base infrastructure in detail

### Globals and lifecycle (`pstat_base_frame.c`)

- **`pmix_pstat_base`** (`pmix_pstat_base_t`): holds the framework event
  base (`evbase`) and the `ops` list of live periodic monitors.
- **`pmix_pstat`** — the active module. Initialized to the three
  `unsupported` stubs; `pmix_pstat_base_select()` overwrites it with the
  winning component's module (or leaves the stubs in place if none runs).
- **`pmix_pstat_base_component`** — back-pointer to the selected component.

**Open (`pmix_pstat_base_open`)** decides where periodic timers run.
The framework-level MCA parameter **`pstat_base_use_separate_thread`**
(bool, default false) controls this:

- **false (default):** `evbase` is aliased to `pmix_globals.evbase` — the
  library's main progress thread. Periodic sampling shares that thread.
- **true:** a dedicated progress thread named `"PSTAT"` is spun up
  (`pmix_progress_thread_init`) so sampling cannot perturb the main
  thread. `close` takes it back down.

Then it constructs `pmix_pstat_base.ops` and opens all components. If
opening the components fails, `open` undoes all of that itself: when a
framework's open fails the base never sets the OPEN flag, and so never
calls the framework's close function.

**Close (`pmix_pstat_base_close`)** is ordered the way it is for two
reasons that pull against each other, and getting either wrong is a
memory-safety bug rather than a leak. The sequence is **pause → destruct
ops → finalize → stop**:

- The ops must not be destructed while the sampling thread is running.
  Each live op owns a timer armed on the framework's base, and
  `pmix_event_del` does **not** wait for a callback that is already
  executing — so a destruct racing a timer fire frees the op out from
  under the `update()` running on it. `pmix_progress_thread_pause()`
  joins the thread, so nothing is in flight once it returns.
- The base must still exist while the ops are destructed, because
  `opdes` deletes each op's timer and `pmix_event_del` reads the base out
  of the event.

That is why the teardown is a `pause` before and a `stop` after, rather
than one call. **`pmix_progress_thread_stop()` frees the event base** —
it drops the last reference to the progress tracker, whose destructor
calls `pmix_event_base_free`. Do not call `pmix_event_base_free` on it as
well; that was a double free. `pmix_psensor_base_close` is the correct
sibling to compare against.

In the **default** configuration none of this applies: `evbase` is the
library's shared base, and `PMIx_server_finalize` calls
`PMIx_Progress_thread_stop(NULL, 0)` near the top — well before it closes
any framework — so the shared thread is already quiesced by the time
this runs, and the base belongs to the runtime rather than to `pstat`.

Close also puts the `unsupported` stubs back into `pmix_pstat` and clears
`pmix_pstat_base_component`. The module it was holding belongs to a
component `components_close` is about to release, which under
`--enable-mca-dso` means an unloaded plugin; `select` only overwrites
`pmix_pstat` when it *finds* a component, so a second init cycle that
found none would otherwise still be pointing into that plugin.

`test/unit/pstat_frame` drives this whole path with the parameter turned
on, since nothing else in the suite does.

**Register (`pmix_pstat_register`)** registers the single
`pstat_base_use_separate_thread` MCA parameter.

The framework itself is declared with
`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pstat, "process statistics", ...)`.
It is opened and selected during **server** startup in
`src/server/pmix_server.c` (search for `pmix_pstat_base_framework`), *after*
`pgpu` and before the listener starts.

### Selection (`pstat_base_select.c`)

`pmix_pstat_base_select()` is a textbook single-select: call
`pmix_mca_base_select("pstat", ...)` to pick the highest-priority runnable
component, call its `init`, and — only if that succeeds — cache it in
`pmix_pstat`/`pmix_pstat_base_component`. The order matters: installing
the module first would leave a module whose `init` failed sitting there
as the active one, and the framework would keep dispatching queries to it
rather than falling back. Nothing is lost by initializing first, since no
component's `init` reads these globals.

Two failure modes, and they are **not** treated alike:

- **No component is runnable** — benign. `select` returns `PMIX_SUCCESS`
  with the `unsupported` stubs still in place, so `pmix_pstat.query`
  returns `PMIX_ERR_NOT_SUPPORTED` and the monitor API reports the
  capability as absent.
- **A component was found but its `init` failed** — `select` returns that
  error, and `pmix_server.c` propagates it, so **`PMIx_server_init`
  fails and the server does not come up**. That is a heavier consequence
  than the framework's own "degrade to unsupported" design suggests, and
  it is worth deciding deliberately rather than inheriting it: the
  alternative is to log and fall back to the stubs, as the no-component
  case does. All three current components' `init` is an unconditional
  `return PMIX_SUCCESS`, so nothing reaches it today.

## Threading

Everything in `pstat` runs on a PMIx progress thread — either the main one
or the dedicated `"PSTAT"` thread, per the MCA parameter above. `query` is
reached only through the thread-shift already performed by the monitor API
(`PMIX_THREADSHIFT(cb, pmix_monitor_processing)`), so components may touch
`pmix_server_globals.clients` and other shared server state directly.
Periodic `update()` fires from the same event base. Do **not** invent a
new path that calls `pmix_pstat.query` from an arbitrary user thread
without thread-shifting first.

Note that the modules build their result arrays with the public
`PMIx_Info_list_*` builder helpers and raise periodic updates with the
public `PMIx_Notify_event` — these are the info-list/event utilities the
rest of the library also uses.

## Building

The framework core (`base/`) is always built into `libpmix`. Each
component builds as a standard MCA component (DSO
`pmix_mca_pstat_<name>.la` or static `libpmix_mca_pstat_<name>.la`). Both
real components ship a `configure.m4`: `plinux` enables **only** on Linux
with a readable `/proc/cpuinfo` and a defined `HZ` (from `<sys/param.h>`),
and `pmacos` enables **only** on Apple/Darwin (`oac_found_apple`), where
it also pulls in the IOKit and CoreFoundation frameworks for its per-disk
statistics. On a host matching neither, only `test` remains and the
framework falls back to it (or to `unsupported`). Adding a component means
creating `src/mca/pstat/<name>/` with the usual `Makefile.am`, a component
struct, and a module; the framework picks it up through the generated
`base/static-components.h` — no core code changes are needed.

Remember the top-level golden rules that bite here:

- Editing a `Makefile.am` only needs a plain `make`; adding/removing a
  *component directory* or touching `configure.m4` changes the build
  wiring resolved by `configure`, so re-run
  `./autogen.pl && ./configure ... && make`.
- The framework is only meaningfully exercised inside a server. To test a
  real change, drive it through `PMIx_Process_monitor` (see the `test`
  component's `AGENTS.md` for how the canned component makes this possible
  without `/proc`).

## When adding or modifying a component

- Open the component struct with `PMIX_MCA_BASE_VERSION(pstat)` and set
  `pmix_mca_component_name` to your component's directory name.
- Provide a `pmix_mca_query_component` that hands back your
  `pmix_pstat_base_module_t` and a priority. Return no module / a failure
  when the component cannot run in this environment.
- Implement `query` following the established shape: parse the request
  into a `pmix_pstat_op_t` with the base helpers, select target peers with
  `PMIX_PSTAT_APPEND_PEER_UNIQUE`, run `update()` synchronously through
  `op->cb` for the immediate answer, and — if a rate was given — append
  the op to `pmix_pstat_base.ops` and arm the timer with
  `PMIX_PSTAT_OP_START`. Honor `PMIX_MONITOR_CANCEL`.
- Return each result value under the correct published attribute
  (`PMIX_PROC_RESOURCE_USAGE`, `PMIX_NODE_RESOURCE_USAGE`,
  `PMIX_DISK_RESOURCE_USAGE`, `PMIX_NETWORK_RESOURCE_USAGE`, each a
  `PMIX_DATA_ARRAY` of the per-item info), so callers can parse them
  uniformly regardless of which component produced them.
- Document any new statistic attribute keys in
  [`include/pmix_common.h.in`](../../../include/pmix_common.h.in) and the
  user docs, per the attribute rules in the top-level guide, and teach the
  matching base parse helper to recognize them.
