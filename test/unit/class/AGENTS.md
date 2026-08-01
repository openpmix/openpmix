<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: Unit tests for the PMIx class system

This directory holds the `make check` suite for
[`src/class`](../../../src/class) — the C object model and the container
classes built on it. Read
[`src/class/AGENTS.md`](../../../src/class/AGENTS.md) first: it describes
the contracts these tests encode, and this file assumes them. The
top-level [`AGENTS.md`](../../../AGENTS.md) rules (copyright header,
`pmix_config.h` first, constant-on-the-left, brace everything,
warning-free under `--enable-devel-check`) apply here too.

## Layout

One program per class, plus two for concurrency — one across threads and
one across processes:

| Program | Subject |
|---------|---------|
| `class_object` | `PMIX_NEW`/`RETAIN`/`RELEASE`/`CONSTRUCT`/`DESTRUCT`, class hierarchy, TMA initialization |
| `class_list` | `pmix_list_t` / `pmix_list_item_t` and the `FOREACH` macros |
| `class_hash_table` | `pmix_hash_table_t`, all three key types, growth and backshift deletion |
| `class_pointer_array` | `pmix_pointer_array_t`, index reuse, growth across `free_bits` words |
| `class_bitmap` | `pmix_bitmap_t`, including the high-bit shift and `num_set_bits` boundaries |
| `class_value_array` | `pmix_value_array_t` |
| `class_ring_buffer` | `pmix_ring_buffer_t` |
| `class_hotel` | `pmix_hotel_t` (no event base — eviction timers are disabled) |
| `class_refcount` | The reference count under concurrent threads |
| `class_tma_shared` | The object model and the TMA-aware containers across a **process** boundary, in a shared `mmap` segment |

Each is a standalone `main()` that prints `PASS:`/`FAIL:` per assertion
via a local `report()` helper and exits non-zero if anything failed.
There is no framework; keep it that way. New cases go in the matching
`class_*.c`, and its name goes in `main()`.

Adding a program means adding it to `check_PROGRAMS`, `TESTS`, a
`_SOURCES`/`_LDFLAGS`/`_LDADD` triple, and the `clean-local` list in
[`Makefile.am`](Makefile.am), plus a line in
[`.gitignore`](.gitignore). No `configure`/`autogen.pl` re-run is needed
for a `Makefile.am` edit — a plain `make` regenerates the `Makefile`.

**These are `check_PROGRAMS`, so `make all` does not build them.** A
plain `make` in this directory prints "Nothing to be done for `all'" and
leaves whatever binaries are lying around untouched — which, after a
change to `src/class`, means you can run stale binaries against a new
library and get failures that have nothing to do with your change (a
`cls_sizeof` assert is the usual symptom). Always use `make check`.

## The configuration trap

This is the single most important thing to understand about this
directory, because it is how several real defects in `src/class` survived
having a test suite.

The tree everyone develops in — and the one
`contrib/dockerswarm/build.sh` produces — is configured
`--enable-debug --enable-devel-check`, commonly with
`--disable-visibility`. In that configuration:

- A correctness guard written inside `#if PMIX_ENABLE_DEBUG` is
  **present**, so a test that exercises it passes whether or not the
  guard exists in the builds that ship. An un-init'd
  `pmix_hash_table_t` returned a clean error here and took a `SIGFPE` in
  an optimized build; `pmix_value_array_remove_item` rejected a bad index
  here and `memmove`'d a wrapped `size_t` length there.
- A class descriptor missing `PMIX_EXPORT` **links**, because nothing is
  hidden. `pmix_ring_buffer_t_class` was missing it for years, and
  `pmix_server_trkr_t` / `pmix_server_caddy_t` / the `gds` active-module
  class kept `test/unit` from linking at all on a default-visibility
  tree — which nobody noticed, because nobody builds one. If you add a
  test that names a library class, build it once without
  `--disable-visibility` before you trust it. See the export rule in
  [`src/class/AGENTS.md`](../../../src/class/AGENTS.md), including why
  grepping the headers for `PMIX_EXPORT` gives the wrong answer.

**Until recently, a default-visibility tree did not run this suite at
all.** `test/Makefile.am` wrapped its whole `SUBDIRS` line in
`if !WANT_HIDDEN`, so unless you configured `--disable-visibility`, a
top-level `make check` ran the 15 programs in `test/` itself, reported
`# FAIL: 0`, and never entered `unit/class/`. The configuration this
suite most needs to be run in was the one it was never run in, and
nothing warned you. The condition is gone (see
[`../AGENTS.md`](../AGENTS.md) for what replaced it), and
`contrib/dockerswarm/run-class-tests.sh` still invokes
`make -C test/unit/class check` directly, which is why its coverage was
real throughout.

So a green run here proves less than it looks like it does. Several
cases in the suite are labelled `(all builds)` precisely because they
only distinguish old code from new in an *optimized* build.

Close the gap with:

```sh
contrib/dockerswarm/run-class-tests.sh linux    # or macos
```

which builds and runs this suite `--disable-debug` with default symbol
visibility (see `contrib/dockerswarm/README.md` §11). Run it after any
non-trivial change to `src/class`. It reads the program list out of
[`Makefile.am`](Makefile.am), so a new `class_*` test is picked up without
touching the script. It is not a multi-node test and does not pretend to
be — with the single exception of `class_tma_shared`, these are
single-process data structures.

One caveat about its ten-node contention pass: it used to fail on all ten
nodes for a reason that had nothing to do with the library. It staged
`test/unit/class/$p`, which in an uninstalled libtool build is a `/bin/sh`
**wrapper**, not the executable — that lives under `.libs/`. The copied
wrapper then reported `.libs/$p does not exist` and exited 1 everywhere,
so that pass had never actually run anything. It stages `.libs/$p` now,
falling back to the plain path. If you add a runner that ships
`check_PROGRAMS` to another machine, remember this.

A third gap worth knowing about: **a bare `assert()` is debug-only here
too.** `config/pmix.m4` adds `-DNDEBUG` whenever `--enable-debug` is off,
so asserts vanish in every shipping build exactly as a
`#if PMIX_ENABLE_DEBUG` block does. A case that only proves "the assert
fires" proves nothing about the library users get; write it against the
return value instead.

The most recent sweep of `src/class` turned up five more defects of
exactly these two shapes, and every one of them is now pinned by a case
that was *verified by reintroducing it in the optimized build*:

| Case | Program | What backing the fix out does |
|------|---------|-------------------------------|
| `out-of-range room number` | `class_hotel` | **SIGSEGV** — `pmix_hotel_checkout`'s upper bound was an `assert()` only, so it wrote past `rooms[]` |
| `push without storage` | `class_ring_buffer` | **SIGSEGV** — `pmix_ring_buffer_push` had no guard at all, unlike `pop`/`poke` |
| `reserve requires an item size` | `class_value_array` | `reserve` succeeds on a zero item size, committing capacity over a 0-byte block |
| `key type mixing is refused` | `class_hash_table` | 8 cases fail — the check was inside `#if PMIX_ENABLE_DEBUG` |
| `set_max_size argument checking` | `class_bitmap` | 5 cases fail — a negative cap silently stored 0 and bricked the bitmap |

Note the shape of the hash-table case in particular: asserting merely
`PMIX_SUCCESS != rc` there is **not** discriminating, because without the
check a wrong-type *get* simply probes past the entry and reports
`PMIX_ERR_NOT_FOUND`. It has to assert `PMIX_ERROR == rc`. Watch for that
whenever "the call fails" is the property under test — pick the code.

## What these tests deliberately do not do

- **They never call `PMIX_DESTRUCT` twice on one object.** That is a
  contract violation; `--enable-debug` builds abort on the magic-ID
  assert, correctly. The teardown cases instead assert that the
  destructor left the object in its *constructed* state — pointers NULL,
  sizes zero — which is what keeps a release build from double-freeing
  and what makes destruct-then-reconstruct work. Do not "fix" a test by
  weakening that assert.
- **They do not put one list item on two lists**, do not remove during a
  plain `PMIX_LIST_FOREACH`, and do not otherwise poke the debug
  spot-checks to see what happens. Those asserts exist to catch caller
  bugs; a test that trips one is a broken test.
- **The per-class programs do not test the TMA path.** They all run with
  the default (libc) allocator; TMA coverage lives in `class_tma_shared`
  and nowhere else. Do not sprinkle a TMA through the others — a
  half-hearted one is worse than none, because `pmix_obj_get_tma()`
  decides "this object has a custom allocator" by testing `tma_malloc`
  alone, so a partially filled-in `pmix_tma_t` reads as valid and then
  calls through a NULL member.

## Writing a good case here

- **Name the defect, not the API.** A regression case should say what
  used to go wrong, so the next reader knows what breaking it means. The
  `/* Regressions */` blocks at the bottom of each file follow that
  shape.
- **Prefer a case that fails loudly.** The value-array zero-alloc case
  *hangs* without its fix, and the hotel static-init case takes SIGSEGV;
  both were verified by reintroducing the bug. A case that would only
  produce a subtly wrong number is worth less.
- **Verify it by reintroducing the defect.** That is the standard here,
  not a nicety: revert the fix, build (in the *optimized* configuration
  if the guard was ever debug-gated), and confirm the case fails. Four of
  the current regression cases crash outright when their fix is backed
  out — `class_object`'s STATIC_INIT destruct, `class_bitmap`'s NULL
  count, `class_pointer_array`'s negative index, `class_ring_buffer`'s
  pop-after-destruct — which is how you know they are load-bearing.
- **Do not bend a test to accommodate a bug.** If a case fails, the
  default assumption is that `src/class` is wrong. See the top-level
  guide.
- **Keep white-box pokes rare and justified.** A couple of cases reach
  into struct members (`va.array_alloc_size`, `ring.addr`,
  `base->obj_tma.tma_malloc`) because the state they describe is not
  reachable through the API. That is fine when the alternative is not
  testing a hang or an uninitialized field at all — but reach for the
  public call first.

## `class_tma_shared`

This is the only cross-**process** program in the suite, and the only one
that exercises a TMA. It exists because two claims in
[`src/class/AGENTS.md`](../../../src/class/AGENTS.md) were load-bearing
and untested: that the C11 reference count is well defined for processes
sharing a TMA segment (the `pthread_mutex_t` it replaced was never
`PTHREAD_PROCESS_SHARED`, so this was undefined), and that
`pmix_hash_table_t` and `pmix_pointer_array_t` allocate through
`pmix_obj_get_tma()` rather than libc so a second process can read what
the first built.

It mmaps a `MAP_SHARED` segment, lays a bump allocator over it with every
`pmix_tma_t` member filled in, builds the containers and an object
inside it, and forks. Children read the containers back; one child writes
and the parent reads the result; and six children hammer retain/release on
one object and drop one net reference each, after which the parent checks
that exactly its own reference survives.

It now also pins the **TMA exemption** in `pmix_hash_table.c`'s key-type
check. That exemption only became load-bearing when the check stopped
being debug-only: before, an optimized build had nothing for a TMA table
to be exempt *from*. Now the `NULL == pmix_obj_get_tma(&ht->super)`
clause is the only thing keeping `gds/shmem3`'s tables readable in the
builds that ship, because `ht_type_methods` points at a file-scope static
in whichever process last touched the table and that address is
meaningless to a peer. Delete the clause and this program does not even
get through `build_shared_state()`.

That case is also why this program deliberately puts **both** `uint32`
and `ptr` keys in one table — a thing no other test here does, and a
thing that would be a caller bug on an ordinary heap-backed table
(the ptr key storage would never be freed, and a later grow or removal
would rehash those elements through `key.u32`). It is safe here only
because the table never grows or removes after the mixing, and it is
present on purpose, to demonstrate the exemption. Do not copy the
pattern into the other programs.

Two things to preserve if you extend it:

- **The start barrier is not decoration.** Without it the children tend to
  run one after another and a deliberately broken (non-atomic) count
  survives by luck; with it, reintroducing a non-atomic `pmix_obj_update`
  fails the case on every run. Verified both ways.
- **Only one process mutates a container at a time.** These classes are
  not internally synchronized and are not meant to be. The subject is
  whether the *storage* is shared, not whether concurrent mutation is
  safe — it is not.

What it deliberately does not model: segment negotiation and attach
between unrelated processes. Its second process comes from `fork()`, so
the segment lands at the same address and the function pointers inside the
embedded `pmix_tma_t` stay valid. Real `gds/shmem3` peers get neither —
which is exactly why `pmix_hash_table.c` skips its key-type assert for TMA
tables. That half belongs to `gds/shmem3` and the swarm group tests.

## `class_refcount`

The reference count is the **only** part of `src/class` documented as
safe to touch from more than one thread; every other class assumes the
caller serializes, which in PMIx means the progress thread. So this is
the one place threads belong in this suite. Do not add threads to the
other programs to "improve coverage" — you would be testing a contract
the classes do not offer.

It checks three properties, all of which the C11 atomic in
`pmix_obj_update()` is responsible for: balanced retain/release from N
threads leaves the count exactly where it started; exactly one thread
observes zero and therefore exactly one destructor runs; and the writes
a thread made before its last release are visible to that destructor
(the `acq_rel` ordering). The test is written against the properties
rather than the mechanism, so it does not need to change if the
mechanism does.

It carries one more case that is not about the refcount at all: **"class
init race"**, which puts N threads on the first-ever use of a
never-instantiated class so they all race into `pmix_class_initialize`.
Lazy class init is a double-checked lock whose fast path is unlocked (see
[`src/class/AGENTS.md`](../../../src/class/AGENTS.md)), and nothing else
in this suite ever has two threads in it at once.

**Be honest about what that case proves on its own: very little.** It
cannot reliably fail on x86-64 or arm64 even with the release/acquire
pair removed, because both give the plain load enough ordering in
practice. Its value is that it creates the concurrency for a race
detector to find. The real signal is ThreadSanitizer:

```sh
./configure --enable-debug CC=clang \
    CFLAGS="-fsanitize=thread -g -O1" LDFLAGS="-fsanitize=thread"
make && make -C test/unit/class class_refcount
./test/unit/class/class_refcount
```

Against the plain-`int` version of `cls_initialized` that reports seven
data races inside `pmix_class_initialize` and aborts; against the current
code it reports none. If you change anything about class initialization,
that is the check to run — not this program's exit status.

The barrier is hand-rolled from a mutex and condvar because macOS has no
`pthread_barrier_t`. No per-target thread flags are needed —
`config/pmix_config_threads.m4` folds them into the global
`CFLAGS`/`LDFLAGS`/`LIBS`.

Each thread writes only its own slot of the payload array, so the
visibility check is not itself a data race. Keep it that way if you
extend it.
