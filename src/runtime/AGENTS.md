# AGENTS.md: The PMIx Runtime / Bootstrap Layer

This document orients AI agents and human contributors working in
`src/runtime`, the code that brings `libpmix` up and tears it back down.
It assumes you have already read the top-level
[`AGENTS.md`](../../AGENTS.md) — the golden rules (prefix conventions,
`pmix_config.h`-first include order, constant-on-the-left comparisons,
brace-everything, `#define`-logical-macros-to-0/1, warning-free under
`--enable-devel-check`), the **thread-safety / progress-thread model and
the caddy pattern**, the "prefer an MCA parameter over a hard-coded
constant" rule, the `show_help` regeneration rule, and the
copyright-header requirement all apply here and are not repeated. This
file covers what is specific to `src/runtime`: the init/finalize
ordering contract, the global-state object it owns, the MCA-parameter
registry, the progress-thread engine, and the `pmix_info` support
library.

Like `src/client` and `src/event`, **`src/runtime` is not an MCA
framework.** There is no component structure — just five `.c` files and
four headers compiled straight into `libpmix` via `Makefile.include`
(which appends to the top-level `sources`/`headers` lists; there is no
`Makefile.am` here). A change takes effect with a plain top-level
`make` from an already-configured tree; you need
`autogen.pl`/`configure` only if you add or remove a source file. The
code is *role-shared*: `pmix_rte_init` runs inside clients, servers,
tools, and launchers, differentiated only by the `type` argument and a
handful of incoming directives.

## What this directory is

This is the layer every PMIx process passes through exactly once on the
way in and once on the way out. It owns:

- **Library bring-up / tear-down** (`pmix_init.c` / `pmix_finalize.c`)
  — `pmix_rte_init` and `pmix_rte_finalize`, plus the "util" sub-layer
  (`pmix_init_util` / `pmix_finalize_util`) used by the standalone
  tools that need MCA/params but not a full client. Also defines and
  statically initializes the process-wide `pmix_globals` object.
- **MCA parameter registration** (`pmix_params.c`) —
  `pmix_register_params` / `pmix_deregister_params`: every
  `pmix`-project MCA var that is not owned by a specific framework
  (client/server verbosity, IOF options, event caching, hostname
  handling, progress-thread binding, `pmix_info` colors, …).
- **The progress-thread engine** (`pmix_progress_threads.c`) — the
  libevent event-base + dedicated-thread machinery that *is* the PMIx
  progress thread, reference-counted and addressable by name. Backs the
  public `PMIx_Progress` and `PMIx_Progress_thread_stop` APIs.
- **The `pmix_info` support library** (`pmix_info_support.c`) — all the
  formatting/enumeration logic behind the `pmix_info` executable
  (`src/tools/pmix_info/`) and, in part, the other CLI tools: MCA-var
  dumping, component/version display, install-path display, config
  dump, and the pretty/parsable output formatter.

Almost all the *state* this directory manipulates lives in
`pmix_globals` (`src/include/pmix_globals.h`) and
`pmix_client_globals`/`pmix_server_globals`, not in file-scope
variables here. The MCA-param C globals (e.g. `pmix_keep_fqdn_hostnames`,
`pmix_maxfd`, `pmix_progress_thread_cpus`) are declared `extern` in
[`pmix_rte.h`](pmix_rte.h) and defined in `pmix_params.c`.

## The init / finalize ordering contract

`pmix_rte_init(type, info, ninfo, cbfunc)` is called once from each
role's own init: `pmix_client.c`, `pmix_server.c`, `pmix_tool.c`. The
order of operations is load-bearing — later subsystems depend on
earlier ones being open, and `pmix_rte_finalize` must unwind in a
compatible order. The spine is:

1. **`pmix_init_util`** (idempotent, latched on
   `pmix_globals.util_initialized`): output → pinstalldirs →
   show_help → keyval parser → `mca_base_var` → `mca_base_open` →
   `pmix_net_init` → `pif`. Standalone tools call this directly instead
   of `pmix_rte_init`. (TSD keys created later, e.g. in `pmix_net_init`,
   register themselves for finalize-time cleanup regardless of which
   thread creates them — see `src/threads/thread.c`.)
2. **`pmix_register_params`** — must come before anything reads an MCA
   value.
3. **Directive scan** — walk the incoming `info[]` for the small set of
   bootstrap directives (`PMIX_HOSTNAME`, `PMIX_NODEID`,
   `PMIX_NODE_INFO_ARRAY`, `PMIX_EXTERNAL_PROGRESS`,
   `PMIX_EXTERNAL_AUX_EVENT_BASE`, `PMIX_HOSTNAME_KEEP_FQDN`,
   `PMIX_BIND_PROGRESS_THREAD`, `PMIX_BIND_REQUIRED`); everything else
   is handed to `pmix_iof_check_flags`.
4. **Progress thread + event base** (`pmix_progress_thread_init(NULL)`)
   — creates `pmix_globals.evbase`. If no external aux event base was
   supplied, `evauxbase` is aliased to `evbase`.
5. **Globals construction** — ids, `mypeer` (a `pmix_peer_t` stamped
   with our version), the events/notifications/nspaces/keyindex
   structures, client-globals lists, verbosity output channels, uid/gid,
   `PMIX_DEBUG` channel, hostname/aliases.
6. **Framework open+select, in dependency order**: bfrops → pcompress →
   ptl → psec → gds → preg (preg **must** follow pcompress) → plog.
   `pmix_init_registered_attrs()` then builds the attribute registry.
7. **`pmix_progress_thread_start(NULL)`** — actually spins the thread
   (skipped when `external_progress` is set; the host drives progress).

`pmix_rte_finalize` (guarded by `pmix_init_called`) unwinds:
`progress_thread_pause` (stop the loop **without** freeing the base, so
components can still finalize cleanly) → release attrs → close plog,
preg, ptl, psec, bfrops, pcompress, gds → `pmix_net_finalize` →
deregister/finalize params → keyval → pinstalldirs, pif, `mca_base_close`
→ show_help → output → destruct the `pmix_globals` members → hwloc
finalize → TSD key destruct → `pmix_name_fns_finalize` →
`pmix_finalize_util` → **`pmix_progress_thread_stop(NULL)`** (this is
what finally frees the event base) → NULL out `evbase`/`evauxbase`.

**Re-entrancy is a first-class requirement here.** A process may call
`PMIx_Init`/`PMIx_Finalize` (or the tool/server equivalents) more than
once in its lifetime, and recent history in this directory is almost
entirely about making the *second* init start from a clean slate (see
`git log` — "Cleanup init/finalize cycle", "fix a problem after second
pmix init", "Do not shutdown libevent during finalize", "runtime: leave
no residue or dangling state after finalize"). Consequences you must
preserve when editing:

- **Do not destroy libevent's global thread state on finalize.** The
  loop is paused and the base is freed, but the library is left able to
  come back up. Freeing more aggressively re-introduces the
  second-init crashes these commits fixed.
- **Every pointer cached across the gap must be NULLed on finalize** so
  the next init does not dereference freed memory —
  `evbase`/`evauxbase` here, `shared_thread_tracker` in
  `pmix_progress_threads.c`, the print-buffer TSD latch cleared by
  `pmix_name_fns_finalize`. If you cache a new pointer at init, clear it
  at finalize.
- **`evauxbase` ownership is conditional.** It either aliases `evbase`
  (we own it) or was supplied by the caller via
  `PMIX_EXTERNAL_AUX_EVENT_BASE` (they own it). Finalize only drops the
  reference; it must never `free` it. Consumers use it for
  signal/SIGCHLD events (`src/common/pmix_pfexec.c`,
  `src/common/pmix_iof.c`, `src/tool/pmix_tool.c`).

## `pmix_globals`: the process-wide state object

`pmix_init.c` defines `pmix_globals` with a large designated-initializer
(the `*_STATIC_INIT` macros). This object is `PMIX_EXPORT`ed because
**every plugin links against it** — it is the shared spine of the whole
library. Rules:

- The static initializer, the runtime construction in `pmix_rte_init`
  (steps 5–6 above), and the teardown in `pmix_rte_finalize` are **three
  views of the same field list** and must stay in agreement. If you add
  a field to `pmix_globals_t`, initialize it statically, construct it in
  init if it needs it, and destruct/free/NULL it in finalize. A field
  constructed but not destructed leaks on every finalize; one destructed
  but not re-constructed crashes on second init.
- The finalize path deliberately drains the notification hotel
  room-by-room and the `iof_requests` pointer array item-by-item before
  destructing the containers — mirror that discipline for any new
  container-of-refcounted-objects you add.

## MCA parameters (`pmix_params.c`)

`pmix_register_params` is latched by the file-scope `pmix_register_done`
and registers all the non-framework `pmix`-project vars. Patterns to
follow:

- Set the C global to its default **before** the
  `pmix_mca_base_var_register` call (the register call overwrites it
  from the environment/file/CLI if the user set it). Add new params next
  to their thematic neighbors (client verbosity, server verbosity, IOF,
  event, hostname, …).
- Verbosity params write directly into `pmix_client_globals.*_verbose` /
  `pmix_server_globals.*_verbose`; `pmix_rte_init` later opens an output
  channel for each nonzero one. If you add a verbosity var, add the
  matching `pmix_output_open` block in `pmix_init.c`.
- `pmix_deregister_params` intentionally does **not** free the
  registered vars (the `mca_base_var` system frees them in
  `pmix_mca_base_var_finalize`); it only resets the latch so a second
  init re-registers cleanly. The one thing that *is* hand-freed is
  `pmix_var_dump_color[]`, released in `pmix_rte_finalize`.
- The `var_dump_color` machinery (`parse_color_string`) turns a
  `key=ansi_code` list into ready-to-emit `\033[..m` escape strings used
  by `pmix_info`. It reports malformed tokens via `help-pmix-runtime.txt`
  and, on any allocation failure, frees the whole partial result.

Help text for this directory lives in
[`help-pmix-runtime.txt`](help-pmix-runtime.txt). Per the top-level
rule, after editing any `show_help` content you must
`rm src/util/pmix_show_help_content.* && make` so the compiled-in copy
is regenerated.

## The progress-thread engine (`pmix_progress_threads.c`)

A `pmix_progress_tracker_t` wraps one libevent base + one OS thread,
kept on a file-scope `tracking` list and identified by name. `NULL`
means the shared **"PMIX-wide async progress thread"** — the one
`pmix_rte_init` creates and that `pmix_globals.evbase` points at.
Frameworks that need their own thread pass a name
(`pstat` → `"PSTAT"`, `psensor` → `"PSENSOR"`).

Lifecycle primitives, and how they differ (this is the easy-to-confuse
part):

| Function | Refcount | Event base | Thread |
|----------|----------|------------|--------|
| `pmix_progress_thread_init` | +1 (creates if absent) | creates | — |
| `pmix_progress_thread_start` | — | — | spins the engine |
| `pmix_progress_thread_pause` | — | kept | stops the loop, keeps base |
| `pmix_progress_thread_resume` | — | kept | re-spins (errors if active) |
| `pmix_progress_thread_stop` | −1; frees at 0 | freed at 0 | joined at 0 |

Key facts:

- **The base always carries a persistent "block" timer** (`dummy_timeout_cb`,
  1-hour re-arm) so `pmix_event_loop(..., ONCE)` never returns just
  because the base went empty.
- Each base keeps something to block on via that timer; the engine loop
  runs while `trk->ev_active` is set and exits after
  `pmix_event_base_loopexit`.
- `stop_progress_engine` sets/clears `pmix_globals.progress_thread_stopped`
  for the *shared* thread only. That flag gates whether PMIx APIs may
  still thread-shift work in — code that posts to the progress thread
  checks it to avoid enqueuing onto a dead loop.
- The public **`PMIx_Progress_thread_stop`** (2-arg, in `pmix.h`) is
  *not* the same as internal `pmix_progress_thread_stop` (1-arg name).
  The public one parses `PMIX_PROGRESS_THREAD_NAME` /
  `PMIX_PROGRESS_THREAD_FLUSH` directives, optionally drains the base
  with a marker event, and stops the engine **without** touching the
  refcount or the tracking list. `PMIx_Progress` runs a single
  non-blocking loop pass on the shared base (used by hosts that drive
  progress themselves).
- The shared-thread teardown in `pmix_rte_finalize` uses
  `pause` (early) then `stop` (last).

## The `pmix_info` support library (`pmix_info_support.c`)

This is the largest file and is almost entirely presentation logic for
the `pmix_info` tool and its siblings (`src/tools/*`). It has its own
lightweight init/finalize (`pmix_info_init` / `pmix_info_finalize`)
distinct from `pmix_rte_init` — a tool wants MCA params and framework
registration but not a running progress thread or a connected peer.
Notable pieces:

- **`pmix_info_out`** is the shared formatter. It supports a
  center-aligned "pretty" mode (with terminal-width wrapping and
  ANSI-color-aware line-length accounting) and a `key:value` "parsable"
  mode (which quote-escapes values containing `:` or `"`). Almost every
  other function funnels through it.
- **`pmix_info_register_framework_params`** /
  **`pmix_info_close_components`** are refcounted
  (`pmix_info_registered`) so multiple tools can share the registration.
- Enumeration walks `pmix_frameworks[]` (generated by autogen, from
  `src/include/pmix_frameworks.h`) and the `component_map` built by
  `pmix_info_register_project_frameworks`.
- Because these run in a short-lived CLI process, several functions
  `exit()` directly on user error rather than returning, and cleanup is
  deliberately minimal. Do not copy that style into library code.

## Threading notes specific to this directory

- `pmix_init_util` guards itself with an **atomic** bool
  (`pmix_atomic_check_bool`/`set_bool`) because tools can race into it;
  the rest of init assumes single-threaded bring-up (the progress
  thread does not exist yet for most of it, and is paused first thing in
  finalize).
- Everything after `pmix_progress_thread_start` and before
  `pmix_progress_thread_pause` is the normal PMIx world where the
  progress-thread / caddy rules from the top-level guide apply. Init and
  finalize themselves run on the main thread.

## Building and testing

Plain top-level `make` picks up edits here (no `configure` needed unless
you add/remove a file). There is **no dedicated unit test** for
`src/runtime` — every `make check` test and every `./simptest` run
exercises init/finalize implicitly, and the re-entrancy fixes were
validated with init→finalize→init cycles. If you change the
init/finalize contract, an explicit double-cycle smoke test is the
right thing to add under `test/unit`. Do not diagnose functional
failures against an `--enable-test-build` tree (see the top-level
guide).

## Known rough spots

Structural quirks worth knowing before you touch the relevant code:

- **Init failure aborts rather than unwinds.** On any error after the
  early stages, `pmix_rte_init` jumps to `return_error`, emits a
  `show_help` diagnostic, frees the transient IOF-flag strings, and
  returns — it deliberately does **not** tear down the frameworks,
  globals, or event base it already brought up, because a failed
  `PMIx_Init` aborts the process. Do not mistake this for a leak to
  "fix" on the error path; if you add a new failure point, follow the
  same pattern (set `error` to a short label and `goto return_error`).

## When modifying code here

- **Keep the three views of `pmix_globals` in sync** (static init,
  `pmix_rte_init` construction, `pmix_rte_finalize` teardown). This is
  the single most common way to break this directory.
- **Preserve re-entrancy.** After your change, a
  init→finalize→init→finalize cycle must run clean (no leak, no crash,
  no use of a freed base). NULL every pointer you cache across finalize.
- **Respect the framework open/select order** in `pmix_rte_init` and the
  reverse-ish close order in `pmix_rte_finalize`; the preg-after-pcompress
  dependency is real.
- **Default an MCA param before registering it**, and add the matching
  output-channel open if it is a verbosity var.
- **Regenerate `show_help`** after touching `help-pmix-runtime.txt`.
- Library code must not adopt the `pmix_info` tool's `exit()`-on-error /
  minimal-cleanup style.
