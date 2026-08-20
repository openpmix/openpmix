<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSENSOR `heartbeat` Component

`heartbeat` is the `psensor` component that watches for periodic
`PMIx_Heartbeat()` beacons from a monitored process and alerts when they
stop arriving. Read the framework
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

`PMIX_MONITOR_CANCEL` is handled before that gate — `heartbeat_start`
passes the id in `monitor->value` to `heartbeat_stop()` and then declines
anyway, so the cancel goes on to `pstat`. The framework
[`AGENTS.md`](../AGENTS.md) explains why declining is the right answer.

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
`requestor` (retained peer), `id` (the `PMIX_MONITOR_ID` cancel handle),
`tv` (window length), `nbeats` (beats seen this window), `nmissed`
(consecutive empty windows), `ndrops` (how many of those are tolerated),
`error` (the status to raise), `range`, `info`/`ninfo` (payload for the
event), and **`stopped`** (latch, see below). Its destructor releases the
peer, frees `id`/`info`, and deletes the timer if `event_active`.

## `heartbeat_start` — arming a window timer and the shared recv

1. Decline unless `monitor->key == PMIX_MONITOR_HEARTBEAT`.
2. Allocate the tracker, retain the requestor, and parse directives:
   `PMIX_MONITOR_HEARTBEAT_TIME` → `tv.tv_sec`,
   `PMIX_MONITOR_HEARTBEAT_DROPS` → `ndrops`, `PMIX_MONITOR_ID` → `id`,
   `PMIX_RANGE` → `range`. The two numbers go through
   `PMIx_Value_get_number` and the other two are type-checked, because
   all four arrive off the wire; a directive that will not convert takes
   the whole request down with `PMIX_ERR_BAD_PARAM`.
3. If `tv.tv_sec == 0` (no window given), release and return
   `PMIX_ERR_BAD_PARAM`.
4. **Post the heartbeat receive once.** If
   `component.recv_active` is false, allocate a `pmix_ptl_posted_recv_t`
   on `PMIX_PTL_TAG_HEARTBEAT` with cbfunc
   `pmix_psensor_heartbeat_recv_beats`, **prepend** it to
   `pmix_ptl_base.posted_recvs`, and set `recv_active = true`. This is the
   one place `psensor` reaches directly into `ptl` internals — the recv is
   shared by *all* heartbeat trackers, not one per requestor.
5. Thread-shift to `add_tracker`, which appends the tracker and arms a
   **persistent** timer (`check_heartbeat`) at `tv`. It is persistent for
   a reason — see "The tracker timer is persistent" in the framework
   [`AGENTS.md`](../AGENTS.md); `check_heartbeat` must never re-arm it.

## Heartbeat reception: `recv_beats` → `add_beat`

`pmix_psensor_heartbeat_recv_beats` is the PTL callback that fires on the
`ptl` progress thread whenever a `PMIX_PTL_TAG_HEARTBEAT` message arrives
from a peer. It does the minimum — retain the sending `peer` into a small
`pmix_psensor_beat_t` caddy and thread-shift onto `pmix_psensor_base.evbase`
— then `add_beat` (on the monitor thread) finds the tracker whose
`requestor == peer`, increments `nbeats`, and clears both `stopped` and
`nmissed` (a beat proves the process is alive again, and gives back the
whole drop allowance). The caddy is released.

Note `add_beat` stops at the **first** tracker matching the peer, so a
requestor holding two heartbeat monitors credits only one of them per
beat.

## The window check: `check_heartbeat`

Fires every `tv` seconds:

- If `nbeats == 0` **and** `!stopped`: no beat arrived in this window, so
  `nmissed++`. While `nmissed <= ndrops` the requestor's drop allowance
  still covers it and the sampler simply returns. Past that the process
  is declared dead: it builds a `source` proc from the requestor,
  **retains the tracker** (to keep it alive across the async notify), sets
  `stopped = true` so it will **not** re-report every window, and raises
  the alert via `PMIx_Notify_event` scoped to `range` — with the status
  the requestor asked for, or `PMIX_MONITOR_HEARTBEAT_ALERT` if they
  passed `PMIX_SUCCESS`. The completion callback (`opcbfunc`) releases the
  retained tracker; if `PMIx_Notify_event` returns an error it never runs,
  so the sampler gives that reference back itself.
- Otherwise it just logs the beat count. `add_beat` is what clears
  `nmissed`, which is what makes the count *consecutive*.
- Either way it resets `nbeats = 0`. It does **not** re-arm the timer:
  the timer is persistent and libevent re-armed it before this callback
  was entered.

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
- **The sampler must not re-arm its own timer.** `check_heartbeat` hands
  a *retained* tracker to an asynchronous `PMIx_Notify_event`, and the
  completion callback that releases it runs on the library's progress
  thread, not this one — so the tracker's `pmix_event_del` can race a
  sample in flight. A one-shot re-armed from the end of the sampler
  survives that delete; a persistent one does not. The full argument is
  in the framework [`AGENTS.md`](../AGENTS.md).
- **The PTL recv is shared, posted lazily, and retired by
  `heartbeat_close`.** It is prepended to `pmix_ptl_base.posted_recvs` on
  the first heartbeat `start` and outlives individual trackers by design —
  `stop` does not un-post it. Component close is the one place that does,
  and it must, for two separate reasons: the recv names a function in this
  component while `ptl` closes *after* `psensor` (so a DSO build would
  leave the ptl list pointing into an unloaded plugin), and `recv_active`
  lives in a file-scope struct that outlives the library, so a second
  `PMIx_server_init` in the same process would find the flag set, the
  recv gone, and never count another beat.
- **Being posted lazily leaves a window, and the server screens for it.**
  A client may call `PMIx_Heartbeat()` before any monitor has been armed.
  Until the recv exists, the server's *wildcard* recv matches the beat
  and hands it to the command switchyard — so
  `pmix_server_message_handler` drops `PMIX_PTL_TAG_HEARTBEAT` explicitly
  rather than trying to read a command out of it and replying with the
  error. If you ever make this recv eager, that screen becomes dead code
  rather than wrong code, but say so when you remove it.
- **This component depends on `ptl` internals** (`pmix_ptl_base.posted_recvs`,
  `PMIX_PTL_TAG_HEARTBEAT`, `pmix_ptl_posted_recv_t`). Beat delivery only
  works because `PMIx_Heartbeat()` sends on that reserved tag; the two
  ends are a matched pair — see the `ptl` framework doc for the tag
  reservation.
- Priority 5 is marked "irrelevant" in the source; routing is by
  `monitor->key`, so do not read meaning into the number.
