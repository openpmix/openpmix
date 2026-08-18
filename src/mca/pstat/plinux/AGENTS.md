<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSTAT `plinux` Component

`plinux` ("**P**MIx **Linux**") is the real, production `pstat` component:
it samples resource-utilization statistics from a Linux node by reading
the kernel's `/proc` filesystem. Read the framework
[`AGENTS.md`](../AGENTS.md) first — this file only covers what is specific
to `plinux`. The op object, the `op->cb` synchronous-vs-timer duality, the
four spec structs, and the parse helpers are all framework concepts
described there.

## Files

| File | Contents |
|------|----------|
| `pstat_plinux.h` | Declares the component and module symbols. |
| `pstat_plinux_component.c` | Component struct + `pstat_plinux_component_query`. |
| `pstat_plinux_module.c` | The module: `query`, the periodic `update`, the four `*_stat` readers, and the `/proc` text-parsing helpers. |
| `configure.m4` | Gates the build to Linux-with-`/proc`. |

## Component (`pstat_plinux_component.c`)

The component uses the bare `pmix_pstat_base_component_t`. Its
`pstat_plinux_component_query` is unconditional at run time — all the
"can I run here?" work was done at **configure** time — and simply
returns the module with **priority 80**:

```c
*priority = 80;
*module = (pmix_mca_base_module_t *) &pmix_pstat_plinux_module;
return PMIX_SUCCESS;
```

80 easily beats `test` (20), so on any node where `plinux` was built it
wins selection.

### Build gating (`configure.m4`)

`MCA_pmix_pstat_plinux_CONFIG` sets the component "happy" only when
`oac_found_linux` is `yes` **and** `/proc/cpuinfo` is readable, then
additionally requires that `HZ` is declared (via `<sys/param.h>`). `HZ`
is essential: the module converts the kernel's jiffies-based CPU time
fields into seconds by dividing by `HZ`. On a non-Linux host the whole
component directory is omitted from the build, so nothing here needs a
run-time `#ifdef`.

## Module (`pstat_plinux_module.c`)

`init`/`finalize` are no-ops. All the substance is in `query`, its four
category readers, and the shared line-parsers.

### `query` — request dispatch

`query` implements exactly the framework contract described in the
framework guide. Its shape:

1. **Cancel fast-path.** If the monitor key is `PMIX_MONITOR_CANCEL`,
   find the op by its `PMIX_MONITOR_ID` string in `pmix_pstat_base.ops`,
   remove and release it, and return (unknown id ⇒ success).
2. **Build an op.** `PMIX_NEW(pmix_pstat_op_t)`, copy in the `requestor`
   and `eventcode`, and scan `directives` for `PMIX_MONITOR_ID` and
   `PMIX_MONITOR_RESOURCE_RATE`.
3. **Branch on the monitor key** — one block each for
   `PMIX_MONITOR_PROC_RESOURCE_USAGE`, `..._NODE_...`, `..._DISK_...`,
   `..._NET_...`. Each block:
   - parses its requested fields out of `monitor->value` (a data array)
     with the corresponding base helper;
   - for the proc block, resolves the **target peers** — from
     `PMIX_MONITOR_TARGET_PROCS` / `PMIX_MONITOR_TARGET_PIDS`, or, if
     neither was given, *all* local clients — using
     `PMIX_PSTAT_APPEND_PEER_UNIQUE` against
     `pmix_server_globals.clients`;
   - runs `update()` synchronously via a stack `op->cb` to produce the
     immediate `*results`;
   - if a rate was given, appends the op to `pmix_pstat_base.ops` and arms
     the timer with `PMIX_PSTAT_OP_START(op, op->rate, update)`; otherwise
     `PMIX_RELEASE`s the one-shot op.

The **node** block is the interesting one: `PMIX_MONITOR_NODE_RESOURCE_USAGE`
parses node, net, *and* disk specs from the same array, so a single node
request can nest network and disk sub-arrays inside the returned
`PMIX_NODE_RESOURCE_USAGE` data array. It also filters "empty" answers:
if the collected array holds only the two items the block itself seeded
(hostname + nodeID), it treats that as "no data found" and returns success
with no results.

`PMIX_ERR_EMPTY` from the info-list conversion is mapped to
`PMIX_SUCCESS` in the proc/disk/net blocks — an empty sample is not an
error.

**Everything `query` reads out of `monitor` and `directives` is
untrusted.** For a client request both arrays came off the wire and
nothing between `pmix_server_monitor()`'s unpack and here inspects them.
`PMIx_Check_key` compares only the key, so the *type* has to be checked
before the union is read: `monitor_fields()` is the one place that
unwraps `monitor->value` into the field array (`PMIX_UNDEF` means "every
field in this category", `PMIX_DATA_ARRAY` is the real list, anything
else is `PMIX_ERR_BAD_PARAM`), and `PMIX_MONITOR_ID` is checked for both
type and a `NULL` string before it is `strdup`ed. The framework guide's
"The info array is untrusted" note is the same rule applied one level
down.

After each synchronous `update()` call, `query` checks `cb.status`
before touching the list — see "Who owns the answer list" in the
framework guide. Collection failures arrive that way rather than as a
return value, because `update()`'s signature is libevent's.

### `update` — the collection engine

`update()` is both the synchronous collector (called directly by `query`
with `op->cb` set) and the periodic timer handler (fired by libevent with
`op->cb == NULL`). See the framework guide for the full duality; the
Linux specifics:

- It uses a zeroed spec struct + `memcmp` to decide whether each category
  was requested before doing any file I/O.
- A reader that returns `PMIX_ERR_NOT_FOUND` is reporting an absence, not
  a failure — no `/proc/diskstats` on this kernel, no `/proc/net/dev`
  inside this container, a client that exited between being selected and
  being sampled. `update()` skips that category (or, for a process, that
  peer) and keeps the rest of the sample. Only a genuine error abandons
  it.
- The node path collects node stats into a sub-list and, when net/disk
  were *also* requested as part of a node request, folds them into that
  same sub-list (tracked by `nettaken` / `dktaken` so the standalone
  net/disk blocks below don't double-collect).
- Each reader converts its own sub-list with `PMIx_Info_list_convert()`
  and then `PMIx_Info_list_add()`s the result as a `PMIX_DATA_ARRAY`.
  That add **copies**, so every one of them destructs its array
  afterwards; skipping it leaks a full sample per proc/disk/interface on
  every tick.
- On the timer path it wraps the result as an event: it adds
  `PMIX_EVENT_NON_DEFAULT` and a `PMIX_EVENT_CUSTOM_RANGE` targeting
  `op->requestor`, converts the list, and calls `PMIx_Notify_event` with
  `PMIX_RANGE_CUSTOM`; a `pmix_cb_t` carries the info and is released by
  the `evrelease` completion callback. `PMIx_Notify_event` can refuse the
  request outright — it returns `PMIX_ERR_INIT`,
  `PMIX_ERR_NOT_AVAILABLE` (the progress thread has stopped, which is
  exactly what a monitor still ticking during finalize sees) or
  `PMIX_ERR_NOMEM` *without* ever invoking the callback — so a failed
  notify has to release the caddy itself.

### The four `/proc` readers

Each reader appends one or more `PMIX_*_RESOURCE_USAGE` data arrays (built
with the `PMIx_Info_list_*` helpers) to the answer, each tagged with a
`*_SAMPLE_TIME`.

- **`proc_stat`** — for one peer, opens `/proc/<pid>/stat` and parses it
  field-by-field per `proc(5)`: command name, state, the utime+stime CPU
  time (converted via `HZ` into a `PMIX_TIMEVAL`), priority, thread count,
  and the processor number. It then reads `/proc/<pid>/status` for
  `VmPeak`/`VmSize`/`VmRSS`, and `/proc/<pid>/smaps` (summing every `Pss:`
  line) for proportional set size. Only fields whose `op->pstats` flag is
  set are emitted. Emits `PMIX_PROC_RESOURCE_USAGE`.
- **`node_stat`** — reads `/proc/loadavg` (the three load averages) and
  `/proc/meminfo` (total/free/buffers/cached/swap-cached/swap-total/
  swap-free/mapped), gated by `op->ndstats`. Emits into the node sub-list.
- **`disk_stat`** — reads `/proc/diskstats`, keeping lines whose device
  name matches a known physical-disk driver prefix — `sd`, `hd`, `vd`,
  `xvd`, `nvme`, or `mmcblk` (see `is_disk_device`) — or only those named
  in `op->disks`, and emits the read/write/io counters selected by
  `op->dkstats`. Emits `PMIX_DISK_RESOURCE_USAGE`.
- **`net_stat`** — reads `/proc/net/dev` (skipping its two header lines),
  splitting on the `:` after the interface name, and emits the
  received/sent byte/packet/error counters selected by `op->netstats`,
  optionally filtered by `op->nets`. Emits `PMIX_NETWORK_RESOURCE_USAGE`.

Values that `/proc` reports in kB are normalized to MB by
`convert_value()` (divides by 1024 when the field is suffixed `kB`).

### The text-parsing helpers

`/proc` files are whitespace-formatted text, so the module carries three
small parsers, all operating on a single static line buffer `input`:

- **`local_getline`** — `fgets` one line into `input`, strip the trailing
  newline, skip leading non-alphanumerics.
- **`local_stripper`** — split a `key: value` line at the colon, trimming
  whitespace, returning the value (and NUL-terminating the key in place).
- **`local_getfields`** — tokenize a line into a `PMIx_Argv` of
  alphanumeric fields. Used for the positional `diskstats` / `net/dev`
  columns.
- **`next_field`** (in `proc_stat`) — advance a pointer past the current
  whitespace-delimited field, used to walk the fixed `/proc/<pid>/stat`
  layout.

## Caveats and known rough edges

These are real observations about the current Linux parsing code; if you
touch this file, be aware of them (and fixing them is welcome as a
standalone commit — do not paper over them):

- **The `disk_stat` / `net_stat` readers index fixed columns.** Both use
  positional columns (disk: device name at `fields[2]`, stats at
  `fields[3]`..`fields[13]`; net: stats at `fields[0]`..`fields[10]`), so
  each guards against a short line before parsing — disk requires at least
  14 fields, net at least 11. Extra trailing columns are ignored, which is
  what lets the readers accept modern kernels' wider lines (the
  discard/flush counters `/proc/diskstats` gained in 4.18+/5.5+, and the
  16 columns `/proc/net/dev` supplies per interface). One real limitation
  remains: the newer disk discard/flush counters are **not** surfaced as
  attributes. (Disk *selection* is no longer SCSI-only — `is_disk_device`
  now matches `sd`/`hd`/`vd`/`xvd`/`nvme`/`mmcblk` device-name prefixes,
  so `nvme*` and other non-SCSI disks are reported. Add a prefix there if
  a driver you care about is missing.)
- **`node_stat` matches `/proc/meminfo` keys with exact `strcmp`** (e.g.
  `strcmp(dptr, "MemTotal")`) against the colon-stripped key produced by
  `local_stripper`. This depends on that stripping being exact; changes to
  the helpers can silently drop fields.
- **Static line buffer.** `input` (and thus `local_getline` /
  `local_stripper`) is a single shared static buffer, so the readers are
  **not** reentrant. That is fine today because everything runs on one
  progress thread, but do not call these from a second thread.
- **The parsers scan for an alphanumeric, so every loop needs its own
  terminator.** `local_getline` skipping leading whitespace,
  `local_stripper` walking back from the colon and forward to the value,
  `local_getfields` tokenizing, `next_field` stepping the
  `/proc/<pid>/stat` columns — each of these searches for a character
  class rather than for the end of the string, and each therefore has to
  test for `'\0'` (and, for the backwards walk, for the start of the
  buffer) explicitly. A line with no alphanumeric in it, a key with an
  empty value, or a `stat` line shorter than the column the caller is
  asking for will otherwise walk straight off the end of a 1 KB static
  buffer. Real `/proc` content does not currently produce any of those
  shapes, which is why this was never seen — the bounds are there so a
  future kernel format, a container's synthesized `/proc`, or a new
  caller cannot turn a parsing surprise into an out-of-bounds read.
  `isalnum`/`isspace` take their argument as an `unsigned char` for the
  same reason: a plain `char` is signed here.
- The `PMIX_PROC_PERCENT_CPU` (`pctcpu`) flag is parsed by the base helper
  but `proc_stat` does not currently compute a %CPU value — computing it
  would require two samples over an interval.
- **`/proc` counters are 64-bit.** `utime`/`stime` are jiffies printed as
  `unsigned long long` — a many-threaded process on a large node passes
  2^31 of them — and the `diskstats` / `net/dev` columns are byte counts
  that pass 4 GB routinely. They are parsed with `strtoull` and
  accumulated at that width; `strtoul` into an `int` (or into a
  `uint64_t` on a 32-bit build) silently wrapped them.
- **A rate without an id gives an un-cancellable monitor.**
  `PMIX_MONITOR_RESOURCE_RATE` is accepted with no `PMIX_MONITOR_ID`, and
  the resulting op goes onto `pmix_pstat_base.ops` with `id == NULL`.
  Nothing can name it in a `PMIX_MONITOR_CANCEL`, so it samples until the
  framework closes. Cancel skips such ops rather than dereferencing the
  NULL, but whether the request should be rejected up front is an open
  question rather than a settled one.
