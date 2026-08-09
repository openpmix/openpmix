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
[`src/server/pmix_server_registration.c`](../server/pmix_server_registration.c)
(the canonical reference named in the top-level guide) and `pmix_session.c` in this
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

### stdin flow control lives here (`pmix_iof.c`)

`pmix_iof_flow_control()` is the single place an XON/XOFF is applied, no
matter which of the three things asked for it: the host through
`PMIx_server_IOF_flow_control`, a `push_stdin` upcall completed with
`PMIX_ERR_IOF_XOFF`, or an upstream server through the
`PMIX_PTL_TAG_IOF_CONTROL` recv. It suspends/resumes `stdinev_global`
and relays to every peer flagged `stdin_producer`; a launcher is both a
server and a tool, so a chain of them carries the request back to
whoever holds the actual input stream.

Three rules that are load-bearing here:

- **Nothing is buffered on behalf of a suspended stream.** A suspension
  is just a read event left un-armed — the bytes stay in the producer's
  input stream and the OS applies the back-pressure. If you ever find
  yourself queueing to "help", you have moved the unbounded growth
  rather than removed it.
- **The read is re-armed by the acknowledgement, not by the read
  handler.** Both stdin paths in `pmix_iof_read_local_handler` return
  without re-arming; `opcbfn` (host upcall) and `iof_stdin_cbfunc` (relay
  to our server) do it when the far end answers. That ack is the only
  thing that paces the producer — restoring the old unconditional
  `goto reactivate` silently removes flow control, which is why
  `test/unit/iof_flow.c` asserts it.
- **`PMIX_ERR_IOF_XOFF` is not a failure.** It means "I took the data,
  now stop"; no data was lost and no stream was closed, so it must not
  raise `PMIX_ERR_IOF_FAILURE` or tear the read event down. Only an XON
  clears it — there is no status that means "resume".

Note the vestigial remnants that are *not* the mechanism:
`pmix_iof_sink_t.xoff` and `PMIX_IOF_MAX_INPUT_BUFFERS` in
`pmix_iof.h` are both dead (written once, never read; defined, never
used) and were inherited from ORTE. Don't reason about flow control from
them.

Four more things about the stdin side that are easy to get wrong:

- **There is exactly one read event on our stdin, and
  `pmix_iof_setup_stdin_read()` is the only thing that builds it.** Both
  roles that forward their own stdin go through it — `PMIx_IOF_push` with
  `PMIX_IOF_PUSH_STDIN`, and a tool given `PMIX_FWD_STDIN` at init — and
  a second call once the stream is running is a no-op. That is not tidying:
  `src/tool/pmix_tool.c` used to keep a read event of its own, so a tool
  told to forward stdin at init and then handed a `PMIX_IOF_PUSH_STDIN`
  ended up with two read events on fd 0 stealing bytes from each other,
  and `pmix_iof_flow_control` — which only knows `stdinev_global` — could
  not suspend a tool's stdin at all.
- **The SIGCONT event belongs on `evbase`, and it has to be *added*.**
  Its handler is `pmix_iof_stdin_cb`, which resolves `stdinev_global` and
  arms or disarms that read event — all state `evbase` owns — so putting
  the signal on `evauxbase` (which a host may supply as a *different*
  thread) races the progress thread. And `pmix_event_signal_set` only
  *assigns* an event; without a matching `pmix_event_add` the handler
  never runs, which is how a tool started in the background can end up
  never reading stdin at all after it is foregrounded.
- **`stdinev_global` and `stdinsig_ev` are process-wide, not per
  request**, so nothing in the request machinery ever gives them back.
  `pmix_iof_finalize()` does, and `pmix_rte_finalize` calls it **while
  the event base is still standing** — after `pmix_progress_thread_stop`
  the delete would be the use-after-free it exists to prevent. Note the
  second-init hazard this closes: a stale `stdinev_global` names an event
  registered on a base the previous cycle tore down.
- **stdin is the application's descriptor, not ours.** The read-event
  destructor closes only fds above 2, exactly as the write-event
  destructor does. A read event on a pfexec child's pipe is ours to
  close; fd 0 is not, and closing it at finalize takes the caller's stdin
  away. `test/unit/tool_cycle.c` puts a pipe on fd 0, cycles a tool with
  `PMIX_FWD_STDIN`, and fails on the first cycle if the descriptor is
  closed or is no longer the same pipe.

### Output delivery and the end of a stream (`pmix_iof.c`)

Output arrives at `pmix_iof_write_output()` one `pmix_byte_object_t` at a
time and leaves through a `pmix_iof_write_event_t` ("the channel"). Three
invariants govern that path, and all three have been broken in ways that
lose the user's output *silently*.

- **A zero-byte delivery is an end-of-stream marker, and what it means
  depends on the sink.** A sink this library opened for one source
  (`pmix_iof_setup`, fd > 2) is finished: close it and stop, leaving
  `wev.pending` set so nothing re-arms the event on a descriptor that is
  gone. The **shared** `pmix_client_globals.iof_stdout` / `iof_stderr`
  sinks (fd 1 and 2) are fed by *every* source, so one source closing
  must not stop them — skip the marker and keep draining. Getting this
  wrong is invisible in testing and total in effect: `wev.pending` stays
  set with the event un-armed, `write_output_line` only arms the event
  when `pending` is clear, and every later chunk from every other source
  sits in the queue until finalize.
- **`pending` is the arm/disarm interlock, not a status bit.** Any path
  that returns from `pmix_iof_write_handler` without clearing it is
  asserting "this channel is done forever".
- **A stream's last, unterminated line is written when the stream
  closes.** Non-raw output is split on `'\n'` and the tail is parked on
  `pmix_server_globals.iof_residuals` until the rest of the line arrives.
  The zero-byte marker is the last chance to write it, so
  `pmix_iof_write_output` flushes the matching residual before passing
  the marker along. Do not rely on the sweeps for this: a server's
  `pmix_iof_flush_residuals()` at `PMIx_server_finalize` prints it late
  and out of order, and a client or tool never sweeps that list at all
  (`iof_sink_destruct` gates `flush_sink_residuals` on being a server).

Two related facts worth knowing before you touch that code:

- `pmix_server_globals.iof_residuals` is **statically initialized**
  (`PMIX_LIST_STATIC_INIT` in `pmix_server.c`), which is why every role
  may append to it without a server ever having been initialized.
- A residual grows without bound for a source that never emits a newline,
  and each new chunk copies the whole accumulation. That is inherent to
  "buffer until the line completes"; it is not currently capped.
- `PMIX_IOF_TAG_DETAILED_OUTPUT` costs **two GDS fetches per line of
  output** — `pmix_iof_prep_output` looks the source's `PMIX_HOSTNAME`
  and `PMIX_PROC_PID` up every time it formats one. Both are immutable
  per source, so this is the obvious thing to cache if detailed tagging
  ever shows up in a profile.

### An IOF request a process registers on *itself* has no delivery path

`pmix_globals.iof_requests` holds two kinds of entry that look alike and
are delivered in completely different ways. Knowing which is which is the
whole of understanding this code:

- **Registered by a remote peer**, in `pmix_server_ops.c`'s
  `PMIX_IOF_PULL_CMD` handler. `req->requestor` is that peer, and
  delivery means packing a message and sending it —
  `pmix_iof_process_iof()` is the only thing that does this.
- **Registered by this process**, through its own `PMIx_IOF_pull`.
  Delivery means invoking `req->cbfunc`.

The second kind only works when the registration was **forwarded
upstream**. `req->cbfunc` is invoked from exactly three places — the
`PMIX_PTL_TAG_IOF` receive handlers in `pmix_server_iof.c`, `pmix_client.c`
and `pmix_tool.c` — and every one of them fires only when a message
arrives on a socket. A client or tool with a server above it gets that:
`myreg()` sees a NULL `requestor`, sends `PMIX_IOF_PULL_CMD` upstream, and
the server later sends matching output back down.

**A process that is its own server does not.** `PMIx_IOF_pull` sets
`req->requestor = pmix_globals.mypeer` in that case, and from there:
`pmix_iof_process_iof()` refuses it at its "never forward to myself" test,
so the live path skips it; and there is no upstream, so no message ever
arrives and `req->cbfunc` is never called. The registration is inert. The
output still reaches the terminal, because `_iofdeliver` calls
`pmix_iof_write_output()` before it walks the request array — but nothing
is ever handed to the callback the caller registered.

This is a gap, not a regression: it looks like a feature that was started
and not finished. `requestor` is still load-bearing on that path, though —
it is what tells `myreg()` not to send a registration upstream to itself.

**None of this affects a peer that registers with us**, which is the case
that actually matters and which works. Cached output in
`pmix_server_globals.iof` is delivered to a newly-registered *remote*
requestor from two places, both through `pmix_iof_process_iof()`:

- `pmix_server_process_iof()` (`src/server/pmix_server_ops.c`) — the
  spawn-time, per-namespace registration.
- `_iofreg()` (`src/server/pmix_server_switchyard.c`) — the completion of a tool's
  `PMIx_IOF_pull` arriving over the wire. Note *where* it scans: after
  `PMIX_SERVER_QUEUE_REPLY` has sent the refid back, so the tool cannot
  be handed IO for a handler id it has not been told yet. Ordering, not a
  timer, is what solves that.

A third copy of this used to live here as `process_cache()`, reached only
from `myreg()` and a one-second `delayed_process_cache` timer that the
blocking `PMIx_IOF_pull` armed. Both only ever carry requests *this*
process registered, so its own "never forward to myself" test rejected
every entry and it had never once run. Being unreachable is also how it
drifted out of step with its twin: it packed `req->local_id` where the
identical message for the identical receiver needs `req->remote_id` — the
id *the requestor* knows the request by, not our index into our own
array. It and the timer are gone. If local delivery is ever wanted, it
needs a design decision first — what invokes the callback, and whether
the cache entry is consumed when it does — not a revival of that
function, whose message-packing shape is wrong for a request that needs a
callback rather than a socket write.

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
10. **Nothing in here may enter the blocking form of a public API.**
   This is the top-level "back-end code must never call a `PMIx_*` that
   thread-shifts" rule, and it has two distinct faces in this directory.
   *Inside the library*: `PMIx_Notify_event` with a **NULL cbfunc** is
   its blocking form — it posts a caddy to the progress thread and then
   `PMIX_WAIT_THREAD`s on it. Every recv callback and every threadshift
   handler here already runs on that thread, so the caddy can never fire
   and the progress thread is dead for the life of the process. Pass a
   do-nothing completion instead; `pmix_pfexec.c` and `psensor/file` are
   the reference. *At the entry points*: a **host** can call a blocking
   `PMIx_*` from inside a PMIx callback, which is the same deadlock from
   the other side. Guard every blocking path with
   `pmix_progress_thread_check_blocking("<api name>")` and return
   `PMIX_ERR_WOULD_BLOCK` — a tool that deregisters from inside its own
   IOF delivery callback is doing exactly this, and the guard turns a
   permanent hang into a diagnostic.
11. **Caddy fields you did not set are garbage, not zero.** `PMIX_NEW`
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
- [`test/unit/iof_output.c`](../../test/unit/iof_output.c) — the IOF
  output path, driven by standing a pipe up in place of stdout and
  stderr and handing the library output through
  `PMIx_server_IOF_deliver`. It covers the end-of-stream invariants
  above: that a zero-byte marker from one source does not silence the
  shared channel for every other source, and that a last line with no
  newline is written when the stream closes. Both failures are silent
  against an unfixed library — the output simply never appears — so the
  test asserts on bytes read back out of the pipe, not on return codes.
- [`test/unit/iof_flow.c`](../../test/unit/iof_flow.c) — the stdin half:
  XOFF stops the reading, XON restarts it, nothing is lost across a
  suspension. See the flow-control rules above.
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
