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

- **`query`**, **`monitor_fields`** and **`update`** are the *same code*
  as `plinux`'s — not merely the same shape. Same cancel fast-path, same
  op construction, same `PMIX_MONITOR_ID` / `PMIX_MONITOR_RESOURCE_RATE`
  parsing and validation, same four monitor-key branches, same peer
  selection against `pmix_server_globals.clients`, same
  synchronous-`op->cb` collection and error reporting, same periodic
  timer arming, and the same `PMIX_RANGE_CUSTOM` notification targeted at
  the requestor.

  A `diff` of the two, ignoring whitespace, should come back empty; that
  is the check to run after touching either. They used to differ, and
  every difference turned out to be a defect rather than a deliberate
  choice — including the one that looked most like a choice. `test` did
  not record the requestor in the op and notified at `PMIX_RANGE_LOCAL`
  without the `PMIX_EVENT_NON_DEFAULT` and `PMIX_EVENT_CUSTOM_RANGE`
  directives, so a periodic monitor's samples went to every event handler
  on the node instead of to the one process that asked for them. A test
  double that answers a different set of processes than the component it
  stands in for is not testing the thing it is standing in for.

  The rules this shared code encodes — the untrusted monitor value, who
  owns the answer list on the synchronous path, why a reader's
  `PMIX_ERR_NOT_FOUND` is skipped rather than fatal, why every
  `PMIx_Info_list_convert()` result is destructed after it is added, and
  why a refused `PMIx_Notify_event` releases its own caddy — are written
  up in the [framework guide](../AGENTS.md) and
  [`plinux/AGENTS.md`](../plinux/AGENTS.md). The canned readers never
  return `PMIX_ERR_NOT_FOUND`, so that branch is unreachable here; it
  stays because divergence is the thing being guarded against.

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
  counters, and honors `op->disks`: a caller naming `sd01` gets `sd01`
  and nothing else, exactly as the real readers behave. (It used to
  ignore the filter and report both, which left the `disks` argv the
  base parse helper builds with no consumer on this path.)
- **`net_stat`** — fabricates **three** interfaces (`net000`..`net002`)
  with fixed counters, and honors `op->nets` the same way.

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

- **Keep it in sync with `plinux`.** Its value as a test double comes
  from executing the *same* control flow, and `query`/`update` are
  currently identical text. If you add a monitor key, a directive, or
  change the op lifecycle in `plinux`, mirror it here — and in `pmacos`,
  which is the third copy — so CI keeps covering the new path. All three
  drifting apart is how the same bug came to be fixed three times.
- **`test/unit/pstat_query` asserts the query contract against whichever
  component the host selects.** Run it under each one
  (`PMIX_MCA_pstat=test ./pstat_query`) after changing any of them; a
  rule that only one component honors is not a rule.
- Keep the emitted values fixed and documented — downstream tests may
  assert on them. If you must change a canned value, grep the test suite
  for callers first.
