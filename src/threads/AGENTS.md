<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: PMIx Threading Primitives

This document orients AI agents and human contributors working in
`src/threads`, the small set of low-level threading primitives on which
PMIx's concurrency model is built. It assumes you have already read the
top-level [`AGENTS.md`](../../AGENTS.md) — the golden rules (prefix
conventions, `pmix_config.h`-first include order, constant-on-the-left
comparisons, brace-everything, `#define`-logical-macros-to-0/1,
warning-free under `--enable-devel-check`), the copyright-header
requirement, and **especially the "Thread Safety and the Progress
Thread" section** all apply here and are not repeated. That section
describes the *policy* (thread-shift before touching shared state, caddy
lifetimes, blocking vs. non-blocking API paths); this directory provides
the *mechanism* those policies are built from.

Like `src/class`, **`src/threads` is not an MCA framework** — no
framework header, no components, no selection logic, no `configure.m4`.
It is a handful of headers and two `.c` files compiled directly into
`libpmix` (see [Building](#building)). Its wire-up lives in
`Makefile.include`, which is *included* from the top-level
`src/Makefile.am` (note: not a standalone `Makefile.am`, and it builds no
separate convenience library).

## Why this code matters more than its size suggests

Under 900 lines total, but load-bearing. The `pmix_lock_t` construct and
its `PMIX_CONSTRUCT_LOCK` / `PMIX_ACQUIRE_THREAD` / `PMIX_WAIT_THREAD` /
`PMIX_WAKEUP_THREAD` / `PMIX_RELEASE_THREAD` macros defined in
`pmix_threads.h` are the exact machinery every blocking public API uses
to hand a request to the progress thread and block until it completes —
they appear in **~300 sites** across the tree. The memory-barrier macros
`PMIX_POST_OBJECT` / `PMIX_ACQUIRE_OBJECT` are the only thing making
cross-thread object hand-off safe. A subtle change to any of these is not
a local change; it is a tree-wide concurrency change that can manifest as
a rare deadlock or a use-after-free that only shows up under load on one
platform. Treat this directory as semantics-frozen unless you have a
specific, measured reason and team consensus, and match the existing
patterns exactly.

## The files

```
src/threads/
├── pmix_mutex.h          public mutex API (prototypes); pulls in the unix impl
├── pmix_mutex_unix.h     pthread-backed mutex struct, inlines, static-init macro
├── mutex.c               pmix_mutex_t class constructor/destructor
├── pmix_threads.h        pmix_lock_t, the ACQUIRE/WAIT/WAKEUP/RELEASE macros,
│                         condition wrappers, memory barriers, thread API protos
├── thread.c              pmix_thread_t lifecycle + TSD key registry/cleanup
├── pmix_tsd.h            thread-specific-data (pthread key) wrappers
└── Makefile.include      included from src/Makefile.am (adds headers + sources)
```

Most of the behavior lives in the headers as `static inline` functions
and macros — `mutex.c` and `thread.c` together are barely 200 lines. Read
the `.h` files first; for the lock/barrier machinery the header *is* the
implementation.

## The mutex (`pmix_mutex.h`, `pmix_mutex_unix.h`, `mutex.c`)

`pmix_mutex_t` is a `pmix_object_t` subclass wrapping a
`pthread_mutex_t`. It is a full class (`PMIX_CONSTRUCT`/`PMIX_NEW`-able)
and also has a `PMIX_MUTEX_STATIC_INIT` for file-scope statics.

- **Debug builds harden the mutex.** Under `PMIX_ENABLE_DEBUG` the plain
  mutex is created `ERRORCHECK` (via `PMIX_HAVE_PTHREAD_MUTEX_ERRORCHECK`
  / `..._NP`, whichever `configure` found), so a self-deadlock or a
  double-unlock aborts loudly with a `perror` instead of hanging
  silently. The `pmix_mutex_trylock`/`lock`/`unlock` inlines check for
  `EDEADLK`/`EPERM` and `abort()` in debug; in release they are a thin
  pass-through to the pthread call with zero overhead.
- **The `atomic_` variants are aliases.** `pmix_mutex_atomic_lock` and
  friends simply call the non-atomic versions — the historical
  distinction (lock-free vs. pthread) collapsed to "always pthread" in
  PMIx. Do not read significance into the `atomic_` name.

## The lock (`pmix_threads.h`) — the piece you will actually touch

`pmix_lock_t` bundles a `pmix_mutex_t`, a `pmix_condition_t` (a raw
`pthread_cond_t` with `pmix_condition_wait/signal/broadcast` wrappers), a
`bool active`, and a `pmix_status_t status`. It is the standard
handshake between a caller thread and the progress thread. Every access
to `active` happens under the mutex (with the barrier macros below), so
it is a plain `bool` — do not add `volatile` back to it. The lifecycle
macros:

| Macro | Runs on | Effect |
|-------|---------|--------|
| `PMIX_CONSTRUCT_LOCK(l)` | caller | init mutex+cond, set `active = true` |
| `PMIX_DESTRUCT_LOCK(l)` | caller | tear down mutex+cond |
| `PMIX_ACQUIRE_THREAD(l)` | caller | block until `active == false`, **then re-arm** `active = true` |
| `PMIX_WAIT_THREAD(l)` | caller | block until `active == false`, leave it false |
| `PMIX_RELEASE_THREAD(l)` | holder | set `active = false`, signal, **unlock** (assumes lock already held) |
| `PMIX_WAKEUP_THREAD(l)` | handler | lock, set `active = false`, signal, unlock |

The two easy-to-confuse pairs:

- **`ACQUIRE` vs. `WAIT`.** Both block on the condition. `ACQUIRE_THREAD`
  re-arms `active = true` before returning (it is a *gate* you pass
  through repeatedly — acquire/release cycles), and returns holding the
  mutex is *not* implied — read the macro. `WAIT_THREAD` is a one-shot:
  it waits, then unlocks and returns, leaving `active` false. Blocking
  public APIs use the `CONSTRUCT_LOCK` … `THREADSHIFT` … `WAIT_THREAD`
  pattern (see the top-level thread-safety section and
  `src/server/pmix_server.c`).
- **`RELEASE` vs. `WAKEUP`.** `RELEASE_THREAD` assumes the caller already
  holds the mutex (it pairs with `ACQUIRE_THREAD`); it does not lock,
  only unlocks. `WAKEUP_THREAD` takes the lock itself — it is what a
  progress-thread handler calls to wake a blocked API caller that is
  sitting in `WAIT_THREAD`. **Every blocking path needs a matching
  `WAKEUP` (or `RELEASE`)** or the caller hangs forever — this is the
  single most common threading bug in the code base.

All four blocking/waking macros loop on `while ((lck)->active)` to absorb
spurious condition-variable wakeups — do not "simplify" that to an `if`.

`PMIX_LOCK_STATIC_INIT` exists for file-scope locks; note it initializes
`active = false`, whereas `PMIX_CONSTRUCT_LOCK` sets `active = true` — a
statically-initialized lock is "already released," a constructed one is
"armed." Pick the initializer that matches how the first
acquire/wait will read `active`.

### Memory barriers and cross-thread object hand-off

`PMIX_POST_OBJECT(o)` (a write memory barrier, `pmix_atomic_wmb`) and
`PMIX_ACQUIRE_OBJECT(o)` (a read barrier, `pmix_atomic_rmb`) bracket the
publication of an object from one thread and its consumption by another.
The lock macros already embed them (`ACQUIRE`/`WAIT` do an
`ACQUIRE_OBJECT`, `RELEASE`/`WAKEUP` do a `POST_OBJECT`), so within the
lock handshake you get the barrier for free. If you ever hand an object
between threads *outside* a lock, you are responsible for the barrier
pair yourself. The object argument is currently ignored — the macros are
whole-thread fences — but pass the real object anyway; the header notes
the model may be revamped and the argument made meaningful.

## Threads (`thread.c`)

`pmix_thread_t` wraps a `pthread_t` plus a run function and argument.
`pmix_thread_start` / `pmix_thread_join` are the only two entry points,
and their only live caller is the progress-thread engine in
`src/runtime/pmix_progress_threads.c`. `PMIX_THREAD_CANCELLED` is the
sentinel a cancellable thread body returns. The "not started" / "joined"
state of a `pmix_thread_t` is tracked with a `(pthread_t) -1` sentinel in
`t_handle`, compared with plain `==`/`!=`; this is a sentinel test, not a
comparison of two real thread identities (which would use
`pthread_equal`), and is left as-is deliberately.

## Thread-specific data (`pmix_tsd.h`, `thread.c`)

Thin wrappers over `pthread_key_*`, plus a small **key registry** in
`thread.c` that exists to plug a real leak:

- `pmix_tsd_key_create()` creates a pthread key and appends the
  key+destructor to a global registry (`pmix_tsd_key_values`),
  **regardless of which thread it runs on**. Callers create their keys
  lazily on first use, which may land on any thread (commonly the
  progress thread), so the registry mutation is serialized by a private
  `pmix_tsd_key_values_lock`. The `realloc` return is checked; on OOM the
  key stays valid and usable, it just is not registered for auto-cleanup.
- `pmix_tsd_keys_destruct()` (called from `pmix_finalize.c`, on the main
  thread) walks the registry under the same lock, invokes each destructor
  on the finalizing thread's value, and **deletes the pthread key** so
  its slot is reclaimed. This is necessary because pthread destructors
  never fire for the main thread (there is no `pthread_join(main)`), so
  without it every key — and its `PTHREAD_KEYS_MAX`-limited slot — would
  leak across an init/finalize cycle.

The only live callers of the TSD API are `src/util/pmix_net.c`
(`hostname_tsd_key`) and `src/util/pmix_name_fns.c`
(`print_args_tsd_key`); both create their key lazily on first use and
clear a static `*_init` latch in their `_finalize` so a subsequent
`PMIx_Init` recreates it. Because registration no longer depends on the
creating thread, that lazy-first-use pattern is safe — the key is tracked
and cleaned up no matter which thread touched it first.

## Design notes worth knowing

- **Direct `active` writes bypass the macros in a few init paths.** A
  handful of sites (`src/server/pmix_server.c`,
  `src/mca/pmdl/base/pmdl_base_frame.c`) set `lock.active = false`
  directly on a freshly constructed caddy/lock. That is a plain
  single-threaded initialization write on the owning thread *before* the
  mutex/cond handshake begins, which is why `active` needs no `volatile`;
  do not assume those writes provide any cross-thread ordering.
- **`ACQUIRE_THREAD` and `WAIT_THREAD` emit distinct debug strings**
  (`"Acquiring thread"` / `"Thread acquired"` vs. `"Waiting for thread"` /
  `"Thread obtained"`) so the two are distinguishable in a
  `pmix_debug_threads` log.

## Building

`src/threads` has no `configure.m4` and nothing conditionally compiled,
so a change to any of these files takes effect with a plain top-level
`make` from an already-configured tree (see the top-level guide's
"Test-building your changes"). You only need the
`autogen.pl` + `configure` cycle if you *add or remove* a source file,
because the file list lives in `Makefile.include` — and even then, since
`Makefile.include` is `include`d by `src/Makefile.am`, editing it is a
`Makefile.am` change: a plain `make` regenerates the `Makefile` and
rebuilds. (Full `autogen.pl`/`configure` is only mandatory for
`configure.ac`/`config/*.m4` changes, which this directory has none of.)
The four headers are listed in `Makefile.include`'s `headers` and are
installed under `$(pmixincludedir)/src/threads`, so they are part of the
consumable internal surface — keep them warning-free and portable.

## When changing code here

- **Preserve the lock handshake exactly.** If you add a blocking API,
  copy the `CONSTRUCT_LOCK` / `THREADSHIFT` / `WAIT_THREAD` /
  `DESTRUCT_LOCK` pattern from `src/server/pmix_server.c` and make sure
  the progress-thread handler calls `WAKEUP_THREAD` (or `RELEASE_THREAD`)
  on every exit path, including error paths. A missing wake is a hang.
- **Keep the spurious-wakeup `while` loops.** Never collapse them to
  `if`.
- **Do not add locking "for safety" to the mutex/lock primitives
  themselves**, and do not reorder or resize `struct pmix_mutex_t` /
  `pmix_lock_t` casually — they are embedded by value in hundreds of
  structs and initialized by static-init macros that mirror their layout.
- **Build the whole tree and run `make check` plus
  `test/simple/simptest`** after any change here, and prefer valgrind:
  concurrency bugs in this directory are rarely caught by a compile, and
  the debug `ERRORCHECK` mutex + clean-shutdown paths exist precisely to
  make deadlocks and leaks observable.
- **Keep it warning-free and portable.** This code compiles on every
  platform PMIx supports, under `-Werror` in CI. Use the
  `__pmix_attribute_*__` wrappers rather than bare GCC attributes.
