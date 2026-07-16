<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PMIx Class System

This document orients AI agents and human contributors working in
`src/class`, the foundation on which nearly all of PMIx is built. It
assumes you have already read the top-level [`AGENTS.md`](../../AGENTS.md)
— the golden rules (prefix conventions, `pmix_config.h`-first include
order, constant-on-the-left comparisons, brace-everything, warning-free
under `--enable-devel-check`), the thread-safety / progress-thread model,
and the copyright-header requirement all apply here and are not repeated.
This file covers what is specific to `src/class`: the C object model that
gives the whole code base single-inheritance "classes" with
retain/release memory management, and the handful of container classes
built on top of it.

Unlike most directories under `src/`, **`src/class` is not an MCA
framework.** There is no framework header, no components, no selection
logic — just a small static library (`libpmix_class.la`) of headers and
`.c` files that are linked into `libpmix` and used everywhere. There is
correspondingly no `configure.m4` and no runtime plugin behavior to
reason about; changing anything here is a plain `make` away from being
compiled (see [Building](#building)).

## Why this code matters more than its size suggests

These files are tiny but load-bearing. `pmix_object.h` defines the object
model that *every* `pmix_*_t` in the tree participates in; `pmix_list_t`
appears in ~120 source files and `PMIX_LIST_FOREACH` is used ~400 times.
A subtle change to `PMIX_RELEASE`, to constructor/destructor ordering, or
to the list item ownership rules does not break one subsystem — it breaks
all of them, often as a use-after-free or leak that only shows up under
valgrind days later. Treat this directory as ABI- and
semantics-frozen unless you have a specific, measured reason and team
consensus. Match the existing patterns exactly.

## The object model (`pmix_object.{h,c}`)

This is the single most important file in the directory. It implements a
C-language object system with **single inheritance** and
**reference-count (retain/release) memory management**. Read the long
comment block at the top of `pmix_object.h` first — it is the canonical
tutorial. The essentials:

### Defining a class

A class is a struct whose **first member is its parent class**, plus a
statically-instantiated *class descriptor* named `<type>_class`:

```c
// in the .h
struct sally_t {
    parent_t parent;       // MUST be first — this is the inheritance
    void    *first_member;
};
typedef struct sally_t sally_t;
PMIX_CLASS_DECLARATION(sally_t);       // declares  extern pmix_class_t sally_t_class;

// in the .c
static void sally_construct(sally_t *s) { ... }
static void sally_destruct(sally_t *s)  { ... }
PMIX_CLASS_INSTANCE(sally_t, parent_t, sally_construct, sally_destruct);
```

Every class ultimately derives from **`pmix_object_t`**, the one class
that breaks the pattern: its descriptor (`pmix_object_t_class` in
`pmix_object.c`) is pre-marked initialized with no parent and no
constructor/destructor. A constructor or destructor may be `NULL` if the
class needs none.

### Creating and destroying instances

| Macro | Use for | Effect |
|-------|---------|--------|
| `PMIX_NEW(type)` | heap objects | `malloc` + set refcount to 1 + run constructors (parent→child) |
| `PMIX_RELEASE(obj)` | heap objects | decrement refcount; at 0 run destructors (child→parent), free, **and set `obj` to NULL** |
| `PMIX_RETAIN(obj)` | heap objects | increment refcount |
| `PMIX_CONSTRUCT(&obj, type)` | stack/static objects | initialize in place + run constructors (no allocation) |
| `PMIX_DESTRUCT(&obj)` | stack/static objects | run destructors (no free) |

Rules that trip people up:

- **There is no explicit `delete`/`free`.** Lifetime is entirely
  refcount-driven. `PMIX_RELEASE` frees only when the count hits zero.
- **`PMIX_RELEASE` nulls its argument.** It takes the variable, not a
  copy — that is why it is a macro, not a function. Do not
  `PMIX_RELEASE(some_expression)`.
- **Constructors run parent-first; destructors run child-last.** Each
  level's constructor should initialize only its own fields and assume
  the parent already ran. `pmix_class_initialize` (lazily, on first use)
  walks the parent chain to build the ordered constructor/destructor
  arrays cached in the class descriptor.
- **Never call `pmix_obj_run_constructors`/`_destructors` or
  `pmix_obj_new_tma` directly** — always go through the macros.
- Every class provides a **`PMIX_*_STATIC_INIT`** macro for
  file-scope/stack objects that must be usable before any constructor
  runs (e.g. `PMIX_LIST_STATIC_INIT`, `PMIX_HASH_TABLE_STATIC_INIT`).
  Prefer it to `PMIX_CONSTRUCT` for statics.

### Reference counting is mutex-based, not lock-free

Each `pmix_object_t` carries its own `pthread_mutex_t obj_lock`, and
`pmix_obj_update` takes it for every retain/release. This is deliberately
simple and correct, **not** a hot-path atomic. Do not sprinkle
`PMIX_RETAIN`/`PMIX_RELEASE` into tight loops on the assumption they are
free. In `--enable-debug` builds the mutex is `ERRORCHECK` (so a
recursive lock aborts loudly) and each object carries a magic-ID
(`PMIX_OBJ_MAGIC_ID`) plus the file/line of last construction; the
retain/release/destruct macros assert on the magic to catch
double-frees and wild pointers.

### The class registry and finalize

`pmix_class_initialize` records every initialized class's allocated
descriptor arrays in a global `classes[]` table (guarded by a private
`class_mutex`). `pmix_class_finalize()` frees them all. It is called
exactly once per role, as the **absolute last** thing before exit
(`src/server/pmix_server.c`, `src/client/pmix_client.c`,
`src/tool/pmix_tool.c`), purely so valgrind/purify report a clean
process. After it runs, every class is inoperable — the
`pmix_class_init_epoch` counter is bumped so any class touched again
lazily re-initializes. Do not call it from library code.

> Note: the comment above `pmix_obj_run_constructors` warns of a
> "hardwired maximum depth" of the inheritance tree. That is stale — the
> implementation allocates the constructor/destructor arrays dynamically
> from the computed hierarchy depth. Inheritance depth is not fixed in
> practice; correct the comment if you touch that code, don't design
> around a limit that isn't there.

### TMA: the touchable-memory allocator

Every object embeds a `pmix_tma_t obj_tma` — a struct of
`malloc`/`calloc`/`realloc`/`strdup`/`memmove`/`free` function pointers
plus context. When the pointers are `NULL` (the overwhelmingly common
case) the object uses libc `malloc`/`free`. When they are set, the object
and everything it allocates live in a **caller-provided memory arena** —
this is how the shared-memory datastore (`src/mca/gds/shmem2`, and
helpers in `src/util/pmix_hash.c`, `src/mca/bfrops/base/bfrop_base_tma.h`)
places PMIx objects into an mmap'd segment shared between processes.

What this means when working in `src/class`:

- `PMIX_NEW(type, tma)` and `PMIX_CONSTRUCT(&obj, type, tma)` take an
  optional trailing TMA argument (the variadic-macro machinery at the top
  of `pmix_object.h` dispatches on argument count). A child object created
  by a container must propagate the container's TMA.
- **Container `.c` code must allocate through the object's TMA, not raw
  libc.** The pattern is `pmix_tma_t *tma = pmix_obj_get_tma(&obj->super);`
  then `pmix_tma_malloc(tma, ...)` / `pmix_tma_free(tma, ...)` — these
  helpers fall back to libc when `tma == NULL`, so the same code serves
  both cases. `pmix_pointer_array.c` and `pmix_hash_table.c` are the
  reference examples; note that some classes here (list, ring buffer,
  value array, bitmap, hotel) predate TMA and still call libc directly,
  which is fine only because they are not stored in shared memory.
- `pmix_obj_get_tma` detects "has a custom allocator" by testing whether
  `tma_malloc` is non-NULL. Because of that convention, a partially
  filled-in TMA is a bug. Copy an existing static-init block verbatim.

## The container classes

All of the following are `pmix_object_t` subclasses; create/destroy them
with the object macros above. Each is a thin, well-understood data
structure — the value of reading the header is in the ownership and
threading contracts, which the compiler does not enforce.

### `pmix_list_t` / `pmix_list_item_t` — intrusive doubly-linked list

The workhorse container (~120 files). To put a thing on a list, make its
struct derive from `pmix_list_item_t` (first member). Key contracts,
mostly spelled out in the header's opening comment:

- **An item can be on at most one list at a time.** Adding an item to a
  second list without removing it from the first silently corrupts the
  first. `--enable-debug` builds carry a per-item refcount and
  `belong_to` pointer and *spot-check* this with asserts — but the checks
  are best-effort (the class deliberately uses no locks), so they reduce,
  not eliminate, the risk.
- **The list does not own its items.** Destructing/releasing a
  `pmix_list_t` leaves the items it held untouched (orphaned). Use
  `PMIX_LIST_DESTRUCT(list)` / `PMIX_LIST_RELEASE(list)` to release every
  contained item *and then* destruct/release the list itself.
- **Add/remove transfer ownership without refcounting.** `append`,
  `prepend`, `insert`, `remove_item`, `remove_first`/`_last` do **not**
  `PMIX_RETAIN`/`PMIX_RELEASE` the item — the caller hands ownership to
  the list and takes it back on removal.
- **Iteration:** `PMIX_LIST_FOREACH(item, list, type)` (and `_REV`) is the
  normal loop and is **not** safe against removing the current item;
  `PMIX_LIST_FOREACH_SAFE(item, next, list, type)` (and `_SAFE_REV`) is.
  Reaching for the plain macro inside a loop that removes items is a
  common and nasty bug.
- The list keeps an O(1) cached `pmix_list_length`; `pmix_list_splice` /
  `pmix_list_join` are O(N) only because they must fix that count.
- `pmix_list_sort` sorts via an array of pointers + `qsort`; its compare
  function receives **double pointers** (`pmix_list_item_t **`).

### `pmix_pointer_array_t` — index-addressable pointer table

A growable array that hands out **stable integer indices** for the
pointers you store, reusing freed slots. Originally built for Fortran
MPI-handle translation (hence `int` indices capped at `INT_MAX`), it is
now used broadly (~40 files) wherever an object needs a small integer
name. A companion `free_bits` bitmap makes "find lowest free slot" fast;
`lowest_free` is an optimization hint, **not** a guarantee that all
higher slots are taken. `pmix_pointer_array_get_item` is an inlined,
bounds-checked O(1) read. TMA-aware.

### `pmix_hash_table_t` — open-addressing hash table

Linear-probing hash table with backshift deletion (it rehashes the run
after a removed slot to keep lookups correct — see the worked example in
`pmix_hash_table_remove_elt_at`). Three key types, each with its own
get/set/remove/iterate family: **`uint32`**, **`uint64`**, and arbitrary
binary **`ptr`** (the table copies and owns the ptr key bytes; it never
owns the stored value). One table holds exactly **one key type at a
time** — `--enable-debug` asserts if you mix them (the check is skipped
for TMA tables, which may have stale method pointers across processes).
Grows when density crosses `ht_growth_trigger` (default 1/2, doubling).
Iterate with `PMIX_HASH_TABLE_FOREACH(key, uint32|uint64|ptr, val, ht)`;
do not remove during a foreach. TMA-aware.

### `pmix_hotel_t` — fixed-occupancy timeout slots

A "hotel" has a fixed number of rooms; an opaque pointer "checks in" to a
room (getting back a room number) and either checks out explicitly or is
**evicted by a libevent timer** after a timeout, firing a user callback.
The canonical use is ACK-based retransmission / pending-request tracking.
Deliberately minimal and inlined for the fast path (`pmix_hotel.h`);
`pmix_hotel.c` holds only init/construct/destruct and the eviction
callback. Two hard rules: the eviction callback must **not** call any
checkout function (the occupant is already checked out before it fires),
and the three copies of the checkout/eviction bookkeeping logic
(`pmix_hotel_checkout`, `pmix_hotel_checkout_and_return_occupant`,
`pmix_hotel.c:local_eviction_callback`) must be kept in sync — the header
says so at each site. Needs an event base; `#include <event.h>`.

### `pmix_ring_buffer_t` — fixed-size pointer ring

Push/pop/poke over a fixed ring of `size` pointers; pushing into a full
ring returns the displaced oldest entry (NACK-style schemes, per the
hotel header's cross-reference). Small and self-contained; not TMA-aware.

### `pmix_value_array_t` — by-value dynamic array

A growable array that stores elements **by value** (contiguous
`memcpy`-in, not pointers), so element size is fixed at
`pmix_value_array_init(&a, sizeof(elem))`. Get/set by index (auto-growing
on set), append, remove-with-shift. `PMIX_VALUE_ARRAY_GET_ITEM` /
`_SET_ITEM` / `_GET_BASE` are unchecked, fast, typed macros — the caller
owns bounds-checking. Not TMA-aware.

### `pmix_bitmap_t` — auto-expanding bit array

A bitmap over `uint64_t` words. `set_bit` auto-expands the array to fit
(bounded by an optional `max_size` that **must** be set before `init`);
`clear_bit`/`is_set_bit` return an error / false for out-of-range bits
rather than expanding. Provides find-first-unset-and-set, whole-bitmap
and/or/xor, population counts, copy, compare, and a `_get_string`
debugging dump. Not TMA-aware.

## Directory layout

```
src/class/
├── pmix_object.{h,c}          the object model: classes, PMIX_NEW/RETAIN/
│                              RELEASE/CONSTRUCT/DESTRUCT, TMA, lazy class init
├── pmix_list.{h,c}            intrusive doubly-linked list + FOREACH macros
├── pmix_pointer_array.{h,c}   index-addressable growable pointer table
├── pmix_hash_table.{h,c}      open-addressing hash table (u32/u64/ptr keys)
├── pmix_hotel.{h,c}           fixed-occupancy slots with timeout eviction
├── pmix_ring_buffer.{h,c}     fixed-size pointer ring
├── pmix_value_array.{h,c}     by-value contiguous dynamic array
├── pmix_bitmap.{h,c}          auto-expanding bitmap
└── Makefile.am                builds noinst libpmix_class.la; installs headers
```

Much of the behavior lives in `static inline` functions in the headers
(the containers are on hot-ish paths and inline aggressively), so read the
`.h` before assuming the `.c` tells the whole story — for `pmix_hotel` and
much of `pmix_list`, the header *is* the implementation.

## Threading

With one exception, these classes are **not internally synchronized**.
The exception is the `pmix_object_t` reference count, which is
mutex-protected so `PMIX_RETAIN`/`PMIX_RELEASE` are safe from any thread.
Everything else — list links, hash-table slots, pointer-array indices,
hotel rooms — assumes the caller provides serialization. In PMIx that
serialization is the **progress thread**: per the top-level thread-safety
rules, shared library state (which is overwhelmingly built from these
containers) is manipulated only on the progress thread, and public API
entry points thread-shift before touching it. The lists in particular are
documented as intentionally lock-free precisely because they are expected
to be used single-threaded-per-list. Do not add locking to these classes
to "make them thread-safe"; fix the caller to respect the progress-thread
model instead.

## Building

`src/class` compiles into `noinst` convenience library
`libpmix_class.la`, which is absorbed into `libpmix`. There is no
`configure.m4` and nothing is conditionally compiled, so a change here
always takes effect with a plain top-level `make` (from an already
configured tree) — see the top-level guide's "Test-building your changes"
section; you do not need `autogen.pl`/`configure` unless you add or remove
a source file in `Makefile.am` (adding a file only needs `make`; it will
regenerate the `Makefile`). Headers listed in `Makefile.am`'s `headers`
are installed into `$(pmixincludedir)/src/class`, so they are part of the
consumable internal surface — keep them clean.

## When adding or modifying code here

- **Adding a new class:** follow the object-model recipe above exactly —
  parent struct first, `PMIX_CLASS_DECLARATION` in the `.h`,
  `PMIX_CLASS_INSTANCE` in the `.c`, a `PMIX_*_STATIC_INIT` macro, and the
  standard copyright header. Add both files to `Makefile.am`
  (`headers`/`sources`) and, if the header should be internally
  consumable, it belongs in `headers`. Prefer making a new class
  TMA-aware from the start (allocate via `pmix_tma_*` off
  `pmix_obj_get_tma`) if there is any chance it will live in a datastore.
- **Changing an existing class:** assume it is used in dozens to hundreds
  of places and that its ownership/threading contract is relied upon. A
  change to constructor/destructor behavior, to what `PMIX_RELEASE` frees,
  or to list item ownership is effectively a tree-wide change — build the
  whole tree and run `make check` plus `test/simple/simptest`, and prefer
  valgrind (the object system's debug asserts and clean-shutdown
  `pmix_class_finalize` exist specifically to make leak/UAF detection
  meaningful).
- **Do not weaken the debug spot-checks.** The list refcount asserts and
  the object magic-ID checks catch real bugs. If one fires, the default
  assumption is that a *caller* violated the contract (an item on two
  lists, a double free), not that the check is wrong.
- **Keep it warning-free and portable.** This code runs on every platform
  PMIx supports and is compiled with `-Werror` in CI. Use the
  `__pmix_attribute_*__` wrappers and `PMIX_HIDE_UNUSED_PARAMS` rather
  than bare GCC attributes.
