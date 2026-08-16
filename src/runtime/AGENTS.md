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
   is handed to `pmix_iof_check_flags`. Two things follow from that.
   First, these values come straight from the host and nothing has
   validated them yet, so check `value.type` before following a union
   member — `PMIX_CHECK_KEY` says what the key is, never what is in the
   value. Second, `pmix_iof_check_flags` **allocates**: it `strdup`s
   `PMIX_IOF_OUTPUT_TO_FILE` / `_TO_DIRECTORY` into the local flag block,
   which is then copied wholesale into `pmix_globals.iof_flags`. That
   copy is a bare struct member with no destructor behind it (only the
   per-namespace `pmix_iof_flags_t` gets one), so `pmix_rte_finalize`
   owns those two strings.
4. **Progress thread + event base** (`pmix_progress_thread_init(NULL)`)
   — creates `pmix_globals.evbase`. If no external aux event base was
   supplied, `evauxbase` is aliased to `evbase`.
5. **Globals construction** — ids, `mypeer` (a `pmix_peer_t` stamped
   with our version), the events/notifications/nspaces/keyindex
   structures, client-globals lists, verbosity output channels, uid/gid,
   `PMIX_DEBUG` channel, hostname/aliases. The hostname is resolved from
   the first of: the `PMIX_HOSTNAME` directive, the `PMIX_HOSTNAME`
   environment variable, `gethostname()` — but `pmix_set_aliases` then
   runs **regardless of which** supplied it. Every *other* node's name
   goes through `pmix_set_aliases` too (see `gds/hash`), so skipping it
   for our own leaves `pmix_check_local()` unable to match the form of
   our name we did not keep.
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
- **The scalars need resetting too, not just the containers.** The
  containers and pointers are obvious; the `bool`s and `uint32_t`s are
  the ones that get missed, because forgetting them costs nothing until
  a *second* init. Nothing re-initializes them on the way in —
  `pmix_rte_init` writes them only when the matching directive is
  present — so whatever cycle N was told becomes cycle N+1's default.
  `pmix_rte_finalize` therefore restores them to their static-init
  values at the very end. Two are worth knowing by name:
  `external_progress` left set makes the next
  `pmix_progress_thread_start` decline to spin the engine, so if that
  cycle's host is not driving the loop the first blocking call hangs;
  and `external_topology` left set makes `pmix_hwloc_finalize` decline
  to destroy the next cycle's self-discovered topology. `nodeid` and
  `sessionid` reset to `UINT32_MAX`, which is the "not told" sentinel.
  The reset must come **after** the progress-thread teardown, which
  reads `external_progress`.
- The finalize path deliberately drains the notification hotel
  room-by-room and the `iof_requests` pointer array item-by-item before
  destructing the containers — mirror that discipline for any new
  container-of-refcounted-objects you add.

### Two things in `pmix_globals` this directory does *not* finish

Both look like violations of the rule above. Neither is. Read this
before "fixing" either one — each has cost a debugging session.

- **`mypeer` carries two references, and this file only drops one.**
  `pmix_rte_init` creates it (refcount 1) and the role then retains it
  when it points `pmix_client_globals.myserver` at the same object
  (`pmix_server.c`, `pmix_client.c`, `pmix_tool.c`). `pmix_rte_finalize`
  drops the first; the role drops the second immediately after
  `pmix_rte_finalize()` returns, and *that* is what frees it. So
  **`PMIX_RELEASE` here does not free, and the pointer must not be
  NULLed** — every role guards its release with
  `if (NULL != pmix_globals.mypeer)`, so clearing it cancels the release
  that does the freeing and leaks the peer and its namespace (~3KB) on
  every cycle. Note also that `PMIX_RELEASE` never clears the pointer it
  is handed; only the magic id.
- **The `iof_stdout` / `iof_stderr` sinks are constructed here and
  destructed by each role.** That split is not tidy, but it is forced:
  `iof_sink_destruct` reads `pmix_globals.mypeer` (released above) and,
  in a server, walks `pmix_server_globals.iof_residuals` (destructed in
  `PMIx_server_finalize`). Moving the destruct into `pmix_rte_finalize`
  segfaults the client tests. If you add a role, destruct them in its
  finalize; if you add state with the same shape, expect the same
  constraint.

### The consequence: an object can outlive the base its events are on

The late release above happens *after* `pmix_progress_thread_stop(NULL)`
has freed the event base, and a peer owns two libevent events — as does
everything the peer reaches on its way out: its namespace, that
namespace's IOF sinks, and each sink's write event. `event_del()` on any
of them then takes the lock of a base that has been handed back to the
allocator.

Nothing is being deleted at that point (libevent tore every event off the
base when it freed it), so the call is pure hazard: whatever has since
been written over the lock decides what happens. Usually the bytes still
read as an unheld mutex and nobody notices — which is why this survived
for so long — and when they do not, the process blocks forever on a mutex
no thread will release, with no output and no clue. It presents as a hang
that comes and goes with unrelated changes elsewhere in the library,
because what moves is the heap, not the code.

`pmix_event_del()` is therefore not `event_del()`: it routes through
`pmix_event_del_checked()` in
[`src/include/pmix_globals.c`](../include/pmix_globals.c), which declines
once both `evbase` and `evauxbase` are NULL. **Do not "simplify" that
back into a direct call**, and note that the two pointers being NULL is
the only in-scope signal that the base is gone — which is part of why
`pmix_rte_finalize` NULLs them rather than leaving them dangling.

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
  matching `pmix_output_open` block in `pmix_init.c` **and the matching
  `close_output` in `pmix_finalize.c`.** The close is not optional: an
  output descriptor owns a `strdup`ed prefix that only
  `pmix_output_close` frees, the next `pmix_output_init` marks every slot
  unused without clearing what it pointed at, and the id lives in a
  file-scope struct that outlives the library — so an unclosed channel
  both leaks on every init/finalize cycle and leaves an id naming a slot
  the next cycle may hand to someone else. It also has to happen *before*
  `pmix_output_finalize`, because `pmix_output_close` is a no-op once the
  output system is down.
- `pmix_deregister_params` intentionally does **not** free the
  registered vars (the `mca_base_var` system frees them in
  `pmix_mca_base_var_finalize` — `var_destructor` frees each string var
  through its *storage* pointer and NULLs it, which is also why the
  `free()`+`strdup()` the directive scan does to
  `pmix_progress_thread_cpus` is safe). It resets the latch so a second
  init re-registers cleanly, and frees the one thing that really is ours:
  the `pmix_var_dump_color[]` escapes built by `parse_color_string`.
- The `var_dump_color` machinery (`parse_color_string`) turns a
  `key=ansi_code` list into ready-to-emit `\033[..m` escape strings used
  by `pmix_info`. A malformed or unrecognized entry is reported via
  `help-pmix-runtime.txt` and skipped — the rest of the list still
  applies — and on any allocation failure the whole partial result is
  freed. Every slot always ends up holding a free-able string (`""` when
  no entry named it), so callers can emit it unconditionally.
- Assigning a string literal to a `char *` C global right before
  registering it is the established idiom here (`pmix_net_private_ipv4`,
  `pmix_var_dump_color_string`) and is safe: `register_variable`
  immediately replaces the storage with `strdup` of it. Do not "fix" it
  by heap-allocating the default.

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
  checks it to avoid enqueuing onto a dead loop. Roughly a hundred call
  sites across `src/client`, `src/server`, `src/common` and `src/event`
  read it, so anything that leaves it stale is felt library-wide.
- **`external_progress` suppresses the *engine*, never the bookkeeping.**
  When the host drives our base itself there is no thread to spin, join,
  or pause — but `start`/`pause`/`resume` must still move
  `progress_thread_stopped`, and `stop` must still drop the refcount and
  free the base. An early-out that skips those leaves the shared tracker
  and its base alive past `pmix_rte_finalize`, with the flag still
  reading "running", so every API guard above believes a torn-down
  library is open for work.
- `PMIX_NEW` does **not** zero the object it allocates (see
  `pmix_obj_new_tma`), so the tracker's embedded `block` event is garbage
  until `pmix_event_assign` runs. `block_assigned` records that, and the
  destructor consults it — a tracker released on an allocation-failure
  path would otherwise hand libevent uninitialized memory.
- The CPU list behind `pmix_progress_thread_cpus` /
  `PMIX_BIND_PROGRESS_THREAD` is user input and is validated
  (`parse_cpu_range`) before any of it reaches `CPU_SET`. `strtoul` alone
  cannot do this: it reports 0 for a token with no digits, so a typo used
  to become "bind to cpu 0" silently, and it returns values past
  `CPU_SETSIZE`, which `CPU_SET` drops on glibc/musl but writes out of
  bounds for on BSD. Rejected entries are reported through
  `help-pmix-runtime.txt` and skipped.
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

- `pmix_init_util` latches on an **atomic** bool
  (`pmix_atomic_check_bool`/`set_bool`). Note what that does and does
  not buy you: those macros are a plain `atomic_load`/`atomic_store`, so
  the latch makes the *repeat* call cheap and race-free to read, but it
  is a check-then-set, not a compare-and-swap — two threads arriving
  together would both run the body. Bring-up is expected to be
  single-threaded; the progress thread does not exist yet for most of
  init, and is paused first thing in finalize.
- Everything after `pmix_progress_thread_start` and before
  `pmix_progress_thread_pause` is the normal PMIx world where the
  progress-thread / caddy rules from the top-level guide apply. Init and
  finalize themselves run on the main thread.

## Building and testing

Plain top-level `make` picks up edits here (no `configure` needed unless
you add/remove a file). Do not diagnose functional failures against an
`--enable-test-build` tree (see the top-level guide).

Beyond the implicit coverage every `make check` test and every
`./simptest` run gives init/finalize, four `test/unit` programs target
this directory directly:

| Test | Covers |
|------|--------|
| [`runtime_init`](../../test/unit/runtime_init.c) | hostname/alias resolution from a host-supplied `PMIX_HOSTNAME`, `pmix_set_aliases` under both FQDN policies, `var_dump_color` parsing, and that every `show_help` topic `src/runtime` names actually exists |
| [`progress_threads`](../../test/unit/progress_threads.c) | the whole name-addressed engine — refcounting, distinct bases, start/pause/resume/stop, CPU-list validation, and the blocking-call-from-the-progress-thread guard |
| [`info_support`](../../test/unit/info_support.c) | `pmix_info_make_version_str` and the parsable `pmix_info_out` formatter |
| [`client_cycle`](../../test/unit/client_cycle.c) / [`tool_cycle`](../../test/unit/tool_cycle.c) | the re-entrancy contract — many init→finalize→init cycles in one process |

Two notes for anyone adding to these:

- **Cover a new `show_help` topic in `runtime_init`.** A missing topic is
  invisible until the rare path that emits it fires, and then the user
  gets "I couldn't find that help reference" instead of the diagnostic.
  `pmix_show_help_string` returns `NULL` for a topic that is not in the
  file, so one line asserting non-`NULL` — with the same arguments the
  real call site passes — keeps code and help text in step.
- **The affinity path is Linux/BSD-only.** Everything inside
  `#ifdef HAVE_PTHREAD_SETAFFINITY_NP` compiles out on macOS, so the
  `cpulist:` cases in `progress_threads` report `SKIP` there and must be
  exercised on Linux before you trust a change to `parse_cpu_range`.

That last point is what
[`contrib/dockerswarm/run-runtime-tests.sh`](../../contrib/dockerswarm/run-runtime-tests.sh)
exists for (README §18). It runs all five programs on Linux in **both**
`--enable-debug` and `--disable-debug` — the asserts in
`start_progress_engine` and `pmix_info_close_components` only exist in the
first, and users only ever get the second — then drives the CPU list
through a real `PMIx_Init` (the unit test can only reach *named* progress
threads; the shared one is configured from `pmix_rte_init`), and finally
resolves peers across four nodes to guard the alias change end to end.
Run it after touching the progress thread or the init/finalize contract.

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
  Note that `return_error` reads `ret`, so any new `goto` must set it.
- **Directive values come from the host and are not pre-validated.**
  `pmix_rte_init`'s scan is the first thing in the library to touch what
  an RM passed to `PMIx_server_init`. A key given the wrong type, or a
  `PMIX_STRING` carrying a `NULL`, is a host bug — but it must produce
  `PMIX_ERR_BAD_PARAM`, not a `strdup(NULL)`. Check the type before
  dereferencing when you add a directive.
- **`pmix_globals` fields that are MCA-var storage.** Several
  (`max_events`, `output_limit`, `tag_output`, …) are handed to
  `pmix_mca_base_var_register` as backing store, so their values arrive
  from the environment. `pmix_rte_finalize` walks the notification hotel
  with `pmix_globals.max_events`, which is why that walk has to stay
  consistent with the value the hotel was sized with.

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
