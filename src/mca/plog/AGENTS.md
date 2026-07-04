<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PLOG Framework

This document orients AI agents and human contributors working in the
`plog` (**P**MIx **Log**ging) framework. It assumes you have already read
the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden rules,
prefix conventions, thread-safety model, and MCA concepts described there
all apply here and are not repeated. This file covers what is specific to
`plog`: what the framework is for, how a log request flows through it,
and the contract every component must honor. Each component subdirectory
carries its own `AGENTS.md` with component-specific detail.

## What PLOG does

`plog` implements the back end of the `PMIx_Log` / `PMIx_Log_nb` public
APIs. When a client, tool, or server calls `PMIx_Log`, it supplies an
array of `pmix_info_t` "data" entries — each one a piece of information to
be recorded — plus an array of "directives" that qualify how the logging
should happen. The framework's job is to take that request and route
each data item to whatever *channel* can service it: stdout/stderr,
the local or global syslog, email, and so on.

The key design idea is that **the channel is selected by the key of the
data item, not by the caller naming a component**. A caller does not ask
for "the syslog component"; it hands the framework a
`pmix.log.syslog` = "some message" info entry, and the framework finds
whichever active module has claimed the `syslog` channel. This keeps the
public API stable and lets new logging back ends appear purely as new
components plus new attribute keys — no API change, consistent with the
"prefer a new attribute over a new API" rule in the top-level guide.

`plog` is a **multi-select** framework: several components can be active
at once, each servicing a different (possibly overlapping) set of
channels. A single `PMIx_Log` call carrying both a `stdout` entry and a
`syslog` entry will exercise two different modules in one pass.

## Directory layout

```
src/mca/plog/
├── plog.h                 Framework public API: module & component types
├── base/                  Framework infrastructure (see below)
│   ├── base.h             Internal base API + globals + active-module struct
│   ├── plog_base_frame.c  open/close/register, framework declaration
│   ├── plog_base_select.c Component query + priority/ordering logic
│   ├── plog_base_stubs.c  pmix_plog_base_log() — the routing engine
│   └── help-pmix-plog.txt show_help text for plog errors
├── stdfd/                 stdout/stderr channel component
├── syslog/                local/global syslog channel component
└── smtp/                  email channel component (optional; needs libesmtp)
```

## Core data structures (`plog.h`)

`plog.h` is the framework's public header — the contract every component
compiles against. Read it before anything else. It defines:

- **`pmix_plog_module_t`** — the runtime instance a component hands back.
  Its fields:
  - `char *name` — the module's short name (e.g., `"syslog"`). The name
    `"default"` is special (see selection, below).
  - `char **channels` — a NULL-terminated argv array of the channel
    tokens this module services (e.g., `{"stdout","stderr",NULL}`). A
    `NULL` `channels` pointer is a wildcard meaning "this module handles
    *every* channel" — used by catch-all/default modules.
  - `init` / `finalize` — optional lifecycle hooks
    (`pmix_plog_base_module_init_fn_t` / `_fini_fn_t`).
  - `log` — the workhorse (`pmix_plog_base_module_log_fn_t`); does the
    actual delivery.

- **`pmix_plog_base_module_log_fn_t`** — the log entry-point signature,
  shared verbatim by the framework API and every module:
  ```c
  pmix_status_t (*log)(const pmix_proc_t *source,
                       const pmix_info_t data[], size_t ndata,
                       const pmix_info_t directives[], size_t ndirs);
  ```

- **`pmix_plog_API_module_t`** / the global **`pmix_plog`** — a
  one-field struct exposing `pmix_plog.log`, which points at
  `pmix_plog_base_log`. This is the single entry point callers outside
  the framework use (see `src/common/pmix_log.c` and
  `src/server/pmix_server_ops.c`). Back-end code calls `pmix_plog.log(...)`,
  never a component's `log` directly.

- **`pmix_plog_base_component_t`** — just a typedef of the standard
  `pmix_mca_base_component_t`. Simple components (like `stdfd`) use it
  as-is; components needing configuration state (`syslog`, `smtp`) embed
  it as a `super` field in a larger struct.

- **`PMIX_PLOG_BASE_VERSION_1_0_0`** — the version macro every component's
  component struct must open with.

## How a log request flows

1. **Public API.** `PMIx_Log` / `PMIx_Log_nb` (in
   [`src/common/pmix_log.c`](../../common/pmix_log.c)) validate the call,
   thread-shift onto the progress thread, and — for the path that runs
   the request locally — invoke `pmix_plog.log(...)`, i.e.
   `pmix_plog_base_log()`. (A client typically forwards the request to
   its server first; the server, or a server/tool logging locally, ends
   up in `pmix_plog_base_log`. See the note on threading below.)

2. **Routing** — `pmix_plog_base_log()` in `plog_base_stubs.c`. This is
   the heart of the framework; details in the next section.

3. **Delivery** — each selected module's `log` function receives the
   *entire* `data`/`directives` arrays (not just the item that matched)
   and scans them for the keys it knows how to handle, delivering each
   and marking it complete.

### The routing engine: `pmix_plog_base_log()`

Trace [`base/plog_base_stubs.c`](base/plog_base_stubs.c) carefully — most
plog bugs live in the interaction between this function and the modules.
Its algorithm:

1. **Global directives first.** Scan `directives` for:
   - `PMIX_LOG_ONCE` — stop after the first channel that successfully
     handles the request (`logonce`).
   - `PMIX_LOG_AGG`, `PMIX_LOG_KEY`, `PMIX_LOG_VAL` — de-duplication of
     `show_help`-style messages. When aggregation is on and this
     key/val pair has been seen before (`pmix_help_check_dups`), every
     data item is marked complete via `PMIX_INFO_OP_COMPLETED` so nothing
     is logged twice.

2. **Assemble the channel list in request order.** For each *data* item
   not already complete, walk the priority-ordered `pmix_plog_globals.actives`
   array and add any module whose `channels` matches the item's key
   (matched with `strstr` against each channel token; a `NULL` `channels`
   is a wildcard that always matches). Modules are appended in the order
   the *data* is presented, so the caller's first item gets first shot —
   this is what makes `PMIX_LOG_ONCE` deterministic. The `added` flag on
   the active-module struct prevents a module being listed twice.

3. **Deliver.** Reset the `added` markers, then call each listed
   module's `log`. Interpret the return code:
   - `PMIX_SUCCESS` / `PMIX_OPERATION_SUCCEEDED` — handled; if `logonce`,
     stop here.
   - `PMIX_ERR_NOT_AVAILABLE` / `PMIX_ERR_TAKE_NEXT_OPTION` — this module
     declined; continue to the next.
   - anything else — a real error; abort the loop and return it.

4. **Nothing to do?** If no module matched, return
   `PMIX_OPERATION_SUCCEEDED` (**not** `PMIX_SUCCESS`). This distinction
   matters: `PMIX_SUCCESS` tells the caller a callback will fire later,
   while `PMIX_OPERATION_SUCCEEDED` means "done synchronously, no callback
   coming." Returning the wrong one here will hang the caller or double-fire.

**Completion tracking is shared, mutable state on the caller's arrays.**
Modules mark individual items done with `PMIX_INFO_OP_COMPLETED(&data[n])`,
and both the router and the other modules honor `PMIX_INFO_OP_IS_COMPLETE`.
This is how two modules cooperate on one array without double-logging.
When you write a module, mark each item you consume complete, and skip
items already marked.

### Return-code contract for modules (important)

A module's `log` must return one of:

- `PMIX_SUCCESS` / `PMIX_OPERATION_SUCCEEDED` — "I handled at least part
  of this request."
- `PMIX_ERR_TAKE_NEXT_OPTION` or `PMIX_ERR_NOT_AVAILABLE` — "none of these
  keys are mine; let another module try." Returning this (rather than an
  error) when your keys are absent is what lets the multi-select chain and
  `PMIX_LOG_ONCE` work correctly.
- a hard error code — only for genuine failures; it aborts the whole
  routing loop.

Getting this wrong is the most common plog defect: an over-eager module
that returns `PMIX_SUCCESS` for a request it did not actually handle will
swallow a `PMIX_LOG_ONCE` request and starve the channel the caller
actually wanted.

## Base infrastructure in detail

### Globals and lifecycle (`plog_base_frame.c`)

- **`pmix_plog_globals`** (`pmix_plog_globals_t`, declared in `base.h`):
  - `actives` — a `pmix_pointer_array_t` of
    `pmix_plog_base_active_module_t`, the selected modules in priority /
    requested order. This is the array the router walks.
  - `initialized` / `selected` — guard flags making open and select
    idempotent.
  - `channels` — the parsed `plog_base_order` MCA parameter (see below),
    or `NULL` if the user gave no explicit ordering.
- **`pmix_plog_open`** constructs `actives` and opens all components.
- **`pmix_plog_close`** finalizes each active module (calling its
  `finalize`), releases the active-module wrappers, and closes components.
- **`pmix_plog_register`** registers the one framework-level MCA
  parameter, **`pmix_plog_base_order`**: a comma-delimited, prioritized
  list of channel names controlling both *which* channels are used and in
  *what* order. It is split into `pmix_plog_globals.channels` here.

### The active-module wrapper (`base.h`)

`pmix_plog_base_active_module_t` wraps each selected module with
bookkeeping the router and selector need:

| Field | Purpose |
|-------|---------|
| `super` | `pmix_list_item_t` — so it can live on a list during selection |
| `reqd` | this channel was marked "required" via the `:req` modifier |
| `added` | transient flag: already appended to the current routing list |
| `pri` | selection priority returned by the component's query |
| `module` | the `pmix_plog_module_t` the component handed back |
| `component` | back-pointer to the owning component |

### Selection and ordering (`plog_base_select.c`)

`pmix_plog_base_select()` runs once at init (guarded by
`pmix_plog_globals.selected`). It:

1. Queries every component's `pmix_mca_query_component`; skips those with
   no query function or that return no module.
2. Calls each returned module's `init`; drops the module if `init` fails.
3. Inserts surviving modules into a temporary list **in descending
   priority order**, remembering the module named `"default"` if present.
4. **If the user set `plog_base_order`:** walk the requested channel names
   in order, moving the matching module from the temp list into the global
   `actives` array (so user order overrides priority order). A channel
   name may carry a `:req` (or `:required`) suffix — parsed here — marking
   it mandatory. If a requested channel has no matching module, the
   `"default"` module is substituted; if there is no default *and* the
   channel was required, selection fails with the `reqd-not-found`
   `show_help` message and `PMIX_ERR_NOT_FOUND`. Unrequested leftover
   modules are discarded.
5. **If the user set no order:** all modules go into `actives` in pure
   priority order.

At high verbosity (>4) it prints the final resolved order — a useful
debugging hook (`pmix_plog_base_framework.framework_output`).

## Threading

`pmix_plog_base_log()` "takes place in a progress event" — it and the
modules it invokes run **on the progress thread**, reached via the
thread-shift performed by the public `PMIx_Log_nb` path. Do not call
`pmix_plog.log` from an arbitrary user thread without thread-shifting
first; follow the caddy pattern in the top-level guide. Individual
modules may themselves thread-shift outward again when they hand work to
another subsystem — `stdfd`, for example, calls the non-blocking
`PMIx_server_IOF_deliver` and lets a callback release its scratch object
rather than blocking the progress thread.

## Building

The framework core (`base/`) is always built into `libpmix`. Each
component builds as a standard MCA component — either a DSO
(`pmix_mca_plog_<name>.la`) or a static archive
(`libpmix_mca_plog_<name>.la`) depending on the build configuration.
`smtp` and `syslog` ship a `configure.m4` that can disable the component
when its dependency (libesmtp / `syslog.h`) is absent; `stdfd` has no
optional dependency and is always available. Adding a component means
creating `src/mca/plog/<name>/` with the usual `Makefile.am`, a component
struct, and a module — the framework picks it up through the generated
`base/static-components.h`; no core code changes are needed.

Remember the top-level golden rules that bite here specifically:

- If you touch any text in `base/help-pmix-plog.txt` (or add a new
  `show_help` message), you must regenerate the compiled `show_help`
  content: `rm src/util/pmix_show_help_content.*` then `make`.
- Editing a `Makefile.am` only needs a plain `make`; adding/removing a
  *component directory* changes the build wiring resolved by `configure`,
  so re-run `./autogen.pl && ./configure ... && make`.

## When adding or modifying a component

- Open the component struct with `PMIX_PLOG_BASE_VERSION_1_0_0` and set
  `pmix_mca_component_name` to your component's directory name.
- Provide a `query` function returning your `pmix_plog_module_t` and a
  priority. Return no module (or a failure) when the component cannot run
  in this environment (missing server, unresolved host, etc.) — see
  `smtp`'s query, which resolves the SMTP server up front and disables
  itself if that fails.
- In your module's `log`, honor the return-code contract above, mark
  consumed items complete, and skip already-complete items.
- Claim your channel tokens by populating `module->channels` (usually in
  `init`), or set it to `NULL` only if you truly are a catch-all.
- Document any new `PMIX_LOG_*` attribute keys in
  [`include/pmix_common.h.in`](../../../include/pmix_common.h.in) and the
  user docs, per the attribute rules in the top-level guide.
