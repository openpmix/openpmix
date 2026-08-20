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

- **Debug builds harden the mutex - but only the constructed ones.**
  Under `PMIX_ENABLE_DEBUG` `pmix_mutex_construct` creates the mutex
  `ERRORCHECK` (via `PMIX_HAVE_PTHREAD_MUTEX_ERRORCHECK` / `..._NP`,
  whichever `configure` found), so a self-deadlock or a double-unlock
  aborts loudly with a `perror` instead of hanging silently.
  `PMIX_MUTEX_STATIC_INIT` cannot do that - `PTHREAD_MUTEX_INITIALIZER`
  is a *normal* mutex and there is no static spelling of an attribute -
  so a file-scope lock keeps ordinary pthread behavior in every build.
  Do not read a debug run's clean bill of health on a statically
  initialized lock as evidence it is deadlock-free.
  The `pmix_mutex_trylock`/`lock`/`unlock` inlines check for
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
| `PMIX_ACQUIRE_THREAD(l)` | caller | block until `active == false`, re-arm `active = true`, **return with the mutex still held** |
| `PMIX_WAIT_THREAD(l)` | caller | block until `active == false`, leave it false, **unlock before returning** |
| `PMIX_RELEASE_THREAD(l)` | holder | set `active = false`, signal, **unlock** (assumes the mutex is already held) |
| `PMIX_WAKEUP_THREAD(l)` | handler | lock, set `active = false`, signal, unlock |

They are two pairs, and the pairing is about **who is holding the
mutex** — that, not the naming, is what makes them non-interchangeable:

- **`WAIT` + `WAKEUP` is the pair the library actually uses**, at ~120
  sites each. `WAIT_THREAD` unlocks before it returns, so the waiter
  holds nothing; `WAKEUP_THREAD` takes the mutex itself, so a
  progress-thread handler can call it with nothing held. This is the
  `CONSTRUCT_LOCK` … `THREADSHIFT` … `WAIT_THREAD` shape of every
  blocking public API (see the top-level thread-safety section and
  `src/server/pmix_server.c`). **Every blocking path needs a matching
  `WAKEUP`** on every exit including the error ones, or the caller hangs
  forever — the single most common threading bug in the code base.
- **`ACQUIRE` + `RELEASE` is a held-mutex critical section.**
  `ACQUIRE_THREAD` locks, waits, re-arms `active = true` and returns
  **still holding the mutex**; `RELEASE_THREAD` therefore does not lock,
  it only unlocks. Mixing the pairs is not a style choice:
  `RELEASE_THREAD` on a lock you did not acquire is an unlock of a mutex
  you do not hold (which the debug `ERRORCHECK` mutex aborts on), and
  returning from `ACQUIRE_THREAD` without a matching `RELEASE_THREAD`
  leaves the mutex held for good.

  **Nothing in `libpmix` uses this pair — PRRTE does.** These headers are
  installed under `$(pmixincludedir)/src/threads`, and
  `src/runtime/prte_locks.h` includes `src/threads/pmix_threads.h` to
  declare `prte_init_lock` as a `pmix_lock_t`; `prte_init()`,
  `prte_finalize()` and `pmix_server_notify.c` then guard
  `prte_initialized` with `ACQUIRE`/`RELEASE` across eleven call sites.
  So this is external API with exactly one known consumer, not dead code:
  do not retire it on the evidence of an in-tree grep, and treat a change
  to its semantics as a change to PRRTE. `test/unit/threads_primitives.c`
  is the in-tree coverage that keeps it honest.

All four blocking/waking macros loop on `while ((lck)->active)` to absorb
spurious condition-variable wakeups — do not "simplify" that to an `if`.

**`status` is not initialized by `PMIX_CONSTRUCT_LOCK`,** while
`PMIX_LOCK_STATIC_INIT` sets it to `PMIX_SUCCESS`. That asymmetry is
survivable only because of an invariant nothing enforces: every waiter
that reads a lock's `status` is woken by a handler that assigns it
first. It holds today — a sweep of the tree found no `lock.status` read
whose wake path leaves it unset, and most waiters read a `status` field
on their own caddy rather than the lock's. If you add a reader, make
sure **every** path that can wake that lock assigns `status`, including
the error paths; the lock frequently lives in a `PMIX_NEW`'d caddy, and
`PMIX_NEW` does not zero, so the alternative to a real value is heap
garbage rather than zero.

**`WAKEUP_THREAD` signals *before* it unlocks, and that ordering is
load-bearing.** The lock usually lives in the waiting thread's stack
frame, and that thread destroys it (`PMIX_DESTRUCT_LOCK`) the moment
`WAIT_THREAD` returns. Because the signal happens under the mutex, the
waiter cannot re-acquire and return until the waker's unlock has
completed, so the waker is finished touching the object before it can be
torn down. "Optimizing" this into the usual unlock-then-signal form
turns a normal completion into a use-after-free on the condition
variable. The same applies to `RELEASE_THREAD`.

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
sentinel a cancellable thread body returns.

**The `(pthread_t) -1` sentinel in `t_handle` is the whole state
machine**, and all three functions have to maintain it: the constructor
installs it, a failed `pmix_thread_start` puts it back (POSIX leaves the
handle's contents unspecified when the create fails, so trusting
`pthread_create` not to have written it is not portable), and
`pmix_thread_join` restores it after reaping. `pmix_thread_join`
therefore **refuses** a handle still carrying it, rather than passing it
down: `pthread_join((pthread_t) -1, …)` is undefined, and on glibc it
dereferences the sentinel, so the caller gets a segfault where it
expected an error return. Both a never-started thread and a
second join land there. It is compared with plain `==`/`!=` — a sentinel
test, not a comparison of two real thread identities (which would use
`pthread_equal`) — and is left that way deliberately.

Note that `pmix_thread_start`'s parameter validation is inside
`if (PMIX_ENABLE_DEBUG)`, so a release build does not screen a NULL
`t_run`. Callers own that check.

`t_arg` is the only thing the body receives; `pmix_thread_t` has no
destructor and owns nothing, so whatever `t_arg` points at must outlive
the thread. Note that `PMIX_NEW` does not zero, which is why the
constructor sets every field explicitly rather than only the interesting
ones.

## Thread-specific data (`pmix_tsd.h`, `thread.c`)

Thin wrappers over `pthread_key_*`, plus a small **key registry** in
`thread.c` that exists to plug a real leak:

- `pmix_tsd_key_create()` creates a pthread key and appends the
  key+destructor to a global registry (`pmix_tsd_key_values`),
  **regardless of which thread it runs on**. One of its two callers
  creates its key lazily on first use, which may land on any thread
  (commonly the progress thread), so the registry mutation is serialized
  by a private `pmix_tsd_key_values_lock`. The `realloc` return is
  checked; on OOM the key stays valid and usable and the create still
  reports success, it just is not registered for auto-cleanup.
  It returns a `pmix_status_t`, **not** the pthread errno: callers hand
  the result to `PMIX_ERROR_LOG` and `pmix_net_init` returns it straight
  out of `PMIx_Init`, and a positive errno arriving in a status channel
  reads as some unrelated PMIx constant.
- `pmix_tsd_keys_destruct()` walks the registry under the same lock,
  invokes each destructor on the finalizing thread's value, and
  **deletes the pthread key** so its slot is reclaimed. This is necessary
  because pthread destructors never fire for the main thread (there is no
  `pthread_join(main)`), so without it every key — and its
  `PTHREAD_KEYS_MAX`-limited slot — would leak across an init/finalize
  cycle.

  Two conditions on when it may be called, and neither is optional.
  It must run **on the main thread**, whose values nothing else will ever
  destroy; and it must run when **every other thread has been joined**,
  because it deletes the keys outright. A thread still running can reach
  a key that no longer exists, and any code below the call that prints a
  process name re-creates the print-buffer key into a registry nothing
  will walk again — leaking exactly the slot this function exists to
  reclaim. That is why the call sits at the very end of
  `pmix_rte_finalize()`, after `pmix_progress_thread_stop()`, rather than
  beside the other cleanup.

  It only ever destroys the *calling* thread's value. Values other
  threads left behind were already handled by pthread when they exited.
  And it honors pthread's own rule that a destructor is never invoked
  with a NULL value — a destructor written to that contract may
  dereference its argument, and this is the one call site that could
  have handed it NULL, for any key the finalizing thread never used.
  (`pmix_tsd_getspecific` always returns `PMIX_SUCCESS`, so the status
  check around it decides nothing; the NULL check is the real one.)

The only live callers of the TSD API are `src/util/pmix_net.c`
(`hostname_tsd_key`) and `src/util/pmix_name_fns.c`
(`print_args_tsd_key`), and they create their keys differently.
`pmix_net_init()` creates its key eagerly, on the main thread, as its
last act. `get_print_name_buffer()` creates its key **lazily on first
use** — which may land on any thread, commonly the progress thread — and
clears a static `fns_init` latch in `pmix_name_fns_finalize()` so a
subsequent `PMIx_Init` recreates it. Because registration no longer
depends on the creating thread, that lazy-first-use pattern is safe: the
key is tracked and cleaned up no matter which thread touched it first.

## Design notes worth knowing

- **Direct `active` writes bypass the macros in a few init paths.** A
  handful of sites (`src/server/pmix_server_inventory.c`,
  `src/mca/pmdl/base/pmdl_base_frame.c`) set `lock.active = false`
  directly on a freshly constructed caddy/lock. That is a plain
  single-threaded initialization write on the owning thread *before* the
  mutex/cond handshake begins, which is why `active` needs no `volatile`;
  do not assume those writes provide any cross-thread ordering.
- **`ACQUIRE_THREAD` and `WAIT_THREAD` emit distinct debug strings**
  (`"Acquiring thread"` / `"Thread acquired"` vs. `"Waiting for thread"` /
  `"Thread obtained"`) so the two are distinguishable in a
  `pmix_debug_threads` log. **Nothing in the tree ever sets
  `pmix_debug_threads`** — there is no MCA parameter for it and no
  assignment outside its definition, so those lines only appear if you
  set the variable from a debugger. Do not conclude from a silent run
  that the lock handshake was not exercised.
- **The cast in `pmix_thread_start`** hands a
  `void *(*)(pmix_object_t *)` to `pthread_create`, which wants a
  `void *(*)(void *)`. That is a call through an incompatible function
  pointer type, and it is deliberate rather than overlooked: GCC's
  `-Wcast-function-type` (on here through `-Wextra -Werror`) treats any
  pointer parameter as matching any other, so it is not diagnosed, and
  every ABI PMIx supports passes a `void *` and a struct pointer
  identically. Only clang's opt-in `-Wcast-function-type-strict` and
  UBSan's `-fsanitize=function` object, and neither is used by the build
  or by CI (the sanitizer job is ASan only). Leave it alone unless one of
  those is turned on, in which case the fix is a small trampoline rather
  than a cast.

## Testing

[`test/unit/threads_primitives.c`](../../test/unit/threads_primitives.c)
is this directory's regression test, wired into `make check`. It covers
the paths the progress engine never takes: joining a thread that was
never started and joining one twice, the constructor's field
initialization, the `WAIT`/`WAKEUP` round trip driven by a real second
thread, the otherwise-uncalled `ACQUIRE`/`RELEASE` pair, and the TSD
registry — including the case that asserts a destructor is **not** run
for a key the finalizing thread never set a value for. That negative
case is the important one and is easy to make vacuous; read the comment
above it before touching it.

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
