<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSENSOR `heartbeat` Component

`heartbeat` is the `psensor` component that watches for periodic
`PMIx_Heartbeat()` beacons from a monitored process and raises
`PMIX_MONITOR_HEARTBEAT_ALERT` when they stop arriving. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `heartbeat`. It claims a start request whose `monitor->key` is
`PMIX_MONITOR_HEARTBEAT`.

## Files

| File | Contents |
|------|----------|
| `psensor_heartbeat.h` | Component/module symbols + the `recv_beats` PTL callback declaration. |
| `psensor_heartbeat_component.c` | Component struct + `component_query` (priority **5**, "irrelevant", always available). |
| `psensor_heartbeat.c` | The module: `start`/`stop`, the tracker, the window timer, and heartbeat reception. |

## When it is selected

`heartbeat_query` always returns the module (priority 5). Because
`psensor` is multi-select and routing is by `monitor->key`, being on the
`actives` list is not the same as being chosen: `heartbeat_start` returns
`PMIX_ERR_TAKE_NEXT_OPTION` unless `monitor->key == PMIX_MONITOR_HEARTBEAT`,
so it only ever acts on heartbeat requests.

## The component and module structs

The component wraps the base struct to hang two pieces of per-component
state (`psensor_heartbeat.h`):

```c
typedef struct {
    pmix_psensor_base_component_t super;
    bool recv_active;          // have we posted the heartbeat PTL recv yet?
    pmix_list_t trackers;      // one pmix_heartbeat_trkr_t per monitored requestor
} pmix_psensor_heartbeat_component_t;
```

The module is the usual `{ .start, .stop }` pair.

## The tracker (`pmix_heartbeat_trkr_t`)

One per monitored requestor, held on `component.trackers`. Key fields:
`requestor` (retained peer), `tv` (window length), `nbeats` (beats seen
this window), `ndrops`, `error`, `range`, `info`/`ninfo` (payload for the
event), and **`stopped`** (latch, see below). Its destructor releases the
peer, frees `id`/`info`, and deletes the timer if `event_active`.

## `heartbeat_start` — arming a window timer and the shared recv

1. Decline unless `monitor->key == PMIX_MONITOR_HEARTBEAT`.
2. Allocate the tracker, retain the requestor, and parse directives:
   `PMIX_MONITOR_HEARTBEAT_TIME` → `tv.tv_sec`,
   `PMIX_MONITOR_HEARTBEAT_DROPS` → `ndrops`, `PMIX_RANGE` → `range`.
3. If `tv.tv_sec == 0` (no window given), release and return
   `PMIX_ERR_BAD_PARAM`.
4. **Post the heartbeat receive once.** If
   `component.recv_active` is false, allocate a `pmix_ptl_posted_recv_t`
   on `PMIX_PTL_TAG_HEARTBEAT` with cbfunc
   `pmix_psensor_heartbeat_recv_beats`, **prepend** it to
   `pmix_ptl_base.posted_recvs`, and set `recv_active = true`. This is the
   one place `psensor` reaches directly into `ptl` internals — the recv is
   shared by *all* heartbeat trackers, not one per requestor.
5. Thread-shift to `add_tracker`, which appends the tracker and arms an
   `evtimer` (`check_heartbeat`) at `tv`.

## Heartbeat reception: `recv_beats` → `add_beat`

`pmix_psensor_heartbeat_recv_beats` is the PTL callback that fires on the
`ptl` progress thread whenever a `PMIX_PTL_TAG_HEARTBEAT` message arrives
from a peer. It does the minimum — retain the sending `peer` into a small
`pmix_psensor_beat_t` caddy and thread-shift onto `pmix_psensor_base.evbase`
— then `add_beat` (on the monitor thread) finds the tracker whose
`requestor == peer`, increments `nbeats`, and clears `stopped` (a beat
proves the process is alive again). The caddy is released.

## The window check: `check_heartbeat`

Fires every `tv` seconds:

- If `nbeats == 0` **and** `!stopped`: no beat arrived in the window →
  the process appears dead. It builds a `source` proc from the requestor,
  **retains the tracker** (to keep it alive across the async notify), sets
  `stopped = true` so it will **not** re-report every window, and raises
  `PMIX_MONITOR_HEARTBEAT_ALERT` via `PMIx_Notify_event` scoped to
  `range`. The completion callback (`opcbfunc`) releases the retained
  tracker.
- Otherwise it just logs the beat count.
- Either way it resets `nbeats = 0` and re-arms the timer.

Note the tracker is **not removed** on alert (unlike `file`): heartbeat
monitoring persists, and a later beat via `add_beat` clears `stopped` so
the process can be reported alive again and, if it later goes silent once
more, re-alerted. This "latch then revive" behavior is deliberate.

## Gotchas

- **The `stopped` latch is what prevents an alert storm.** Once a process
  is declared dead the tracker keeps firing its timer but stays silent
  until a beat revives it. Do not "simplify" by removing the latch or by
  deleting the tracker on alert — you would either spam events every
  window or lose the ability to notice the process recovering.
- **The PTL recv is shared and posted lazily.** It is prepended to
  `pmix_ptl_base.posted_recvs` on the first heartbeat `start` and left in
  place (`recv_active` never resets, and `stop` does not un-post it). If
  you rework teardown, remember the recv outlives individual trackers by
  design.
- **This component depends on `ptl` internals** (`pmix_ptl_base.posted_recvs`,
  `PMIX_PTL_TAG_HEARTBEAT`, `pmix_ptl_posted_recv_t`). Beat delivery only
  works because `PMIx_Heartbeat()` sends on that reserved tag; the two
  ends are a matched pair — see the `ptl` framework doc for the tag
  reservation.
- Priority 5 is marked "irrelevant" in the source; routing is by
  `monitor->key`, so do not read meaning into the number.
