<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSENSOR `file` Component

`file` is the `psensor` component that watches a file for signs of life —
changes to its size, access time, or modification time — and raises an
alert when the file goes stale. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `file`. It claims a start request whose `monitor->key` is
`PMIX_MONITOR_FILE`, with the file path carried in `monitor->value`.

## Files

| File | Contents |
|------|----------|
| `psensor_file.h` | Component/module symbols. |
| `psensor_file_component.c` | Component struct + `component_query` (priority **20**, "irrelevant", always available). |
| `psensor_file.c` | The module: `start`/`stop`, the tracker, and the `stat` sampler. |
| `help-pmix-psensor-file.txt` | `show_help` topic `file-stalled`. |

## When it is selected

`psensor_file_query` always returns the module (priority 20). As with all
`psensor` components, selection onto `actives` is not the same as being
chosen for a request: `start` returns `PMIX_ERR_TAKE_NEXT_OPTION` unless
`monitor->key == PMIX_MONITOR_FILE`.

`PMIX_MONITOR_CANCEL` is handled before that gate — `start` passes the id
in `monitor->value` to `stop()` and then declines anyway, so the cancel
goes on to `pstat`. The framework
[`AGENTS.md`](../AGENTS.md) explains why declining is the right answer.

## The component and tracker structs

The component wraps the base struct to hold its tracker list
(`psensor_file.h`):

```c
typedef struct {
    pmix_psensor_base_component_t super;
    pmix_list_t trackers;      // one file_tracker_t per monitored file/requestor
} pmix_psensor_file_component_t;
```

Each `file_tracker_t` records the retained `requestor`, the `file` path,
the caller's `id` (the `PMIX_MONITOR_ID` cancel handle) and `error` (the
status to raise), the sample interval `tv`, the three watch flags
`file_size` / `file_access` / `file_mod`, the last-seen `last_size` /
`last_access` / `last_mod`, the `sampled` baseline latch, the counters
`ndrops` (tolerance) and `nmisses` (consecutive unchanged checks), plus
`range` and `info`/`ninfo` for the event.

`last_size` is an `off_t`, not a `size_t`: it holds `st_size`, and the
two are not the same width wherever large-file support is on and pointers
are 32 bits. It used to be a `size_t` compared through an `(int64_t)`
cast, which is the shape a truncating assignment leaves behind.

**The constructor must initialize every pointer field.** `PMIX_NEW`
mallocs and runs the constructor; it does not zero. `file` was set only
in `start`, which happens to assign it before any path that could release
the tracker — a property no one is required to preserve.

## `start` — validating and arming

1. Decline unless `monitor->key == PMIX_MONITOR_FILE`; then `strdup` the
   path from `monitor->value.data.string`.
2. Parse directives: `PMIX_MONITOR_FILE_SIZE` / `_ACCESS` / `_MODIFY` set
   the corresponding watch flag; `PMIX_MONITOR_FILE_DROPS` → `ndrops`;
   `PMIX_MONITOR_FILE_CHECK_TIME` → `tv.tv_sec`; `PMIX_MONITOR_ID` →
   `id`; `PMIX_RANGE` → `range`. The two numbers go through
   `PMIx_Value_get_number` and the other two are type-checked, because
   all four arrive off the wire; a directive that will not convert takes
   the whole request down with `PMIX_ERR_BAD_PARAM` rather than leaving a
   tracker built from whatever the union happened to hold.
3. **Reject an incomplete request:** if the interval is zero **or** none
   of the three watch flags is set, release the tracker and return
   `PMIX_ERR_BAD_PARAM`. A file monitor needs both *how often* to look and
   *what* to look at.
4. Thread-shift to `add_tracker`, which appends the tracker and arms a
   **persistent** timer (`file_sample`) at `tv`. It is persistent for a
   reason — see "The tracker timer is persistent" in the framework
   [`AGENTS.md`](../AGENTS.md); `file_sample` must never re-arm it.

The path itself is validated before the tracker is built: it arrives off
the wire in `monitor->value`, so a value that is not a `PMIX_STRING`, or
a `NULL` string, is rejected with `PMIX_ERR_BAD_PARAM` rather than handed
to `strdup`.

## `file_sample` — the staleness check

Fires every `tv` seconds:

- `stat` the file. **If the `stat` fails** (file not present yet), it
  simply returns — a not-yet-created file is not a fault, and the
  persistent timer brings us back to look again. Note the consequence: a
  file that is *deleted* after being watched also stops the count rather
  than tripping it. That is deliberate, not an oversight.
- **The first sample only records a baseline** (`sampled`) and returns.
  There is nothing to compare against yet, and the constructor's zeroes
  are values a real file can genuinely have — a monitor watching the size
  of a legitimately empty file used to score a miss on its very first
  look.
- Afterwards, compare **every** watched attribute to its last-seen value.
  Any one of them moving means the application is alive: `nmisses = 0`.
  Only if none moved does `nmisses++`. (An `if / else if / else if` chain
  used to check just the first flag that was set, so a request naming
  both a size and a modification time silently got one of them.)
- **Alert when `0 < nmisses` and `nmisses >= ndrops`:** the file is
  declared stalled. At
  verbosity >4 it emits the `file-stalled` `show_help`; it then **removes
  the tracker from the list** and raises the alert via
  `PMIx_Notify_event` (source = requestor, scope = `range`, status =
  the requestor's `error`, or `PMIX_MONITOR_FILE_ALERT` if they asked for
  `PMIX_SUCCESS`). The completion callback (`opcbfunc`) releases the
  tracker — **unless `PMIx_Notify_event` returns an error**, in which case
  it never runs and the sampler has to release the tracker itself; by
  then that reference is the last one. Because the timer
  is persistent it is **still armed** at that point — libevent re-armed it
  before entering this callback — so `file_sample` **deletes it
  explicitly** before letting go of the tracker. A file monitor is
  one-shot: it fires once and is gone.
- Otherwise it does nothing further; the timer brings it back on its own.

## Gotchas

- **`ndrops` counts misses, and a miss is not the same as a sample.** The
  baseline sample scores nothing, and the alert needs `0 < nmisses` as
  well as `nmisses >= ndrops`. Both halves are load-bearing: an equality
  test against a default `ndrops` of 0 fired on the first sample of a
  perfectly healthy file, because a `nmisses` of 0 means "nothing has
  been missed", not "the tolerance is exhausted". So a request with no
  `PMIX_MONITOR_FILE_DROPS` alerts on the first check that shows no
  change — which is what asking to tolerate zero misses means.
- **Two `ctime()` calls in one argument list return the same buffer.**
  Both the verbose sample line and the `file-stalled` topic print an
  access time and a modification time; done with `ctime()` they printed
  the same timestamp twice, and scribbled on any `ctime`/`asctime` result
  the application held on another thread. `render_time()` wraps
  `ctime_r`, and deliberately keeps the trailing newline `ctime` emits —
  the help topic runs its next label straight up against it.
- **The monitor is one-shot.** Unlike `heartbeat`, `file` removes and
  releases its tracker on the first alert. If you need ongoing monitoring
  after a stall, the caller must start a new monitor.
- **The sampler must not re-arm the timer, and must disarm before it
  hands the tracker away.** Both halves come from the same argument in
  the framework doc: re-arming a one-shot from the sampler races a
  `pmix_event_del` made from another thread, and a persistent timer left
  armed on a released tracker fires into freed memory. `make check` covers
  this end to end — `test/simple/simpmonitor.c` watches a file it never
  touches, which only alerts if the timer fires more than once.
- **`stat` is a TOCTOU** (the source carries a `coverity[TOCTOU]`
  annotation): the file can change between the `stat` and any action.
  That is acceptable for a liveness heuristic — do not "harden" it into
  something that assumes the stat is authoritative.
- Priority 20 is marked "irrelevant"; routing is by `monitor->key`, so the
  number does not arbitrate against `heartbeat`.
