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
implements the *resource-usage* side of the `PMIx_Process_monitor` family.
The two split the job: `psensor` takes the liveness monitors and `pstat`
takes the statistics, and `src/common/pmix_monitor.c` offers every
request to `psensor` first.

## What PSENSOR does

`psensor` runs **liveness/health monitors** on behalf of a requestor and
raises a PMIx **event** when a monitored condition trips. Unlike `pstat`,
which samples numeric resource statistics and returns them, `psensor` is
purely about *detecting a fault* — an application that has gone silent —
and *notifying* someone. It ships two monitors:

- **`heartbeat`** — the requestor promises to send periodic
  `PMIx_Heartbeat()` beacons to its server. `psensor/heartbeat` counts the
  beats arriving in each time window; once more consecutive windows have
  passed with **no** beat than the requestor said it would tolerate, it
  alerts. This is dropped-heartbeat detection: the process is presumed
  stalled or dead.
- **`file`** — the requestor names a file that a healthy application is
  expected to keep touching (growing it, or updating its access or
  modification time). `psensor/file` `stat`s the file on an interval; if
  none of the watched attributes has changed across a configured number
  of checks, it alerts. This is staleness detection for applications that
  write progress to a file rather than emit heartbeats.

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

`PMIX_MONITOR_CANCEL` is the exception to the claim/decline shape, and it
is worth understanding before adding a component. Both components act on
a cancel — each hands the id in `monitor->value` to its own `stop()`,
which is already the right matcher — and then **still return
`PMIX_ERR_TAKE_NEXT_OPTION`**. Claiming it would be wrong: the id may
name a `pstat` op rather than a `psensor` tracker, and
`pmix_psensor_base_start` stops walking the moment a module returns
success, so `pmix_monitor.c` would never offer the cancel to the
resource-usage path. Every cancel target is tolerant of an id it does not
hold, so offering the request to everyone costs nothing and strands
nothing. A `monitor->value` that is a `PMIX_STRING` with a `NULL` string
means "cancel every monitor this requestor started"; a value of any other
type is a malformed request, and the components leave it alone rather
than reading it as a request to cancel everything.

Because the two components gate on **mutually exclusive** keys, their
priorities (`file` = 20, `heartbeat` = 5 — both commented "irrelevant" in
the source) never actually arbitrate a tie. The multi-select machinery is
what lets a *new* monitor type be added as a component that recognizes a
new key without touching the others; the priority ordering is essentially
vestigial today, much as the `ptl` role gates make its priorities
vestigial.

## How a request reaches a component

The public monitor API in
[`src/common/pmix_monitor.c`](../../common/pmix_monitor.c) offers every
request to **`pmix_psensor.start`** before anything else. A liveness
monitor watches the *requesting* process itself, so it has no local/remote
target to resolve; `pmix_monitor_processing` recovers the requestor's
peer object and calls `start` with it. `PMIX_ERR_NOT_SUPPORTED` coming
back means no component recognized the key, and the request falls through
to the resource-usage (`pmix_pstat.query`) path. Anything else — success
or a hard error — is the answer.

**`pmix_psensor.stop`** is called from two more places, both teardown:

- `src/server/pmix_server_registration.c` calls `pmix_psensor.stop(peer,
  NULL)` when a client departs, and
- `src/mca/ptl/base/ptl_base_sendrecv.c` calls it from the
  lost-connection teardown.

So a monitor a client started is always torn down when that client goes
away, whether it asked or not.

## Module interface (`pmix_psensor_base_module_t`)

Defined in [`psensor.h`](psensor.h). Both function pointers **must** be
provided by a component (the header comment says so, and both components
supply both):

| Field | Signature | Purpose |
|-------|-----------|---------|
| `start` | `(pmix_peer_t *requestor, pmix_status_t error, const pmix_info_t *monitor, const pmix_info_t directives[], size_t ndirs)` | Begin a monitor for `requestor`. Inspect `monitor->key`; if it is not this component's key, return `PMIX_ERR_TAKE_NEXT_OPTION`. Otherwise build a tracker from `directives` and arm a timer. `error` is the status code the caller wants raised if the monitor trips — see "Events raised" below. |
| `stop` | `(pmix_peer_t *requestor, char *id)` | Tear down monitors. A `NULL` `id` means "stop **all** monitors this requestor started"; a non-`NULL` `id` matches the caller-supplied `PMIX_MONITOR_ID` handle. |

The component data structure is just a typedef of the standard
`pmix_mca_base_component_t` (**`pmix_psensor_base_component_t`**), and the
version macro every component struct opens with is
**`PMIX_MCA_BASE_VERSION(psensor)`**, which stamps in the framework's
interface version from the three **`PMIX_MCA_psensor_*_VERSION`** macros
this header states. The exported global
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
    `pmix_progress_thread_init` **and started** with
    `pmix_progress_thread_start`, so file `stat`s and heartbeat
    bookkeeping cannot perturb the main thread.
  It then constructs the `actives` list and opens all components, undoing
  its own setup if that fails (a framework whose open fails never has its
  close called).
- **`pmix_psensor_base_close`** clears `selected`, **pauses** the
  `"PSENSOR"` thread, destructs `actives`, closes the components, and
  only then **stops** the thread and clears `evbase`. The order is a
  correctness requirement — see "Starting and stopping the monitor
  thread" below.

### Starting and stopping the monitor thread

`pmix_progress_thread_init()` builds an event base and a thread object
but leaves the engine parked; starting it is a separate call. Omitting
that call does not fail loudly: the base exists, trackers arm timers on
it quite happily, and nothing ever runs them. No window is ever checked,
no file is ever sampled, and not even `add_tracker` runs — so a start
returns success and the tracker never reaches its component's list. This
was the state of `psensor` until it was fixed alongside the identical
defect in `pstat`.

Teardown is the other half of the same change, and it is why `close`
pauses and stops in two separate places:

- The tracker lists are destructed by the **component** close functions,
  which run on the finalizing thread rather than on this base's. Every
  live tracker carries a timer armed on the base, so tearing the trackers
  down while the monitor thread still runs can free one out from under
  the sampler executing on it. `pmix_progress_thread_pause` joins the
  thread, so nothing is in flight once it returns.
- The base cannot be freed before that, either: a tracker destructor
  deletes its timer, and `pmix_event_del` reads the base out of the
  event. So the stop — which drops the last reference to the `"PSENSOR"`
  tracker, whose destructor frees the base — has to come *after* the
  components have closed.

`close` clears `pmix_psensor_base.evbase` either way. Note what actually
makes a post-close `pmix_psensor.stop` harmless, because it is *not* that
pointer: nothing checks it, and `event_assign()` silently substitutes
libevent's `current_base` for a `NULL` one, so a caddy posted after close
would land on a base that is no longer looping and simply leak. What
saves it is that `close` destructs `actives` first — so
`pmix_psensor_base_stop` walks an empty list and never reaches a module's
`stop` at all. The same argument covers `start`. Do not "fix" this by
adding a `NULL` check without also explaining why the empty-list argument
stopped holding; and do not reorder `close` so the `actives` destruct
comes after the components close.

**`heartbeat_close` also un-posts the PTL recv it lazily posted.** That
recv names a function in the component, and `ptl` closes *after* this
framework, so between the two the ptl list would otherwise hold a
callback into a component that has been closed — and, in a DSO build,
unloaded. Clearing the component's `recv_active` flag alongside it is the
other half: the flag lives in a file-scope struct that outlives the
library, so a second `PMIx_server_init` in the same process would find it
still set, never re-post, count no beats, and declare every monitored
client dead on its first window.

In the default configuration there is no separate thread to pause:
`evbase` is the library's shared base, which `PMIx_server_finalize` has
already stopped before it closes any framework.

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

Two skips guard the walk, and both matter for a component that does not
yet exist: a component need not supply a query function at all, and one
that does may decline by handing back `PMIX_SUCCESS` and no module. The
first is called unconditionally without the guard; the second lands a
`NULL` module on `actives`, where `pmix_psensor_base_start` dereferences
it on the next monitor request — it checks `mod->module->start` for
`NULL`, which is one level too late. `preg`, the other multi-select
framework, screens for both, and this is the pattern to copy.

### `base/psensor_base_stubs.c` — the dispatch functions

These are the two functions the `pmix_psensor` global points at:

- **`pmix_psensor_base_start`** walks `actives` in priority order and
  calls each module's `start`. A module that returns neither
  `PMIX_SUCCESS` nor `PMIX_ERR_TAKE_NEXT_OPTION` aborts the walk and that
  error is returned. A module that returns `PMIX_SUCCESS` has *serviced*
  the request; if none did — every module declined, or none has a `start`
  pointer at all — the function returns `PMIX_ERR_NOT_SUPPORTED`, which
  is what tells `pmix_monitor.c` to try the resource-usage path and, past
  that, the host RM. Note the consequence of offering the request to
  *every* module: the matching component claims it and the others cheaply
  decline with `TAKE_NEXT_OPTION`.
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
   and arms a persistent timer (`file_sample` / `check_heartbeat`) at the
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

### The tracker timer is persistent

Both components arm their sampler with `pmix_event_assign(...,
PMIX_EV_PERSIST, ...)` plus `pmix_event_add`, and **neither sampler
re-arms its own timer**. That is a correctness requirement rather than a
style choice, and it is the same argument written out at greater length
under "Releasing an op from another thread" in
[`../pstat/AGENTS.md`](../pstat/AGENTS.md).

A repeating timer can be built either way — a one-shot the sampler
re-arms on its way out, or a persistent one libevent re-arms. They are
not equivalent to a thread that wants to free the tracker. A tracker
destructor ends with `pmix_event_del()`, and that can run on a thread
that is *not* this base's: `check_heartbeat` retains the tracker across
an asynchronous `PMIx_Notify_event`, and the completion callback that
releases it runs on the library's own progress thread. `event_del`
(with libevent threading on, which `pmix_init.c` enables) waits for a
callback already running on the event — but it removes the event once,
up front. A one-shot's re-arm happens *after* that removal and outside
the base's lock, so `event_del` can return to a freshly armed timer and
the tracker is freed with a live timer pointing into it.

`EV_PERSIST` moves the re-arm into libevent's `event_persist_closure()`,
made while it still holds the base lock and before the callback is
entered, so a concurrent `event_del` either takes the lock first and
removes the pending timeout or takes it afterwards and removes the
re-armed one. Every interleaving returns disarmed.

Two consequences to keep in mind when editing a sampler:

- **Do not add a `pmix_event_evtimer_add` at the end of one**, and do not
  "simplify" the arming back to `pmix_event_evtimer_set`, which asks for
  flags 0.
- **A sampler that is done monitoring must disarm explicitly.**
  `file_sample` deletes its own timer before handing the tracker to the
  notification, because a persistent timer is still armed while its
  callback runs. That delete is safe precisely because it is made on the
  base's own thread.

## Monitor directives the components honor

The `monitor` argument's **key** selects the component (above); its
**value** carries the target (a file path for `file`; unused for
`heartbeat`). The `directives` array qualifies the request:

| Directive | Consumed by | Effect |
|-----------|-------------|--------|
| `PMIX_MONITOR_HEARTBEAT_TIME` | heartbeat | uint32 seconds per heartbeat window (**required**; 0 ⇒ `PMIX_ERR_BAD_PARAM`) |
| `PMIX_MONITOR_HEARTBEAT_DROPS` | heartbeat | consecutive empty windows tolerated before alerting (`ndrops`; default 0 ⇒ alert on the first) |
| `PMIX_MONITOR_FILE_SIZE` / `PMIX_MONITOR_FILE_ACCESS` / `PMIX_MONITOR_FILE_MODIFY` | file | which `stat` attributes signal life (at least one **required**; **all** of the ones given are watched, and any one moving counts as alive) |
| `PMIX_MONITOR_FILE_CHECK_TIME` | file | uint32 seconds between `stat`s (**required**; 0 ⇒ `PMIX_ERR_BAD_PARAM`) |
| `PMIX_MONITOR_FILE_DROPS` | file | unchanged checks tolerated before alerting (`ndrops`; default 0 ⇒ alert on the first check that shows no change) |
| `PMIX_MONITOR_ID` | both | caller-supplied string handle for this monitor; the cancel key. Must be a non-`NULL` `PMIX_STRING` or the request is `PMIX_ERR_BAD_PARAM`. Naming a monitor twice keeps the first name |
| `PMIX_RANGE` | both | `pmix_data_range_t` scope for the raised event (default `PMIX_RANGE_NAMESPACE`) |

**Read numeric directives through `PMIx_Value_get_number`, not out of the
union.** Every one of these arrives off the wire from a client, which is
free to send the wrong type; reaching straight for `.data.uint32` reads
whatever bits happen to be there and turns a malformed request into a
monitor that quietly never fires. Both components convert, and hand back
the conversion's error. `PMIX_MONITOR_ID` and `PMIX_RANGE` are
type-checked outright for the same reason.

## Events raised

When a monitor trips, the component calls the **public**
`PMIx_Notify_event(...)` — the same event path the rest of the library
uses — with a `source` of the monitored requestor and the tracker's
`range`:

**The status raised is the `error` the requestor supplied**, exactly as
[`PMIx_Process_monitor(3)`](../../../docs/man/man3/PMIx_Process_monitor.3.rst)
promises ("the code the monitor is to use when generating an event
notification"). A requestor that passed `PMIX_SUCCESS` expressed no
preference, and then the component's own code stands:

| Component | Default status code |
|-----------|---------------------|
| `heartbeat` | `PMIX_MONITOR_HEARTBEAT_ALERT` (`-109`) |
| `file` | `PMIX_MONITOR_FILE_ALERT` (`-110`) |

(Both codes live in [`include/pmix_common.h.in`](../../../include/pmix_common.h.in).)
`pstat` does the same thing with its `op->eventcode`, so the two halves
of the monitor API agree.

**`PMIx_Notify_event` does not always reach the callback.** It rejects a
request outright — `PMIX_ERR_INIT`, `PMIX_ERR_NOMEM`, and
`PMIX_ERR_NOT_AVAILABLE` once `pmix_globals.progress_thread_stopped` is
set — and returns before the caddy that would fire `opcbfunc` exists. A
sampler that handed the notification its tracker's last reference must
therefore release it on the error path, or the tracker and the peer it
retains are stranded. The `NOT_AVAILABLE` case is not hypothetical:
`PMIx_server_finalize` stops the progress thread well before it closes
this framework, and in the separate-thread configuration the samplers go
on firing across that gap.

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
`pmix_psensor_base_use_separate_thread`. Both configurations are
exercised by `make check`: `test/unit/run_monitor.pl` runs its
heartbeat-and-file scenario twice, once with the parameter set. The
`start`/`stop` entry points
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
`PMIX_MCA_BASE_VERSION(psensor)`, and a module supplying `start`/`stop`;
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
  disconnect and lost connection — and because a `PMIX_MONITOR_CANCEL` is
  offered to every component too.
- **Honor what the request asked for, or reject it.** A directive parsed
  into a tracker field and then never read is the failure mode this
  framework keeps producing: the drop allowance, the cancel handle and
  the requestor's own status code were each stored and ignored, and every
  one of them looked like working code. If a new directive cannot be
  supported, return `PMIX_ERR_BAD_PARAM` — do not accept it silently.
- **Never re-arm a sampler's own timer, and never start the monitor
  thread without fixing the teardown to match.** See "The tracker timer
  is persistent" and "Starting and stopping the monitor thread" above;
  the two are a pair.
- **Cover a new monitor end to end.** A `start` returns success whether
  or not a sensor was actually armed, so only the arrival of the alert
  proves anything. `test/simple/simpmonitor.c` and
  `test/unit/run_monitor.pl` are the pattern: one rank asks to be
  monitored and then does nothing, another waits for the alert and checks
  its source. Cover the *negative* direction too — that test also arms a
  capped monitor and a cancelled one under a status code of their own, so
  that "this must not alert" is checked rather than assumed. A directive
  that is silently ignored passes every positive test there is.
