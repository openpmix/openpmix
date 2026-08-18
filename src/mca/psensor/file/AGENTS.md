<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSENSOR `file` Component

`file` is the `psensor` component that watches a file for signs of life —
changes to its size, access time, or modification time — and raises
`PMIX_MONITOR_FILE_ALERT` when the file goes stale. Read the framework
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
the sample interval `tv`, the three watch flags `file_size` /
`file_access` / `file_mod`, the last-seen `last_size` / `last_access` /
`last_mod`, the counters `ndrops` (tolerance) and `nmisses` (consecutive
unchanged checks), plus `range` and `info`/`ninfo` for the event.

## `start` — validating and arming

1. Decline unless `monitor->key == PMIX_MONITOR_FILE`; then `strdup` the
   path from `monitor->value.data.string`.
2. Parse directives: `PMIX_MONITOR_FILE_SIZE` / `_ACCESS` / `_MODIFY` set
   the corresponding watch flag; `PMIX_MONITOR_FILE_DROPS` → `ndrops`;
   `PMIX_MONITOR_FILE_CHECK_TIME` → `tv.tv_sec`; `PMIX_RANGE` → `range`.
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
  persistent timer brings us back to look again.
- Compare the watched attribute to its last-seen value and update
  `nmisses`: unchanged ⇒ `nmisses++`; changed ⇒ `nmisses = 0` and the
  last-seen value is refreshed. **Only one attribute is checked per
  sample**, chosen by an `if / else if / else if` chain in the order
  size → access → mod. So if `file_size` is set, the access/mod flags are
  effectively ignored; watch exactly one attribute per monitor to avoid
  surprise.
- **Alert when `nmisses == ndrops`:** the file is declared stalled. At
  verbosity >4 it emits the `file-stalled` `show_help`; it then **removes
  the tracker from the list** and raises `PMIX_MONITOR_FILE_ALERT` via
  `PMIx_Notify_event` (source = requestor, scope = `range`). The
  completion callback (`opcbfunc`) releases the tracker. Because the timer
  is persistent it is **still armed** at that point — libevent re-armed it
  before entering this callback — so `file_sample` **deletes it
  explicitly** before letting go of the tracker. A file monitor is
  one-shot: it fires once and is gone.
- Otherwise it does nothing further; the timer brings it back on its own.

## Gotchas

- **`ndrops` is an equality threshold, not a "≥".** The alert fires the
  moment `nmisses` *equals* `ndrops`. With the default `ndrops == 0`, that
  means the very first sample can alert: `last_size` starts at 0, so a
  freshly-created non-empty file registers a *change* (`nmisses` reset to
  0), and `0 == 0` trips immediately. Callers who want N tolerated misses
  must pass `PMIX_MONITOR_FILE_DROPS` explicitly.
- **Only the first-listed attribute is watched** (size beats access beats
  mod, per the `if/else if` chain). This is easy to misread as "watch all
  three"; it is not.
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
