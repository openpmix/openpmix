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

One program per class, plus one for concurrency:

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
  hidden. `pmix_ring_buffer_t_class` was missing it for years.

So a green run here proves less than it looks like it does. Several
cases in the suite are labelled `(all builds)` precisely because they
only distinguish old code from new in an *optimized* build.

Close the gap with:

```sh
contrib/dockerswarm/run-class-tests.sh linux    # or macos
```

which builds and runs this suite `--disable-debug` with default symbol
visibility (see `contrib/dockerswarm/README.md` §11). Run it after any
non-trivial change to `src/class`. It is not a multi-node test and does
not pretend to be — these are single-process data structures.

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
- **They do not test the TMA path.** Everything runs with the default
  (libc) allocator. Exercising a real TMA means a shared-memory segment
  and a second process, which is `gds/shmem3`'s territory — see the
  group tests under `contrib/dockerswarm`. If you add TMA coverage here,
  it needs a purpose-built allocator, not a partially filled-in
  `pmix_tma_t` (see `pmix_obj_get_tma`'s convention).

## Writing a good case here

- **Name the defect, not the API.** A regression case should say what
  used to go wrong, so the next reader knows what breaking it means. The
  `/* Regressions */` blocks at the bottom of each file follow that
  shape.
- **Prefer a case that fails loudly.** The value-array zero-alloc case
  *hangs* without its fix, and the hotel static-init case takes SIGSEGV;
  both were verified by reintroducing the bug. A case that would only
  produce a subtly wrong number is worth less.
- **Do not bend a test to accommodate a bug.** If a case fails, the
  default assumption is that `src/class` is wrong. See the top-level
  guide.
- **Keep white-box pokes rare and justified.** A couple of cases reach
  into struct members (`va.array_alloc_size`, `ring.addr`,
  `base->obj_tma.tma_memmove`) because the state they describe is not
  reachable through the API. That is fine when the alternative is not
  testing a hang or an uninitialized field at all — but reach for the
  public call first.

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

The barrier is hand-rolled from a mutex and condvar because macOS has no
`pthread_barrier_t`. No per-target thread flags are needed —
`config/pmix_config_threads.m4` folds them into the global
`CFLAGS`/`LDFLAGS`/`LIBS`.

Each thread writes only its own slot of the payload array, so the
visibility check is not itself a data race. Keep it that way if you
extend it.
