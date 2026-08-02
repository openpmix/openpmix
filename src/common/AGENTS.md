<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The `src/common` Role-Shared API Layer

This document orients AI agents and human contributors working in
`src/common`. It assumes you have already read the top-level
[`AGENTS.md`](../../AGENTS.md) — the golden rules (prefix conventions,
`pmix_config.h`-first include order, constant-on-the-left comparisons,
brace-everything, `#if` not `#ifdef` for logical macros, warning-free
under `--enable-devel-check`), the copyright-header requirement, and
especially the **Thread Safety and the Progress Thread** section
(caddies, `PMIX_THREADSHIFT`, blocking vs. non-blocking paths). All of
that applies here and is not repeated. This file covers what is specific
to `src/common`: what lives here, the caddy/thread-shift idiom as it is
actually written in these files, and the recurring pitfalls that this
directory gets wrong often enough to be worth calling out by name.

## What this directory is

`src/common` is **not** an MCA framework. There is no framework header,
no components, no selection logic — just a set of `.c`/`.h` files
compiled straight into `libpmix` via
[`Makefile.include`](Makefile.include) (the `sources +=` / `headers +=`
lists). It holds the implementations of public `PMIx_*` API calls that
are shared by all three roles — **client, server, and tool** — rather
than belonging to any one of them (`src/client`, `src/server`,
`src/tool`). A file lands here when the same entry point must work
whether the caller is an application process, an RM-hosted server, or a
debugger/launcher tool, with the code branching internally on peer role.

Because these are the public API entry points, almost everything here
follows the same shape: a thin public `PMIx_*` function that validates
state, packs the request into a **caddy**, and thread-shifts it onto the
progress thread, plus the handlers that run there and the PTL receive
callbacks that fire when the server replies. Read
[`src/server/pmix_server.c`](../server/pmix_server.c) (the canonical
reference named in the top-level guide) and `pmix_session.c` in this
directory (the smallest complete example) before writing or changing an
entry point.

## The files

| File | Public API | Notes |
|------|-----------|-------|
| `pmix_query.c` | `PMIx_Query_info[_nb]` | Query info from the local library / server / RM; resolves locally-answerable keys (ABI version, attribute support) before forwarding. |
| `pmix_data.c` | `PMIx_Data_pack`/`unpack`/`copy`/`print`/`load`/`unload`/`embed`/`compress` | Mostly synchronous `bfrops` wrappers; moves buffer ownership via `PMIX_EMBED_DATA_BUFFER`/`PMIX_EXTRACT_DATA_BUFFER`. |
| `pmix_attributes.c/.h` | `PMIx_Register_attributes` | Attribute registration + `pmix_info` introspection tables. **The `*_fns[]` tables are stale vs. the Standard** — see the top-level guidance; treat the Standard as authoritative. |
| `pmix_alloc.c` | `PMIx_Allocation_request[_nb]`, `PMIx_Resource_block[_nb]` | RM/scheduler allocation + resource-block requests; caches returned info into GDS. |
| `pmix_control.c` | `PMIx_Job_control[_nb]` | Job-control directives. `query_cbfunc` here is the **reference** lost-connection handler — copy it. |
| `pmix_monitor.c/.h` | `PMIx_Process_monitor[_nb]`, `PMIx_Heartbeat` | The most complex of the request files: offers to `psensor`, then resolves local vs. remote targets and merges `pstat` + host results. |
| `pmix_log.c` | `PMIx_Log[_nb]` | Log/alert delivery; falls back to local `plog` when the server returns `PMIX_ERR_NOT_AVAILABLE`. |
| `pmix_security.c` | `PMIx_Get_credential[_nb]`, `PMIx_Validate_credential[_nb]` | psec credential handshake; self-serves when not connected. |
| `pmix_session.c` | `PMIx_Session_control` | Smallest complete threadshift+PTL example. Good template. |
| `pmix_iof.c/.h` | `PMIx_IOF_pull`/`deregister`/`push`, `PMIx_IOF_channel_string` | I/O forwarding: routes stdout/stderr/stddiag/stdin between procs, servers, and tools. By far the largest file. |
| `pmix_pfexec.c/.h` | (internal) `pmix_pfexec_base_*` | Local fork/exec: when PMIx itself launches processes (singleton, tool spawning children). Collapsed from the former `src/mca/pfexec` framework. |
| `pmix_strings.c` | `PMIx_*_string` converters | Pure enum→string helpers, no threadshifting. Must stay in sync with the enums in `include/pmix_common.h.in`. |

### Output-file naming lives here (`pmix_iof.c`)

`pmix_iof_setup()` is what opens the per-process output sinks, and it is
the only place that knows the namespace and rank at the moment a file is
created — which is why the naming, and the `PMIX_IOF_FILE_PATTERN`
expansion, are PMIx's job and not the launcher's.

Two rules to keep when touching it:

- **Default naming annotates; `PMIX_IOF_FILE_PATTERN` does not.** Without
  the flag the name given in `PMIX_IOF_OUTPUT_TO_FILE` is a stem, written
  as `<file>.<nspace>.<rank>.out`. With it, the name is the caller's own
  and its `%` conversions (`%n`, `%r`, `%R`, `%h`, `%%` — see the contract
  on `pmix_iof_check_pattern()` in `pmix_iof.h`) are expanded instead. The
  stream suffix (`.out`/`.err`) is appended either way, so stdout and
  stderr can never collide.
- **Create the directory from the FINAL name, not the one you were
  handed.** A pattern may put conversions in the directory part
  (`%h/rank-%R`); taking `pmix_dirname()` of the raw pattern creates a
  directory literally named `%h` and then fails to open the file in the
  one the pattern actually named.

`pmix_iof_check_pattern()` exists so a launcher can reject a bad pattern
while the user is still looking at their command line. It shares its
walker with the expander on purpose: a pattern the check accepts must
never be one the expansion rejects, or expand differently.

### There is no separate `pfexec_linux.c`

Historical note: a `pfexec_linux.c` used to live here — a stale leftover
from the pfexec-framework collapse (commit `59da6ad6`) that was moved
into `src/common/` but never wired into `Makefile.include` and still
`#include`d headers deleted in that same commit. It was dead (never
compiled, would not have compiled) and has since been removed. All local
fork/exec logic lives in `pmix_pfexec.c`. If you see references to it in
old commits, they are obsolete.

## The caddy / thread-shift idiom as written here

Every request-style entry point in this directory follows the blocking /
non-blocking split described in the top-level guide. The concrete shape,
using `pmix_session.c` as the model:

1. **Public `PMIx_*` function** checks `pmix_globals.initialized` and
   `progress_thread_stopped` (atomics), allocates a caddy with
   `PMIX_NEW`, and **assigns pointers** to the caller's inputs (no
   copying — the caller guarantees they stay live until the callback).
2. If the caller passed a **NULL cbfunc** (blocking), the function
   constructs a stack `pmix_lock_t` (or a stack `pmix_cb_t`), substitutes
   an internal callback that stores status and wakes the lock,
   `PMIX_THREADSHIFT`s, then `PMIX_WAIT_THREAD`s on the lock.
3. If the caller passed a **cbfunc** (non-blocking), the function
   `PMIX_THREADSHIFT`s and returns `PMIX_SUCCESS` immediately.
4. The **threadshift handler** (`_session_control` etc.) runs on the
   progress thread, packs a `pmix_buffer_t`, and calls
   `PMIX_PTL_SEND_RECV(rc, myserver, msg, <recv_cbfunc>, cd)`.
5. The **PTL receive callback** (`ssnctrlcbfunc` etc.) unpacks the reply
   and delivers it to the caller's cbfunc, then releases the caddy.

The caddy types you will see: `pmix_shift_caddy_t` (the general-purpose
one), `pmix_query_caddy_t` (query/info replies), `pmix_cb_t` (blocking
waiters and GDS fetches), plus a few file-local types
(`pmix_alloc_caddy_t`, `pmix_rb_caddy_t`, the pfexec caddies). All carry
the mandatory `pmix_event_t ev` and a `pmix_lock_t`.

### Ownership rules that bite

- **`->proc` is not freed by the caddy destructor.** `pmix_log.c` and
  `pmix_monitor.c` `PMIX_PROC_CREATE` a `source`/`proc` and hand it to
  `cd->proc`, but `scdes`/`cbdes` do **not** free it. Every success *and*
  error path must `PMIX_PROC_FREE(cd->proc, 1)` explicitly.
- **The `infocopy` flag governs `cb->info` ownership** in
  `pmix_monitor.c`: it starts aliasing the caller's `monitor` array
  (`infocopy = false`) and is only flipped when a copy is made. **Never
  `PMIX_INFO_FREE(cb->info, ...)` without first checking `cb->infocopy`**
  — freeing it while false is a free of caller-owned memory.
- **The results object is released only through the release-fn.** Handlers
  that build a second `results` caddy and pass it as the `release_cbdata`
  of the caller's cbfunc leak it whenever `cbfunc == NULL` (or when the
  substituted blocking cbfunc ignores the release-fn). Handle the NULL
  case explicitly.

## Recurring pitfalls specific to this directory

These are mistakes the files here make repeatedly; check them whenever you
add or edit an entry point:

1. **Wakeup macro confusion.** A terminal callback that wakes a blocking
   waiter must use **`PMIX_WAKEUP_THREAD`** (which locks the mutex,
   signals, unlocks). **`PMIX_RELEASE_THREAD` is not interchangeable** —
   it is the release half of an `ACQUIRE/RELEASE` pair and unlocks a
   mutex it assumes you already hold; calling it from an arbitrary
   completion context is undefined behavior and races the waiter into a
   lost wakeup. And a progress-thread handler must never call
   `PMIX_WAIT_THREAD` on its own caddy's lock — that hangs the progress
   thread (the blocking caller is already waiting on it).
2. **Lost-connection handling.** Every PTL receive callback must treat a
   zero-byte buffer (`PMIX_BUFFER_IS_EMPTY(buf)`) as a lost connection:
   invoke the caller's cbfunc with `PMIX_ERR_COMM_FAILURE` **and**
   `PMIX_RELEASE` the caddy. A bare `return` there both leaks the caddy
   and hangs a blocking caller forever. `pmix_control.c`'s `query_cbfunc`
   is the reference implementation.
3. **Send-failure handling.** When `PMIX_PTL_SEND_RECV` returns non-
   success the recv callback will never fire, so the send-failure branch
   must report the error to the caller and release the caddy (the
   `goto errorrpt` pattern) — releasing only the `msg` buffer strands the
   caller.
4. **Status not propagated on unpack failure.** Caddies default their
   `status` field to `PMIX_SUCCESS`. If the status unpack fails on a
   non-empty buffer, set `results->status = rc` before completing, or the
   caller is told the operation succeeded with no data.
5. **NULL cbfunc.** The `_nb` entry points do not reject a NULL cbfunc.
   Handlers must still release any `results`/info they built when
   `cbfunc == NULL`, rather than relying on a release-fn that only runs
   when the cbfunc is non-NULL.
6. **String converters must track the enums.** `pmix_strings.c` switches
   go stale silently as constants are added to `include/pmix_common.h.in`
   (a missing case returns "UNKNOWN"/"UNSPECIFIED", not a compile error).
   Several bit-mask types (`pmix_device_type_t`) are additionally
   ill-served by an exact-match `switch`. When you add a constant to the
   header, add its case here in the same change.
7. **Copy-paste headers and log labels.** These files were cloned from
   one another, so a new file often starts life with a sibling's `@file`
   banner, `#endif` guard comment, or `pmix_output_verbose` label still
   attached. Check that the banner, include-guard comment, and log
   strings actually name *this* file before you commit.
8. **Never read a `pmix_value_t` union without checking `.type` first.**
   This is the single most common defect in the directory. The value in a
   caller-supplied `pmix_info_t` is whatever the application put there, so
   `directives[n].value.data.proc`, `.data.string` and `.data.darray` are
   all wild pointers until the type has been confirmed. The August 2026
   sweep found this in `PMIx_Log` (`PMIX_LOG_SOURCE`), `PMIx_Query_info`
   (the `PMIX_PROCID`/`PMIX_NSPACE`/`PMIX_RANK` qualifiers),
   `PMIx_Process_monitor` (all four `PMIX_MONITOR_TARGET_*` directives),
   `pmix_iof_check_flags` (`PMIX_IOF_OUTPUT_TO_FILE`/`_TO_DIRECTORY`) and
   `pmix_pfexec` (`PMIX_FORKEXEC_AGENT`) — every one of them a segfault on
   an ordinary application mistake. For a data array, confirm
   `PMIX_DATA_ARRAY == value.type` **and** the array's own element type
   before walking it; `target_array()` in `pmix_monitor.c` is the pattern.
9. **A completion must happen exactly once, on every path.** The two
   failure modes are symmetric and both appear here. *Never* — a request
   that returns without invoking the callback hangs the blocking wrapper
   forever (`PMIx_Process_monitor` with `PMIX_SEND_HEARTBEAT` did exactly
   that). *Twice* — an upcall that reports completion itself **and**
   returns `PMIX_SUCCESS` to say the host will call back delivers two
   completions, and the second wakes a stack lock the caller has already
   destructed (`_rbreq` in `pmix_alloc.c` did exactly that). When you hand
   a request to `pmix_host_server.*`, the return value decides who
   completes it: `PMIX_SUCCESS` means the host will, anything else
   (including `PMIX_OPERATION_SUCCEEDED`) means you must.
10. **Caddy fields you did not set are garbage, not zero.** `PMIX_NEW`
   allocates with `malloc`, so every field a constructor skips starts as
   whatever was on the heap. `scon()`/`qcon()` did not initialize
   `status`, and `chcon()` in `pmix_pfexec.c` did not initialize
   `exitcode`; all three are fixed, but the rule is general — when you add
   a field to one of these structs, add it to the constructor in the same
   change.

## pfexec-specific hazards

`pmix_pfexec.c` is different in kind from the other files — it forks and
execs — and carries hazards the request files do not:

- **Post-`fork()` / pre-`execve()` code must be async-signal-safe.** In a
  multithreaded process the child may only call async-signal-safe
  functions before exec. Be very wary of adding `malloc`-backed calls
  (`opendir`/`readdir`, `pmix_show_help`, `pmix_output*`, `fprintf`) into
  that window — they can deadlock on a lock another thread held at fork.
- **SIGCHLD runs on `evauxbase`; the `children` list belongs to
  `evbase`.** The SIGCHLD handler is installed on
  `pmix_globals.evauxbase` (a host may supply this via
  `PMIX_EXTERNAL_AUX_EVENT_BASE`, in which case it is a *different*
  thread than `evbase`, where spawn/kill/signal/complete run). Because of
  that, `wait_signal_callback` must not touch the `children` list
  directly: it `waitpid`s (process-global, thread-safe) and then
  `PMIX_THREADSHIFT`s each reaped `(pid, status)` to `child_reaped` on
  `evbase`, which does the list work. Preserve that split — do not move
  list access back into the signal handler.
- **Do not block the progress thread.** Handlers run as events, so a
  `sleep()` inside one freezes all event processing. The kill sequence
  (SIGCONT → pause → SIGTERM → pause → SIGKILL) is therefore an
  evtimer-driven state machine (`kill_proc` → `kill_stage2` →
  `kill_stage3`), not a pair of `sleep()`s — keep it that way.
- **`wait_signal_callback` reaps every child of the process**
  (`waitpid(-1, ...)`), not just pfexec's — keep that in mind if you add
  another `waitpid` consumer.

## What the August 2026 sweep changed

A five-round review of this directory landed twenty-odd defects' worth of
fixes. Three are worth knowing about because they change behavior rather
than just hardening it:

- **The pfexec kill sequence now always escalates to SIGKILL.** It used to
  escalate only when the `SIGTERM` had failed to be *delivered*, which is
  backwards: the case the sequence exists for is a child that receives
  `SIGTERM` and ignores it, and such a child was left running — and,
  having already been taken off the `children` list by the kill, never
  reaped either. Every kill now costs one `timeout_before_sigkill` pause
  (default 1s, MCA-tunable) before it completes.
- **A malformed directive is now `PMIX_ERR_BAD_PARAM`, not a crash.** See
  pitfall 8. Callers that were getting away with a mis-typed
  `PMIX_LOG_SOURCE` or `PMIX_MONITOR_TARGET_*` now get an error.
- **`pmix_iof_process_iof` and `process_cache` skip a request with no
  requestor peer.** `pmix_globals.iof_requests` holds both server-side
  requests (which have a peer to forward to) and requests a client — or a
  launcher with an upstream server of its own — registered through
  `PMIx_IOF_pull`, which do not. The server's push handler walks the whole
  array, so it used to read through a NULL peer.

### Known, deliberate, not a defect

`wait_signal_callback` in `pmix_pfexec.c` reads
`pmix_list_get_size(&pmix_pfexec_globals.children)` as an early-out, and it
runs on `evauxbase` while that list belongs to `evbase`. That is a formal
data race. It is left alone on purpose: the child is appended to the list
before the `fork()` that can generate its `SIGCHLD`, so the count cannot be
stale for a child we care about, and removing the guard would make pfexec
`waitpid(-1)` on every `SIGCHLD` in a process that currently has no
children of its own. If you do restructure it, replace the read with a
counter maintained across all six add/remove sites rather than deleting
the check.

## Building and testing

Everything here compiles into `libpmix` with a plain top-level
`make` from an already-configured tree (see the top-level guide's
"Test-building your changes"). You do **not** need
`autogen.pl`/`configure` unless you add or remove a source file in
[`Makefile.include`](Makefile.include) (adding a file only needs a plain
`make`, which regenerates the `Makefile`). The headers listed in
`Makefile.include`'s `headers` are installed and thus part of the
internal consumable surface — keep them clean. After a change, prefer
`make check` plus `test/simple/simptest`, and valgrind for the
caddy/ownership paths — the leaks and use-after-frees this code is prone
to are exactly the class of bug valgrind catches and a smoke test does
not. **Do not** run the functional suite against an `--enable-test-build`
tree (see the top-level guide).

Two suites cover this directory specifically, and they split the same way
`src/client`'s do:

- [`test/unit/common_api.c`](../../test/unit/common_api.c) — the
  singleton half. Everything a client can decide or reject without a
  server: the malformed-directive cases, the locally-resolved queries
  (repeated 200x so a caddy leak shows up), `PMIx_Data_copy_payload`,
  the IOF flag parser and XML escaper, and `PMIx_Register_attributes`.
  Several of its cases segfault against an unfixed library rather than
  merely failing — which is the point.
- [`contrib/dockerswarm/run-common-tests.sh`](../../contrib/dockerswarm/run-common-tests.sh)
  — the connected half. Query/log/job-control/allocation/monitor/IOF with
  the ranks behind *different* PMIx servers. The monitor cases are the
  reason it is multi-node at all: `pmix_monitor_processing`'s remote and
  mixed branches cannot be entered on one node. See README §13 there.

Do not try to grow `common_api.c` into the second list — a test that needs
a server belongs in the swarm suite or in a `run_*.pl` against
`test/simple`.

## When adding or modifying code here

- **Match an existing file's structure exactly.** These entry points are
  deliberately uniform; a new API should read like `pmix_session.c`.
  Every public API must accept a `pmix_info_t info[], size_t ninfo` pair
  (see the top-level guide's attributes section) and must not change the
  signature of any released API.
- **Assign, don't copy, caller inputs into caddies** — the no-copy rule
  from the top-level guide is load-bearing here.
- **Walk every exit path** for the ownership rules above (`->proc`,
  `infocopy`, `results`, lost-connection, send-failure, NULL cbfunc). The
  bugs in this directory cluster on error and edge paths, not the happy
  path.
- **Keep it warning-free and portable** under `-Werror`; use the
  `__pmix_attribute_*__` wrappers and `PMIX_HIDE_UNUSED_PARAMS` rather
  than bare GCC attributes.
