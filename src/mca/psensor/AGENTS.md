<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSENSOR Framework

This document orients AI agents and human contributors working in the
`psensor` (**P**MIx **Sensor**) framework. It assumes you have already
read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden rules,
prefix conventions, thread-safety/caddy model, and MCA concepts described
there all apply here and are not repeated. This file covers what is
specific to `psensor`: what the framework is for, how a monitoring
request is routed to a component, the per-request tracker objects the
components build, the events they raise, and the contract a component
must honor. Each component subdirectory (`file/`, `heartbeat/`) carries
its own `AGENTS.md` with component-specific detail.

There is no `docs/how-things-work/psensor.rst`; the closest companion
reading is the sibling [`pstat`](../pstat/AGENTS.md) framework, which
implements the *resource-usage* side of the `PMIx_Process_monitor` family
and is where the monitor request path lands today (see "Current reality"
below).

## What PSENSOR does

`psensor` runs **liveness/health monitors** on behalf of a requestor and
raises a PMIx **event** when a monitored condition trips. Unlike `pstat`,
which samples numeric resource statistics and returns them, `psensor` is
purely about *detecting a fault* — an application that has gone silent —
and *notifying* someone. It ships two monitors:

- **`heartbeat`** — the requestor promises to send periodic
  `PMIx_Heartbeat()` beacons to its server. `psensor/heartbeat` counts the
  beats arriving in each time window; if a window passes with **no** beat,
  it raises `PMIX_MONITOR_HEARTBEAT_ALERT`. This is dropped-heartbeat
  detection: the process is presumed stalled or dead.
- **`file`** — the requestor names a file that a healthy application is
  expected to keep touching (growing it, or updating its access or
  modification time). `psensor/file` `stat`s the file on an interval; if
  the watched attribute has not changed across a configured number of
  checks, it raises `PMIX_MONITOR_FILE_ALERT`. This is staleness
  detection for applications that write progress to a file rather than
  emit heartbeats.

Like `pstat`, `psensor` only ever runs **inside a PMIx server** — it is
opened and selected during server startup in
[`src/server/pmix_server.c`](../../server/pmix_server.c) (search for
`pmix_psensor_base_framework`), immediately after the `pmix_pmdl`
env-var parse and before the singleton/listener setup. Clients and tools
never load it; they relay monitor requests to their server.

## Multi-select, routed by monitor key

`psensor` is a **multi-select** framework: every runnable component is
active simultaneously and sits on the priority-ordered
`pmix_psensor_base.actives` list. A start request is **offered to each
active module in turn**; a module inspects the request's `monitor->key`
and either claims it or declines by returning
`PMIX_ERR_TAKE_NEXT_OPTION`:

- `heartbeat` claims a request whose `monitor->key` is
  `PMIX_MONITOR_HEARTBEAT`.
- `file` claims a request whose `monitor->key` is `PMIX_MONITOR_FILE`.

Because the two components gate on **mutually exclusive** keys, their
priorities (`file` = 20, `heartbeat` = 5 — both commented "irrelevant" in
the source) never actually arbitrate a tie. The multi-select machinery is
what lets a *new* monitor type be added as a component that recognizes a
new key without touching the others; the priority ordering is essentially
vestigial today, much as the `ptl` role gates make its priorities
vestigial.

## Current reality: the start path is dormant

Read this before assuming `psensor` is on the hot monitor path. The
public monitor API in
[`src/common/pmix_monitor.c`](../../common/pmix_monitor.c) resolves a
request's scope and, for local participation, calls **`pmix_pstat.query`**
— *not* `pmix_psensor.start`. In the current tree there is **no live
caller of `pmix_psensor.start`** (the framework's own
`pmix_psensor_base_start` is wired into the `pmix_psensor.start` slot, but
nothing invokes it). What *is* still called is **`pmix_psensor.stop`**:

- `src/server/pmix_server.c` calls `pmix_psensor.stop(peer, NULL)` when a
  client departs, and
- `src/mca/ptl/base/ptl_base_sendrecv.c` calls it from the
  lost-connection teardown.

So the framework is presently in a transitional state: its teardown path
is live (a defensive "stop any monitors this peer started" on
disconnect), while its start path is not reached by the shipped monitor
API. The heartbeat wire mechanism it depends on is, however, fully
present — `PMIx_Heartbeat()` still sends `PMIX_PTL_TAG_HEARTBEAT` beacons,
and the `heartbeat` component still knows how to receive and count them —
so the monitors work end-to-end **if** something calls `start`. Do not
delete the start machinery on the assumption that it is dead code; treat
it as an intentionally-retained capability whose driver has moved. If you
are wiring monitoring, understand that `pstat` and `psensor` split the job
(statistics vs. liveness) and that the connective tissue on the psensor
side is what is missing.

## Module interface (`pmix_psensor_base_module_t`)

Defined in [`psensor.h`](psensor.h). Both function pointers **must** be
provided by a component (the header comment says so, and both components
supply both):

| Field | Signature | Purpose |
|-------|-----------|---------|
| `start` | `(pmix_peer_t *requestor, pmix_status_t error, const pmix_info_t *monitor, const pmix_info_t directives[], size_t ndirs)` | Begin a monitor for `requestor`. Inspect `monitor->key`; if it is not this component's key, return `PMIX_ERR_TAKE_NEXT_OPTION`. Otherwise build a tracker from `directives` and arm a timer. `error` is the status code the caller wants raised if the monitor trips. |
| `stop` | `(pmix_peer_t *requestor, char *id)` | Tear down monitors. A `NULL` `id` means "stop **all** monitors this requestor started"; a non-`NULL` `id` matches the caller-supplied `PMIX_MONITOR_ID` handle. |

The component data structure is just a typedef of the standard
`pmix_mca_base_component_t` (**`pmix_psensor_base_component_t`**), and the
version macro every component struct opens with is
**`PMIX_PSENSOR_BASE_VERSION_1_0_0`**. The exported global
**`pmix_psensor`** (in `psensor_base_frame.c`) holds the two base
dispatch functions; all back-end code that drives the framework calls
through `pmix_psensor.start(...)` / `pmix_psensor.stop(...)`.

Note both components **wrap** the base component struct in a larger
per-component struct (`pmix_psensor_file_component_t`,
`pmix_psensor_heartbeat_component_t`) so they can hang their `trackers`
list — and, for heartbeat, a `recv_active` flag — off the component
itself. This differs from `pstat`, whose components carry no extra state.

## Selection and lifecycle (`base/`)

The base is small — three files — and, unlike `ptl`, it holds no monitor
logic of its own; the real work lives in the components. The base only
dispatches.

### `base/psensor_base_frame.c` — globals, open/close, MCA param

- Declares the framework with
  `PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, psensor, "PMIx Monitoring Sensors", ...)`.
- Instantiates the global **`pmix_psensor`** module (its `start`/`stop`
  pointing at `pmix_psensor_base_start` / `pmix_psensor_base_stop`) and the
  global state struct **`pmix_psensor_base`** (`pmix_psensor_base_t`),
  which holds the `actives` list, the framework `evbase`, and a `selected`
  guard bool.
- Defines the `PMIX_CLASS_INSTANCE` for `pmix_psensor_active_module_t`
  (the `{ component, module, priority }` wrapper held on `actives`).
- **`pmix_psensor_base_open`** decides where monitor timers run, governed
  by the framework MCA parameter **`pmix_psensor_base_use_separate_thread`**
  (bool, default false):
  - **false (default):** `pmix_psensor_base.evbase` is aliased to
    `pmix_globals.evbase` — monitors share the library's main progress
    thread.
  - **true:** a dedicated progress thread named `"PSENSOR"` is spun up via
    `pmix_progress_thread_init`, so file `stat`s and heartbeat bookkeeping
    cannot perturb the main thread. `close` stops it.
  It then constructs the `actives` list and opens all components.
- **`pmix_psensor_base_close`** clears `selected`, destructs `actives`,
  stops the `"PSENSOR"` thread if one was started, and closes the
  components.

### `base/psensor_base_select.c` — priority-ordered activation

`pmix_psensor_base_select()` (called once from server startup) queries
every component's `pmix_mca_query_component`, wraps each returned module in
a `pmix_psensor_active_module_t`, and **inserts it into `actives` in
descending priority order**. It is idempotent (guarded by
`pmix_psensor_base.selected`). Finding **zero** components is *not* fatal
here — there is no `no-plugins` `show_help` and no error return; an empty
`actives` list simply means `pmix_psensor_base_start` will later report
`PMIX_ERR_NOT_SUPPORTED`. At verbosity >4 it prints the resolved priority
list.

### `base/psensor_base_stubs.c` — the dispatch functions

These are the two functions the `pmix_psensor` global points at:

- **`pmix_psensor_base_start`** walks `actives` in priority order and
  calls each module's `start`. A module that returns neither
  `PMIX_SUCCESS` nor `PMIX_ERR_TAKE_NEXT_OPTION` aborts the walk and that
  error is returned. Any module that *has* a `start` pointer sets an
  internal `didit` flag; if **no** module has one, the function returns
  `PMIX_ERR_NOT_SUPPORTED` so the server knows to ask the host RM instead.
  Note the consequence of offering the request to *every* module: the
  matching component claims it and the others cheaply decline with
  `TAKE_NEXT_OPTION`.
- **`pmix_psensor_base_stop`** walks `actives` and calls every module's
  `stop`, **continuing past errors** (it remembers the first non-success
  and keeps going) so that a stop reliably tears down monitors in *all*
  components — a single requestor could in principle hold both a file and
  a heartbeat monitor.

## The tracker + threadshift pattern (shared by both components)

Neither component does its work in the `start`/`stop` call itself. Both
follow the same shape, and understanding it once covers both:

1. **`start`** validates `monitor->key`, allocates a per-request
   **tracker** object (`file_tracker_t` / `pmix_heartbeat_trkr_t`),
   `PMIX_RETAIN`s the `requestor` peer into it, and parses the
   `directives` into the tracker's fields. If the request is missing a
   sample interval (or, for `file`, anything to watch) it releases the
   tracker and returns `PMIX_ERR_BAD_PARAM`.
2. It then **thread-shifts onto `pmix_psensor_base.evbase`** — not with
   `PMIX_THREADSHIFT`, but by `pmix_event_assign`-ing a scratch event
   (`ft->cdev`) and firing it with `pmix_event_active(..., EV_WRITE, 1)`,
   preceded by `PMIX_POST_OBJECT`. The handler (`add_tracker`) runs on the
   monitor thread, appends the tracker to the component's `trackers` list,
   and arms an `evtimer` (`file_sample` / `check_heartbeat`) at the
   sample interval.
3. **`stop`** allocates a small caddy (`file_caddy_t` /
   `heartbeat_caddy_t` — each a `pmix_object_t` with its own `ev`,
   a retained `requestor`, and an optional `id`), thread-shifts it the
   same way, and its handler (`del_tracker`) removes matching trackers
   from the list and releases them (which deletes the armed timer via the
   tracker destructor).

The tracker destructor is the single teardown point: it releases the
retained `requestor`, frees `id`/`file`/`info`, and `pmix_event_del`s the
timer if `event_active`. Releasing a tracker is therefore the clean way to
cancel a monitor.

All tracker-list access, timer fires, and (for heartbeat) beat counting
happen **on `evbase`** — so the components touch shared state only on the
monitor thread, exactly as the top-level thread-safety rules require.

## Monitor directives the components honor

The `monitor` argument's **key** selects the component (above); its
**value** carries the target (a file path for `file`; unused for
`heartbeat`). The `directives` array qualifies the request:

| Directive | Consumed by | Effect |
|-----------|-------------|--------|
| `PMIX_MONITOR_HEARTBEAT_TIME` | heartbeat | uint32 seconds per heartbeat window (**required**; 0 ⇒ `PMIX_ERR_BAD_PARAM`) |
| `PMIX_MONITOR_HEARTBEAT_DROPS` | heartbeat | (parsed into `ndrops`) |
| `PMIX_MONITOR_FILE_SIZE` / `PMIX_MONITOR_FILE_ACCESS` / `PMIX_MONITOR_FILE_MODIFY` | file | which `stat` attribute signals life (at least one **required**) |
| `PMIX_MONITOR_FILE_CHECK_TIME` | file | uint32 seconds between `stat`s (**required**; 0 ⇒ `PMIX_ERR_BAD_PARAM`) |
| `PMIX_MONITOR_FILE_DROPS` | file | number of unchanged checks tolerated before alerting (`ndrops`) |
| `PMIX_RANGE` | both | `pmix_data_range_t` scope for the raised event (default `PMIX_RANGE_NAMESPACE`) |

The requestor's `PMIX_MONITOR_ID` string (the cancel handle used by
`stop`) is part of the tracker contract but is not itself parsed in the
current component `start` routines — a caller cancels by requestor peer,
optionally narrowed by `id`, in `stop`.

## Events raised

When a monitor trips, the component calls the **public**
`PMIx_Notify_event(...)` — the same event path the rest of the library
uses — with a `source` of the monitored requestor and the tracker's
`range`:

| Component | Status code raised |
|-----------|--------------------|
| `heartbeat` | `PMIX_MONITOR_HEARTBEAT_ALERT` (`-109`) |
| `file` | `PMIX_MONITOR_FILE_ALERT` (`-110`) |

(Both codes live in [`include/pmix_common.h.in`](../../../include/pmix_common.h.in).)

## Directory layout

```
src/mca/psensor/
├── psensor.h                    Framework API: module struct, component typedef, version macro
├── base/
│   ├── base.h                   Internal base API + pmix_psensor_base state + active-module class
│   ├── psensor_base_frame.c     open/close, MCA param, framework decl, active-module class
│   ├── psensor_base_select.c    priority-ordered component activation
│   └── psensor_base_stubs.c     pmix_psensor_base_start / _stop dispatch
├── file/                        file-staleness monitor → PMIX_MONITOR_FILE_ALERT
└── heartbeat/                   dropped-heartbeat monitor → PMIX_MONITOR_HEARTBEAT_ALERT
```

## Threading

Everything in `psensor` runs on a PMIx progress thread — either the main
one or the dedicated `"PSENSOR"` thread, per
`pmix_psensor_base_use_separate_thread`. The `start`/`stop` entry points
may be entered from another thread, which is exactly why both components
thread-shift onto `evbase` before touching their `trackers` list. The
heartbeat receive callback (`pmix_psensor_heartbeat_recv_beats`) fires on
the `ptl` progress thread and likewise thread-shifts (`add_beat`) before
incrementing a tracker's beat count. Do not add a code path that touches a
component's `trackers` list without going through `evbase` first.

## Building

The framework core (`base/`) is always built into `libpmix`. Neither
component ships a `configure.m4`, so **both are always compiled** and
wired through the generated `base/static-components.h` — there are no
platform gates here (the monitors rely only on `stat` and the PTL
heartbeat tag, which are portable). Each builds as a standard MCA
component (DSO `pmix_mca_psensor_<name>.la` or static
`libpmix_mca_psensor_<name>.la`).

The `file` component ships a `show_help` file,
[`file/help-pmix-psensor-file.txt`](file/help-pmix-psensor-file.txt)
(topic `file-stalled`). Per the top-level golden rule, after any
add/delete/modify of that text you must
`rm src/util/pmix_show_help_content.* && make` to regenerate the compiled
help content. `heartbeat` includes the `show_help` header but ships no
help file of its own.

Adding a monitor means creating `src/mca/psensor/<name>/` with the usual
`Makefile.am`, a component struct opened with
`PMIX_PSENSOR_BASE_VERSION_1_0_0`, and a module supplying `start`/`stop`;
the framework picks it up through `static-components.h` with no core
changes. Editing a `Makefile.am` only needs a plain `make`; adding or
removing a *component directory* changes the build wiring resolved by
`configure`, so re-run `./autogen.pl && ./configure ... && make`.

## When working in this framework

- **Gate every `start` on `monitor->key` and decline with
  `PMIX_ERR_TAKE_NEXT_OPTION`.** That is how multi-select routing works;
  returning a hard error instead would abort the base's walk and starve a
  sibling component of a request that was never meant for you.
- **Both `start` and `stop` must thread-shift before touching shared
  state.** Follow the existing `pmix_event_assign` + `pmix_event_active`
  pattern (the caddies/trackers already carry a `pmix_event_t`); never
  manipulate the `trackers` list inline in the entry point.
- **Raise faults with `PMIx_Notify_event`, not a private mechanism**, and
  scope the event with the tracker's `PMIX_RANGE`. Register any new alert
  status code in `include/pmix_common.h.in` per the top-level rule that
  every status value be unique across the whole code base.
- **Keep `stop` tolerant.** A `NULL` `id` means "all monitors for this
  requestor," and the base deliberately calls `stop` on *every* component;
  a component with nothing to stop must simply do nothing and return
  success. This matters because `pmix_psensor.stop` runs on every client
  disconnect and lost connection.
- **Don't assume `start` is on the hot path today** (see "Current
  reality"). If you are re-connecting the monitor API to `psensor`, do it
  in `src/common/pmix_monitor.c` alongside the existing `pmix_pstat.query`
  call, and add a `make check` test that drives a real
  `PMIx_Process_monitor` heartbeat/file request end-to-end.
