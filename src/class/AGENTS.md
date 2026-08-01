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
  file-scope/stack objects that need a defined state before any
  constructor runs (e.g. `PMIX_LIST_STATIC_INIT`,
  `PMIX_HASH_TABLE_STATIC_INIT`). Read the box below before relying on
  one: they are **not** a substitute for `PMIX_CONSTRUCT`, and for
  `pmix_list_t` the statically initialized object is not even usable.

> ### A `PMIX_*_STATIC_INIT` object is tagged with the *base* class
>
> Every one of these macros expands to
> `PMIX_OBJ_STATIC_INIT(pmix_object_t)`, which sets `obj_class` to
> `&pmix_object_t_class` — **not** to the derived class's descriptor. Two
> consequences, neither obvious from the macro:
>
> - `PMIX_DESTRUCT` on such an object does **not** run the derived
>   destructor. It runs `pmix_object_t`'s (empty) chain. For a statically
>   initialized object that is the right answer — it owns nothing yet —
>   but it means a STATIC_INIT is not interchangeable with a construct.
> - `pmix_object_t_class` is pre-marked initialized, so
>   `pmix_class_initialize` never runs for it and never builds its
>   constructor/destructor arrays. Those arrays used to be `NULL`, and
>   both `pmix_obj_run_constructors` and `_destructors` walk them with
>   `while (NULL != *array)` — so `PMIX_DESTRUCT` (or a `PMIX_RELEASE`
>   reaching zero) on a never-constructed STATIC_INIT object
>   **dereferenced NULL and took SIGSEGV**. They are now empty,
>   terminated arrays and it is a correct no-op. Do not set them back to
>   `NULL`; `test/unit/class/class_object.c` pins this.
>
> The invariant to hold onto: a STATIC_INIT gives the object a *defined*
> state, and the matching `PMIX_CONSTRUCT` at init time is what makes it
> a live instance of its own class. Every in-tree use does both.

**When you add a `STATIC_INIT` macro, diff it against the constructor
field by field.** This has been wrong four separate times —
`PMIX_HOTEL_STATIC_INIT` disagreed about `last_unoccupied_room` (a live
bug: `pmix_globals.notifications` reported a free room before
`pmix_hotel_init` ran), `PMIX_POINTER_ARRAY_STATIC_INIT` had `max_size`
and `block_size` at 0 against the constructor's `INT_MAX` and 8 (a
divide-by-zero in `grow_table`), and `PMIX_LIST_ITEM_STATIC_INIT` had
`item_free` at 0 against the constructor's 1 (that field has since been
deleted as dead). Each class's unit test now compares the two structs
member by member; keep that up for new ones.

### Reference counting is a C11 atomic

`obj_reference_count` is a `pmix_atomic_int32_t`, and `pmix_obj_update()`
is a single `atomic_fetch_add_explicit(..., memory_order_acq_rel)`.
**PMIx requires C11 atomics — `config/pmix.m4` fails configure outright
without them — so there is never a question of whether they are
available.** Do not reintroduce a lock here, and do not propose a
fallback path for compilers that lack `<stdatomic.h>`; such a compiler
cannot build PMIx at all.

The ordering is `acq_rel` because the one function serves both
directions: the thread dropping the last reference must publish its
writes before the count reaches zero, and whichever thread *observes*
zero — and therefore runs the destructors and frees the storage — must
acquire them. Anything weaker races the destructor against the last
user. If you touch this, keep it `acq_rel`; a "relaxed is enough for a
refcount" simplification is only true for the increment.

This replaced a per-object `pthread_mutex_t obj_lock` taken and released
on every `PMIX_RETAIN`/`PMIX_RELEASE`, and fixed two defects with it:

- The mutex was `pthread_mutex_init`'d and **never destroyed**, so it
  leaked on any platform where init allocates.
- Objects allocated from a TMA live in a shared-memory segment, and the
  embedded mutex was never `PTHREAD_PROCESS_SHARED` — cross-process
  retain/release was undefined. An atomic `int32_t` in shared memory is
  well defined, which is what makes the TMA path below actually sound.

### Changing the layout of an object is a cross-version change

Dropping `obj_lock` also shrank `pmix_object_t` from **168 bytes to
104**, which moved every field of every derived class — `pmix_list_t`
went from 376 bytes to 248, and `pmix_list_length` from offset 368 to
240.

That is not a local matter. **`gds/shmem3` builds `pmix_list_t`,
`pmix_hash_table_t` and friends *inside a segment it shares with client
processes*** (that is what the TMA below exists for), and those clients
may be from an older release. Before the mitigation described next, an
older peer mapped the new server's segment, read every field at the
wrong offset, and segfaulted — which is what the `xversion` CI jobs
caught, against v3.2, v4.1, v4.2, v5.0 and master alike.

The mitigation is that the shared-memory component carries a **version
number in its name**, bumped in the *same change* as any layout shift:
`shmem2` became `shmem3` here. A client selects its `gds` module by
name, so an older peer does not recognize the new one and falls back to
`hash`. See
[`src/mca/gds/shmem3/AGENTS.md`](../mca/gds/shmem3/AGENTS.md) for why
this component cannot be versioned the way `bfrops` is, and why a
compatibility alias would defeat the whole mechanism.

**So, before changing the size or layout of `pmix_object_t` or of
anything derived from it: that is a cross-version compatibility change,
and it needs the shared-memory component renamed in the same commit
series.** This applies to *any* class here, not just the base — a field
added to `pmix_list_t` has exactly the same effect.

Two dead fields were deleted on top of the `shmem3` rename, before it
shipped, which moved the layout again (measured `--enable-debug`,
arm64):

| | before | after |
|---|---|---|
| `pmix_tma_t` | 64 | 56 |
| `pmix_object_t` | 104 | 96 |
| `pmix_list_item_t` | 136 | 128 |
| `pmix_list_t` | 248 | 232 |
| `pmix_list_length` offset | 240 | 224 |

**No further rename was needed, and the reason is worth understanding
rather than copying.** The `shmem2` → `shmem3` rename in this same
development cycle had not appeared in any release — every release branch
(`v5.0`, `v6.0`, `v6.1`) still shipped `shmem2`, and no tag contained the
rename. One rename covers every layout change made before the new name
ships, because what protects an old peer is not recognising the *name*.
Check that before assuming you get a free ride: `git tag --contains` the
rename commit and look at what the release branches actually carry. Once
`shmem3` is released, the next layout change needs `shmem4`.


In `--enable-debug` builds each object carries a magic ID
(`PMIX_OBJ_MAGIC_ID`) plus the file/line of last construction; the
retain/release/destruct macros assert on the magic to catch double-frees
and wild pointers. **`PMIX_DESTRUCT` on an already-destructed object
aborts on that assert, correctly** — a destructor that resets its class
to the constructed state (several here now do) makes a release build
survive it, but a double destruct is still a caller bug, not a supported
pattern. Do not write tests that do it.

### The class registry and finalize

`pmix_class_initialize` records every initialized class's allocated
descriptor arrays in a global `classes[]` table (guarded by a private
`class_mutex`). `pmix_class_finalize()` frees them all.

**Lazy class init is a double-checked lock, and the fast path is
unlocked.** Every `PMIX_NEW`/`PMIX_CONSTRUCT` tests
`PMIX_CLASS_IS_INITIALIZED(cls)` *without* taking `class_mutex`, and only
calls `pmix_class_initialize` when the class looks stale. That makes the
ordering rules load-bearing:

- `cls_initialized` and `pmix_class_init_epoch` are
  `pmix_atomic_int32_t`, not `int`.
- `pmix_class_initialize` publishes with a **release** store, and it does
  so **last** — after `cls_depth`, `cls_construct_array`,
  `cls_destruct_array` and their contents are all written.
- `PMIX_CLASS_IS_INITIALIZED` **acquires**. Unlocking the mutex is not
  enough to order this, because the reader never takes the mutex.

These were plain `int` accesses, which is the textbook broken
double-checked lock: a thread could observe the new epoch and then call
through a `cls_construct_array` it had no ordering against. ThreadSanitizer
reports seven data races inside `pmix_class_initialize` against the old
code and none against the current code; `class_refcount`'s
"class init race" case is what drives it (see
[`test/unit/class/AGENTS.md`](../../test/unit/class/AGENTS.md)). Note this
did **not** change `sizeof(pmix_class_t)` — a lock-free `_Atomic int32_t`
matches `int` in size and alignment — so external consumers with their own
`PMIX_CLASS_INSTANCE` are unaffected. If you touch this, keep the
release/acquire pair and keep the publish last. It is called
exactly once per role, as the **absolute last** thing before exit
(`src/server/pmix_server.c`, `src/client/pmix_client.c`,
`src/tool/pmix_tool.c`), purely so valgrind/purify report a clean
process. After it runs, every class is inoperable — the
`pmix_class_init_epoch` counter is bumped so any class touched again
lazily re-initializes. Do not call it from library code.

There is a real hazard buried in that "absolute last" requirement.
`pmix_obj_run_destructors` reads `object->obj_class->cls_destruct_array`
and does **not** re-check the epoch, so releasing *any* object after
`pmix_class_finalize()` has run dereferences a freed array. The epoch
counter protects re-*initialization*, not objects that are still alive.
That is why finalize is the last call in each role and why library code
must never call it.

### TMA: the touchable-memory allocator

Every object embeds a `pmix_tma_t obj_tma` — a struct of
`malloc`/`calloc`/`realloc`/`strdup`/`memmove`/`free` function pointers
plus context. When the pointers are `NULL` (the overwhelmingly common
case) the object uses libc `malloc`/`free`. When they are set, the object
and everything it allocates live in a **caller-provided memory arena** —
this is how the shared-memory datastore (`src/mca/gds/shmem3`, and
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
  filled-in TMA is a bug. Copy an existing static-init block verbatim,
  or better, call `pmix_obj_construct_tma()` — which is the *one* place
  that clears every `pmix_tma_t` member. `pmix_obj_new_tma()` used to
  carry its own copy of that code and cleared one member fewer, leaving a
  stray function pointer holding whatever `malloc` returned in every heap
  object in the library. Do not write a second copy.
- **`pmix_tma_t` has seven members and a TMA must fill in all of them.**
  It briefly had an eighth, `tma_memmove`, which was declared, cleared in
  three places, asserted clear by the unit test — and never assigned or
  called anywhere. There was no `pmix_tma_memmove()` helper beside the
  other five, so nothing could reach it; `gds/shmem3` filled in the other
  five and skipped it. It has been deleted (see the layout note above for
  why that was affordable). If you ever want it back, add the
  `pmix_tma_*` inline and wire up every TMA implementation in the same
  change — a member nothing sets is a member that reads as garbage the
  moment someone starts calling it.

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
- **`pmix_list_insert` rejects a negative index.** It used to check only
  the upper bound, so a computed index that went negative fell through
  the walk (which ran zero times) and landed the item at position 1,
  returning `true`.
- `pmix_list_item_t` briefly carried an `item_free` field that the
  constructor set and nothing ever read. It has been deleted along with
  `tma_memmove` (see the layout note above). If you find yourself wanting
  a per-item flag, note that the debug-only `pmix_list_item_refcount` /
  `pmix_list_item_belong_to` pair already answers "is this item on a
  list", and adding a field here is a layout change.
- **`pmix_list_insert` cannot append.** It inserts *before* an existing
  item, so `idx` must be strictly less than the length: the
  one-past-the-end position that would mean "append" is rejected, and an
  empty list rejects every `idx`. Use `pmix_list_append` (O(1)) for the
  back and `pmix_list_prepend` for the front. The header used to say
  "greater than the length" where the code means "greater than or
  equal"; it now says so plainly.

> ### `PMIX_LIST_STATIC_INIT` does not produce a usable list
>
> This is the sharpest edge in the directory and it is not obvious from
> the macro. `pmix_list_construct` makes the list empty by pointing the
> sentinel's `next` and `prev` **at itself** — an address a static
> initializer cannot name. So `PMIX_LIST_STATIC_INIT` leaves both links
> `NULL`, and on such a list `pmix_list_is_empty()` returns **false**
> (`NULL != &sentinel`), `PMIX_LIST_FOREACH` dereferences NULL, and
> `pmix_list_append` writes through a NULL `sentinel->prev`.
>
> The macro exists to give a file-scope list a *defined* state before
> init runs — so that an early failure path can destruct it — **not** to
> make it usable. Every one of the ~45 uses in the tree is followed by a
> `PMIX_CONSTRUCT(&list, pmix_list_t)` at init time; that is mandatory,
> not stylistic. If you add a statically initialized list, add the
> construct with it, and do not touch the list on any path that can run
> before it.
>
> The other `PMIX_*_STATIC_INIT` macros *do* produce a working (empty)
> object, so this is a `pmix_list_t` peculiarity. It is also why
> `PMIX_HOTEL_STATIC_INIT` having disagreed with the hotel constructor
> about `last_unoccupied_room` was a live bug rather than a cosmetic
> one: `pmix_globals.notifications` is built that way and reported a
> free room before `pmix_hotel_init` ever ran. When you add a
> `STATIC_INIT` macro, check it field-by-field against the constructor.

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

`ht_label` is a debugging label the table itself never sets and never
reads — but `src/util/pmix_hash.c` prints it at four sites as
`(NULL == table->ht_label) ? "UNKNOWN" : table->ht_label`. That NULL test
is the only thing between `%s` and a wild pointer, and the constructor
used to skip the field entirely: `PMIX_NEW` mallocs without zeroing, so
every table that does not set its own label (`gds/shmem3`'s, the mca
base's) carried garbage there. The constructor now clears it. If you add
a member to this struct, add it to the constructor in the same edit —
that is the whole defence.

`pmix_hash_table_init2` is a `PMIX_EXPORT` entry point taking two ratios,
and it now validates them. A zero `density_numer` divided by zero on its
first line and a zero `growth_denom` did the same inside
`pmix_hash_grow`; less loudly, a growth factor that does not grow, or a
density of 1/1 or looser, lets the table fill completely — and since
every get/set/remove probes with an unbounded `for (;; ii += 1)`, a full
table is an infinite loop rather than an error.

Two more things about this class that will surprise you:

- **A "get" writes to the table.** Every `pmix_hash_table_get_value_*`
  assigns `ht->ht_type_methods` before probing — that is how the table
  learns its key type. So there is no read-only path: two threads
  reading concurrently is a data race, and a table mapped read-only in
  a shared segment would fault on a lookup. If you ever need a genuinely
  const lookup, that assignment is what has to move.
- **The "one key type per table" rule is enforced by one pointer
  compare, and that compare used to be debug-only.** It sat inside
  `#if PMIX_ENABLE_DEBUG`, so in every build that ships, mixing key
  types was *silent*: the entry point simply retargeted
  `ht_type_methods`, after which a ptr-keyed table had no
  `elt_destructor` — every copied key leaked — while `pmix_hash_grow`
  and `pmix_hash_table_remove_elt_at` rehashed its elements through
  `key.u32`, i.e. the low bytes of a key *pointer*, scattering live
  entries to slots no later lookup would probe. It is unconditional
  now; only the `pmix_output` is still behind the switch.

  **The TMA exemption in that check is load-bearing and must stay.**
  `ht_type_methods` points at a file-scope static in whichever process
  last touched the table, and that address means nothing to a peer that
  has mapped the same `gds/shmem3` segment — a peer comparing it against
  its own would refuse every lookup of a table it can read perfectly
  well. The condition is `NULL == pmix_obj_get_tma(&ht->super) && ...`
  for exactly that reason. Making the check unconditional is what turned
  this from documentation into a live dependency: before, an optimized
  build had nothing for a TMA table to be exempt *from*.
  `test/unit/class/class_tma_shared` pins it, and deleting the exemption
  makes that program fail to build its shared state at all.
- **The ptr family validates its key.** A NULL key walks straight into
  `pmix_hash_hash_key_ptr()` and is dereferenced; a zero `key_size`
  makes every key compare equal to every other (`memcmp` of 0 bytes is
  0) and asks the allocator for a 0-byte block, whose answer is
  implementation-defined — NULL on some platforms, which the setter
  would have reported as an out-of-memory that never happened. All three
  ptr entry points now return `PMIX_ERR_BAD_PARAM` for either.
- Every entry point starts with `key % capacity`, and capacity is zero
  until `pmix_hash_table_init` runs. The guard that catches that used to
  be `#if PMIX_ENABLE_DEBUG`, so an optimized build took a SIGFPE where
  a debug build returned an error. It is unconditional now. **Be very
  careful about putting a correctness guard inside `PMIX_ENABLE_DEBUG`
  in this directory** — the diagnostic `pmix_output` belongs there, the
  check usually does not.

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

**Its consumers are outside this repository.** Nothing in `src/`,
`test/` (beyond its own unit test), `bindings/` or `examples/` uses this
class, but projects that build against PMIx's installed internal headers
do. Do not read the empty in-tree grep as "dead code" and do not propose
removing it — this class is part of the consumable internal surface and
is on the same do-not-break footing as everything else here.

That external-only usage is exactly why its class descriptor was missing
`PMIX_EXPORT` for so long. `pmix_ring_buffer_t_class` is hidden in any
build that does not pass `--disable-visibility`, so
`PMIX_NEW(pmix_ring_buffer_t)` failed to link for precisely the callers
that exist — and nothing in this tree could notice, because the one
in-tree consumer is a unit test normally built against a
`--disable-visibility` tree. Keep that in mind for the whole directory:
**an in-tree grep is not evidence that a class is unused.**

Note also that `pmix_ring_buffer_push`'s tail bookkeeping is only
correct because `tail == head` whenever the ring is full. That invariant
is not asserted anywhere. Do not "simplify" that function without
convincing yourself of it.

**`pmix_ring_buffer_push` was the one accessor with no guard at all.**
`pop` has always tested `tail == -1` and `poke` `size <= i`, but `push`
went straight to `addr[ring->head]` — so a ring that had only been
`PMIX_NEW`'d or `PMIX_CONSTRUCT`'d, or one already destructed,
dereferenced the NULL `addr` instead of declining. Given this class's
consumers are out of tree (see above), that is the least forgivable place
to leave a hole. It now returns NULL when there is no storage, which is
the same answer as "the ring was not full" — an unfortunate conflation,
but the only value the signature has to give, and strictly better than
the fault. All three accessors also tolerate a NULL ring now.

`tail == -1` is this class's "nothing has been pushed yet" flag, and it is
the field the teardown and re-init paths kept forgetting: the destructor
freed `addr` and zeroed `size` but left `head`/`tail` alone, so
`pmix_ring_buffer_pop` on a destructed ring took its "something is on the
ring" branch and dereferenced the NULL `addr` the destructor had just
installed; and `pmix_ring_buffer_init` never set them at all, so
re-initializing a used ring both leaked the old storage and handed back a
"fresh" ring still claiming the old one's occupancy. Both now reset the
pair, and `PMIX_RING_BUFFER_STATIC_INIT` (added alongside) spells `tail`
as `-1` rather than the `0` a plain zero-initializer would give.

### `pmix_value_array_t` — by-value dynamic array

A growable array that stores elements **by value** (contiguous
`memcpy`-in, not pointers), so element size is fixed at
`pmix_value_array_init(&a, sizeof(elem))`. Get/set by index (auto-growing
on set), append, remove-with-shift. `PMIX_VALUE_ARRAY_GET_ITEM` /
`_SET_ITEM` / `_GET_BASE` are unchecked, fast, typed macros — the caller
owns bounds-checking. Not TMA-aware. `pmix_value_array_init` rejects a
zero `item_sizeof` (every address in the class is
`index * array_item_sizeof`, so a zero collapses the array onto one
slot), and holds the grown buffer aside rather than assigning a failed
`realloc` back over the live one — the correction `reserve` and
`set_size` already carried.

**`pmix_value_array_reserve` was the last member of the
init/reserve/set_size trio still missing that zero-item-size check, and
it is the worst place to miss it.** On an array that has only been
`PMIX_CONSTRUCT`'d, `array_item_sizeof` is 0, so `reserve` asked
`realloc()` for `0 * size` bytes — and glibc answers `realloc(NULL, 0)`
with a *non-NULL* pointer to a zero-byte block. `reserve` then reported
success and committed `array_alloc_size = size` over that empty block,
after which `set_size` saw enough capacity to skip growing and every
element write went into the heap past the end of it. When you add a
guard to one member of a family here, check the siblings in the same
edit; this directory has now had the same omission four times.

### `pmix_bitmap_t` — auto-expanding bit array

A bitmap over `uint64_t` words. `set_bit` auto-expands the array to fit
(bounded by an optional `max_size` that **must** be set before `init`);
`clear_bit`/`is_set_bit` return an error / false for out-of-range bits
rather than expanding. Provides find-first-unset-and-set, whole-bitmap
and/or/xor, population counts, copy, compare, and a `_get_string`
debugging dump. Not TMA-aware.

`pmix_bitmap_set_max_size` was the one entry point in the file that did
not validate its argument. Because the stored cap is a *word* count
converted with `((size_t) max_size + 63) / 64`, a negative bit count
became `(size_t) -1`, whose `+ 63` wrapped round to 62, which divided
down to a stored cap of **zero words** — the call reported success and
every later `init`/`set_bit` then failed with `PMIX_ERR_BAD_PARAM` a long
way from the call that actually got it wrong. It rejects `max_size <= 0`
now.

`max_size` is a public *bit* count on the way in and a **word** count once
stored (`pmix_bitmap_set_max_size` converts); the constructor and
`PMIX_BITMAP_STATIC_INIT` both leave it at `INT_MAX`, i.e. uncapped. Two
things here were the directory's standing anti-patterns in miniature:
`pmix_bitmap_set_bit`'s growth path assigned a failed `realloc` straight
back into `bm->bitmap`, leaking the old block and leaving a NULL pointer
behind a non-zero `array_size` for every later accessor to index (the same
correction `pmix_bitmap_init` and `pmix_bitmap_copy` had already had); and
`pmix_bitmap_num_set_bits`'s NULL/negative-length guard sat inside
`#if PMIX_ENABLE_DEBUG`, so an optimized library read `bm->array_size`
off a NULL pointer. Both are unconditional now.

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

test/unit/class/               one make-check program per class, plus
                               class_refcount for the concurrent count and
                               class_tma_shared for the cross-process TMA
                               path; see its AGENTS.md
contrib/dockerswarm/run-class-tests.sh
                               runs that suite --disable-debug with
                               default visibility, on Linux
```

Much of the behavior lives in `static inline` functions in the headers
(the containers are on hot-ish paths and inline aggressively), so read the
`.h` before assuming the `.c` tells the whole story — for `pmix_hotel` and
much of `pmix_list`, the header *is* the implementation.

## Testing

Every class here has a unit test under
[`test/unit/class`](../../test/unit/class), wired into `make check` and
therefore into CI. Read
[`test/unit/class/AGENTS.md`](../../test/unit/class/AGENTS.md) before
adding to them — in particular for what those tests deliberately do
*not* do, and for the configuration trap described there.

The short version of that trap, because it caused several of the bugs
this directory has had:

- The maintainer's tree — and every tree `contrib/dockerswarm/build.sh`
  produces — is configured `--enable-debug --enable-devel-check`, and
  usually `--disable-visibility` too. A guard placed inside
  `#if PMIX_ENABLE_DEBUG` is therefore **never exercised in the
  configuration where its absence bites**, and a missing `PMIX_EXPORT`
  cannot fail to link.
- `contrib/dockerswarm/run-class-tests.sh` exists to close that gap: it
  builds and runs the suite `--disable-debug` with default visibility,
  on Linux. It found the missing `PMIX_EXPORT` on
  `pmix_ring_buffer_t_class` on its first run. Run it after any
  non-trivial change here.
- A multi-*node* test is not meaningful for this directory — these are
  single-process data structures. The genuinely distributed dimension is
  the cross-process TMA path, and that now has a direct test:
  `test/unit/class/class_tma_shared` builds a `pmix_hash_table_t`, a
  `pmix_pointer_array_t` and a reference-counted object inside a real
  `MAP_SHARED` segment and checks from a second process that the storage
  and the count are genuinely shared. It is in `make check` and, being
  multi-process, is the one member of the suite the swarm's ten-node
  contention pass says something about. It stops short of segment
  negotiation and attach between *unrelated* processes — its second
  process comes from `fork()` — which stays with `gds/shmem3` and the
  group tests in `contrib/dockerswarm/run-tests.sh`.

## Threading

With one exception, these classes are **not internally synchronized**.
The exception is the `pmix_object_t` reference count, which is a C11
atomic so `PMIX_RETAIN`/`PMIX_RELEASE` are safe from any thread and from
any process sharing a TMA segment (`class_refcount` and
`class_tma_shared` pin those two halves respectively). That is the whole
of the guarantee:
the count is safe, the *object* is not. Two threads that both hold a
reference still race on every field below `pmix_object_t`.

Everything else — list links, hash-table slots (including on a *read*,
see above), pointer-array indices, hotel rooms — assumes the caller
provides serialization. In PMIx that
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
  lists, a double free, a second `PMIX_DESTRUCT`), not that the check is
  wrong. That applies to tests too: if a new test trips an assert, fix
  the test.
- **Put diagnostics under `PMIX_ENABLE_DEBUG`, not checks.** A guard
  that prevents a divide-by-zero, an out-of-bounds `memmove`, or a
  bogus size computation belongs in every build; only the accompanying
  `pmix_output` belongs inside the `#if`. Several defects in this
  directory were exactly this mistake, and they are invisible in the
  configuration everyone develops in.
- **A bare `assert()` is a debug-only check too.** `config/pmix.m4` adds
  `-DNDEBUG` to `CFLAGS` whenever `--enable-debug` is off, so every
  `assert` in this directory compiles to nothing in every build that
  ships. That is fine for a spot-check on an invariant the code
  establishes itself; it is not fine as the only thing standing between a
  caller's argument and an out-of-bounds write.
  `pmix_pointer_array_test_and_set_item` guarded its index with
  `assert(index >= 0)` alone, so a negative index wrote below the start
  of `addr` in exactly the builds that matter — while its sibling
  `pmix_pointer_array_set_item` had always rejected it outright. The
  hotel's three room-number accessors had the same shape and were worse:
  `pmix_hotel_checkout`, `pmix_hotel_checkout_and_return_occupant` and
  `pmix_hotel_knock` each tested the *lower* bound with an `if` and the
  *upper* bound with `assert(room_num < hotel->num_rooms)`, so a shipped
  build did not check it at all — and `checkout` then **wrote** through
  `rooms[room_num]` past the end of the array and pushed a second
  out-of-bounds write into `unoccupied_rooms[]` via
  `++last_unoccupied_room`. A destructed hotel (`num_rooms` 0, `rooms`
  NULL) reached the same place through room 0, and every caller passes a
  cached `cd->room`. All three now test both bounds unconditionally. If
  the check is about *caller input*, write an `if` and return an error —
  and wrap it in `PMIX_UNLIKELY()` so the branch predictor pays for the
  path that never happens, not the one that always does.
- **Leave objects in the constructed state when you destruct them.**
  The destructors here now do (`pmix_list_destruct` always did — it
  calls the constructor). Freeing a pointer and leaving it in the struct
  behind a non-zero size is what turns one caller mistake into a
  double free or a read of freed memory.
- **Watch the width of shift operands.** The bitmap words are
  `uint64_t`, so mask shifts run to 63. `1UL << offset` is undefined
  above 31 where `unsigned long` is 32 bits (32-bit Linux, Windows) and
  `1LL << 63` overflows a signed 64-bit type. Write `((uint64_t) 1) <<`
  or `1ULL <<`.
- **Mark new class descriptors `PMIX_EXPORT`.** Write
  `PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_foo_t);`. Without it the
  descriptor is hidden outside `libpmix` and `PMIX_NEW(pmix_foo_t)` will
  not link for any consumer — a failure you cannot see locally if your
  tree is configured `--disable-visibility`.

  **The rule, stated as a boundary:** every class declared in an
  **installed** header must have an exported descriptor. An installed
  header *is* the consumable surface — `PRRTE` has no object system of
  its own and drives PMIx's directly (hundreds of `PMIX_NEW` sites, plus
  its own classes declared with `PMIX_CLASS_DECLARATION`) — so a class
  you can `#include` but not link is simply broken as shipped. Classes in
  headers that are *not* installed should stay hidden; exporting those
  only grows the dynamic symbol table and wrongly advertises them as
  consumable. The exception is a class an in-tree `test/unit` program
  needs, since those link against `libpmix` from outside it too
  (`pmix_gds_base_active_module_t` is the one such case today).

  **Two places can do the exporting, and that is why this is hard to
  audit.** Either the declaration in the header:

  ```c
  PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_foo_t);   /* pmix_foo.h */
  ```

  or the definition in the `.c`:

  ```c
  PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_foo_t, ...); /* pmix_foo.c */
  ```

  Either one gives the symbol default visibility, so **grepping the
  headers overcounts what is actually broken** — of 59 bare declarations
  in installed headers, only 31 were genuinely hidden; the rest were
  exported at the definition site. Prefer the declaration form (it is
  what the header promises the reader), but when you want the truth, ask
  the library rather than the source:

  ```sh
  nm -m src/.libs/libpmix.dylib | grep '_class$' | grep non-external   # macOS
  readelf -sW src/.libs/libpmix.so | grep '_class$' | grep LOCAL       # Linux
  ```

  Do that against a tree configured **without** `--disable-visibility`,
  or every symbol looks fine.

  **And determine "installed" from the install tree, not from the
  `Makefile.am`s.** Headers reach `$(pmixincludedir)` through several
  variables and conditional blocks, and a hand-rolled parse of the
  `headers =` lists undercounts badly — it missed 36 of the 115 headers
  PMIx actually installs, including `src/server/pmix_server_ops.h`,
  `src/event/pmix_event.h` and every framework `base/base.h`. Just
  `make install` to a scratch prefix and look:

  ```sh
  find $PREFIX/include/pmix -name '*.h' | sed "s|$PREFIX/include/pmix/||"
  ```

  As of this sweep, 134 class descriptors are defined in `libpmix`, 103
  are exported, and the 31 that remain hidden are correctly hidden: 27
  are private to a single `.c` (no header declaration at all) and 4 are
  `gds/hash` component internals (`pmix_session_t`, `pmix_job_t`,
  `pmix_apptrkr_t`, `pmix_nodeinfo_t`) whose header is not installed. No
  class declared in an installed header is hidden.
- **Keep it warning-free and portable.** This code runs on every platform
  PMIx supports and is compiled with `-Werror` in CI. Use the
  `__pmix_attribute_*__` wrappers and `PMIX_HIDE_UNUSED_PARAMS` rather
  than bare GCC attributes.
