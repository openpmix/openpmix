<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSTAT `test` Component

`test` is the canned-data `pstat` component. It answers monitor requests
with fixed, deterministic values instead of reading the operating system,
so the whole `PMIx_Process_monitor` code path can be exercised on
platforms that have no `/proc` (macOS, other Unixes) and in CI where a
stable, reproducible answer is wanted. Read the framework
[`AGENTS.md`](../AGENTS.md) first — this file only covers what is specific
to `test`.

## Files

| File | Contents |
|------|----------|
| `pstat_test.h` | Declares the component and module symbols. |
| `pstat_test_component.c` | Component struct + `pstat_test_component_query`. |
| `pstat_test.c` | The module: `query`, `update`, and the four canned `*_stat` readers. |

There is **no `configure.m4`**: `test` has no OS dependency and is always
built, so it is available as a fallback on every platform.

## Component (`pstat_test_component.c`)

Bare `pmix_pstat_base_component_t`; `pstat_test_component_query` returns
the module at **priority 20**:

```c
*priority = 20;
*module = (pmix_mca_base_module_t *) &pmix_pstat_test_module;
return PMIX_SUCCESS;
```

20 is below `plinux` (80), so on a Linux node with `/proc` the real
component wins and `test` stays dormant. On any host where `plinux` was
not built, `test` is the highest-priority runnable component and is
selected — which is exactly why the monitor API still works (with fake
numbers) on non-Linux systems.

## Module (`pstat_test.c`)

The module is a near-exact structural clone of `plinux`'s module, and
this is deliberate: keeping the two in lock-step means the `test`
component genuinely exercises the same request-dispatch, op-lifecycle,
and `update()` duality that `plinux` uses — only the leaf `*_stat`
readers differ. Concretely:

- **`query`** is line-for-line the same dispatch as `plinux`'s: same
  cancel fast-path, same op construction, same `PMIX_MONITOR_ID` /
  `PMIX_MONITOR_RESOURCE_RATE` parsing, same four monitor-key branches,
  same peer selection against `pmix_server_globals.clients`, same
  synchronous-`op->cb` collection plus optional periodic timer arming.
  (One small divergence: `test`'s `query` does not `memcpy` the requestor
  into the op, since it targets its periodic events at
  `PMIX_RANGE_LOCAL`.)
- **`update`** is the same synchronous-vs-timer engine, with the same
  `nettaken`/`dktaken` node-nesting logic. The only behavioral difference
  from `plinux` is the notification **range**: on the timer path `test`
  calls `PMIx_Notify_event(..., PMIX_RANGE_LOCAL, ...)` rather than a
  custom range to the requestor.

### The canned readers

The four readers have the same signatures and emit the same attributes as
`plinux`'s, but with hard-coded values and no file I/O:

- **`proc_stat`** — for the given peer, emits the real proc ID / pid /
  hostname (those *are* known), then fixed values for each requested
  field: state `"R"`, cmdline `"test-stats"`, a `1234.5678` timeval,
  priority 5, 10 threads, CPU 2, and fixed floats for peak-vsize / vsize /
  RSS / PSS. Tagged with a real `PMIX_PROC_SAMPLE_TIME`.
- **`node_stat`** — fixed load averages and memory/swap figures.
- **`disk_stat`** — fabricates **two** disks (`sd00`, `sd01`) with fixed
  counters; ignores the `disks` filter.
- **`net_stat`** — fabricates **three** interfaces (`net000`..`net002`)
  with fixed counters; ignores the `nets` filter.

Because the values are constants, a test can assert on them exactly.

## Using `test` to exercise the framework

When you change anything in the base or the monitor plumbing and want to
verify it without a Linux node, force selection of this component so the
answer is deterministic:

```sh
# make the server use the test pstat component
export PMIX_MCA_pstat=test
```

Then drive a request through `PMIx_Process_monitor` from a client against
that server. This is the practical way to smoke-test framework-level
changes (op lifecycle, cancel, periodic-rate timers, result marshalling)
on a developer laptop.

## When modifying `test`

- **Keep it in structural sync with `plinux`.** Its value as a test
  double comes from executing the *same* control flow. If you add a
  monitor key, a directive, or change the op lifecycle in `plinux`, mirror
  it here so CI keeps covering the new path.
- Keep the emitted values fixed and documented — downstream tests may
  assert on them. If you must change a canned value, grep the test suite
  for callers first.
