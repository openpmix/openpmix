The Shared-Memory Datastore: ``gds/shmem3``
===========================================

This document describes the shared-memory datastore — how a PMIx server
builds job, session and modex data *inside* an ``mmap``'d segment, how
every local client reads that same segment in place, how data that
cannot be overwritten is nevertheless updated, and why none of it takes
a lock. It closes with a step-by-step walk of ``PMIx_Get``, from the
public entry point through the pre-computation the library does before
it looks anything up, to the read of the shared segment itself, naming
each thread shift along the way.

It is a companion to :doc:`modex`, which follows a value from
``PMIx_Put`` to the aggregated result; this one is about where that
result *lives*. For a contributor-facing orientation inside the code,
``src/mca/gds/shmem3/AGENTS.md`` covers the component directory and
``src/mca/gds/AGENTS.md`` the framework around it.


The Problem
-----------

A node running many processes of the same job holds the same job data
in every one of them. With the ``hash`` datastore, each client unpacks
the job-level blob its server sends and builds its own hash tables from
it; the modex is worse, because after a collecting fence every local
client holds a full copy of what every process in the job published. On
a fat node that is the largest single block of memory PMIx holds,
replicated once per rank, and every byte of it was unpacked and stored
per rank as well.

``shmem3`` removes both costs at once. The server builds the *actual*
``pmix_list_t``, ``pmix_hash_table_t`` and ``pmix_kval_t`` structures
inside a shared-memory segment, and each local client maps that segment
**at the same virtual address** the server used. N clients then share
one physical copy, and a client's "store" of job data is an ``mmap``
rather than an unpack-and-rebuild.

Two consequences drive everything else in this document:

* Because the structures are read *in place*, the two ends share a
  **memory layout**, not a wire format. Any disagreement about the size
  or shape of those types repoints every field a reader touches.
* Because a segment is mapped by processes the writer does not control,
  **a segment a client can see is never written again**. Updating data
  therefore means publishing a *new* segment, not editing an old one —
  which is what the chains described below exist for, and what makes
  lock-free reading possible.


Scope and Roles
---------------

* The **server** is the sole writer. It creates each segment, allocates
  the structures in it, fills them, and only then makes it readable.
* A **client** is a reader. It maps segments its server describes to it
  and reads them; its mapping of the data region is made read-only by
  the MMU, so this is enforced rather than assumed.
* A PMIx server is also a client of itself — ``pmix_globals.mypeer`` has
  its own datastore module — but that module is pinned to ``hash``, so a
  server's *own* ``PMIx_Put``/``PMIx_Get`` traffic does not go through
  a segment. What goes through a segment is the data the server holds
  *on behalf of its local clients*.

Where the code lives:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - File
     - Contents
   * - ``src/util/pmix_shmem.[ch]``
     - Segment create/attach/detach/protect, the segment header and its
       layout stamp, the reference count.
   * - ``src/util/pmix_vmem.[ch]``
     - Locating a free address range, and reserving one.
   * - ``src/mca/gds/shmem3/gds_shmem3.h``
     - The trackers, the ``sm*_data_t`` structures that live *in* a
       segment, the layout ID.
   * - ``gds_shmem3.c``
     - The TMA allocator, segment placement/creation/attach, the arena,
       the seg-blob pack/unpack, the module entry points.
   * - ``gds_shmem3_store.c``
     - Writing job and session data into a segment through the TMA.
   * - ``gds_shmem3_fetch.c``
     - The read side: ``pmix_gds_shmem3_fetch()`` and the chain walks.
   * - ``gds_shmem3_utils.c/.h``
     - Tracker lookup, the chain publish/head primitives, per-segment
       status helpers.


When ``shmem3`` Is Selected
---------------------------

Two gates must both pass.

**Build gate** (``configure.m4``). The component is compiled only on a
64-bit, non-Apple host. It depends on a large virtual address space and
on the ``/proc/self/maps``-based hole finder, so on macOS or a 32-bit
target it is not built at all. A change made on a Mac has not been
compiled until it has been built on Linux.

**Runtime gate** (``component_query``). Even where built, the component
disqualifies itself (priority ``0``) if ``/proc/self/maps`` is not
accessible. Otherwise it bids priority **20**, against ``hash``'s 10,
so it wins by default.

``assign_module`` bids 20 normally, **100** when the caller named
``PMIX_GDS_MODULE="shmem3"``, and **0** when the caller named some other
module. The server hands the selected module's *name* to each client it
forks in ``PMIX_GDS_MODULE``, so client and server agree by name.

The module deliberately leaves ``store``, ``assemb_kvs_req`` and
``accept_kvs_resp`` ``NULL``. The framework macros detect the ``NULL``
slot on a non-``hash`` module and route those operations to the local
server's own module: ``shmem3`` owns the bulk job/modex data path, and
individual put/get traffic falls back. ``cache_job_info`` likewise
returns ``PMIX_ERR_NOT_SUPPORTED`` — unlike ``hash``, ``shmem3`` does not
pre-cache, it builds the segment lazily inside ``register_job_info``
when the first client connects.


The Name and the Stamp
----------------------

The version suffix in ``shmem3`` is load-bearing. A client selects its
module by name, so a client from an older release that recognizes the
name will map the segment — whatever the segment now contains. Since
what is shared is a memory layout, a change to ``pmix_object_t`` or to
anything derived from it silently repoints every field an older peer
reads, and the observed failure is a segfault a long way from the change
that caused it. So each generation *replaces* its predecessor: when the
layout changes, the number is bumped, older peers no longer recognize
the name, and ``hash`` — which never disqualifies itself — wins for them.
The component has been renamed twice for exactly this reason.

Renaming guards *released* peers. It cannot guard two builds that both
call themselves ``shmem3`` and still disagree, and the most common such
case needs no code change at all: ``--enable-debug`` adds
``obj_magic_id`` to the front of ``pmix_object_t`` and two fields to the
back, so a debug build and a default build of the same commit disagree
by 24 bytes in the base class alone.

Every segment therefore carries a **layout stamp** in its header:

.. code-block:: c

   #define PMIX_GDS_SHMEM3_LAYOUT_ID                                     \
       ((uint32_t)(                                                      \
           PMIX_GDS_SHMEM3_LAYOUT_VERSION                                \
         +   3u * (uint32_t)sizeof(pmix_object_t)                        \
         +   5u * (uint32_t)sizeof(pmix_list_t)                          \
         /* ... one term per type that lives in a segment ... */         \
       ))

It is **computed, not maintained** — a hand-maintained version number is
one somebody has to remember to bump — and the distinct prime
multipliers keep a growth in one type from cancelling a shrink in
another. ``pmix_shmem_segment_attach()`` refuses a segment whose stamp
differs, returning ``PMIX_ERR_NOT_SUPPORTED``; ``shmem3_attach()`` turns
that into ``PMIX_ERR_TAKE_NEXT_OPTION`` and the client quietly uses
``hash``.

The header that holds the stamp is fixed-width and free of any
class-derived type or debug conditional, on purpose: a stamp that can
itself shift cannot detect a shift.


Anatomy of a Segment
--------------------

A segment is a file under the session/nspace tmpdir, mapped
``MAP_SHARED``. Its layout::

    +--------------------------------------------------+  <- hdr_address
    | pmix_shmem_header_t                              |
    |   ref_count (atomic), magic, layout_id           |
    |   ... padded out to a page boundary ...          |
    +--------------------------------------------------+  <- data_address
    | shared_{job,session,modex}_data_t                |
    |   tma            (the allocator's state)         |
    |   current_addr   (the bump pointer)              |
    |   <the roots: lists, hash table, key index>      |
    +--------------------------------------------------+
    | everything those roots point at, carved out      |
    | of the same segment by the bump allocator        |
    |                                                  |
    |                        ...                       |
    +--------------------------------------------------+

The header is deliberately outside the data region. It carries the
**reference count** that attach and detach maintain, which is what lets
one generation of a segment be handed off while readers are still on it:
the backing file survives until the last holder lets go, and dropping
the last reference unlinks it.

.. important::

   The ``size`` a client must attach with is the **mapped footprint** —
   the value the creator's handle carried after
   ``pmix_shmem_segment_create()`` returned, which is
   ``pmix_shmem_utils_segment_footprint()`` of the requested data size,
   not the requested size. Pass the smaller number and the mapping ends
   a page short of the data region, which nothing can detect until a
   reader faults on the tail. That is why the creator's ``shmem->size``
   goes on the wire.


The allocator
^^^^^^^^^^^^^

Everything in a segment is allocated by a **bump allocator**, the
``pmix_tma_t`` ("temporary memory allocator") wired up in
``gds_shmem3.c``. ``PMIX_NEW(type, tma)`` and the ``pmix_tma_*``
routines carve from ``current_addr`` and advance it, 8-byte aligned;
``free`` is a **no-op**.

Two properties are easy to undo by accident:

* **Every block carries a 16-byte header** immediately ahead of the
  address handed out — the extent plus a magic. Its *only* consumer is
  ``tma_realloc()``, which needs the old size to know how much to copy.
  Nothing in the segment is reached by walking allocations, so this is
  not part of the shared layout. It must stay a multiple of 8, or every
  object in the segment is misaligned. It replaced a side hash table
  mapping address to extent, which cost two heap allocations and a
  pointer-keyed hash insert on *every* allocation and only ever grew.

* **Nothing zeroes a block, and nothing needs to.** A bump allocator
  only hands out space no caller has touched, and
  ``pmix_shmem_segment_create()`` opens its backing file
  ``O_CREAT | O_TRUNC`` and ``ftruncate``\ s it from empty — so every
  page of a fresh segment reads as zero. The ``O_TRUNC`` is load-bearing
  rather than tidiness: a backing path collides only with a file some
  earlier server left behind when it died, and paths are built from pids,
  which get reused.

Overflowing a segment ``abort()``\ s the process with a guidance
message. There is no growing: the pre-sizing arithmetic plus the
``segment_size_multiplier`` MCA parameter is the only guard, so segments
are sized generously.

.. warning::

   Never ``PMIX_CONSTRUCT`` an object destined for shared memory.
   ``PMIX_CONSTRUCT`` records the constructing process's addresses into
   the object; shared objects must be built through the TMA
   (``PMIX_NEW(type, tma)``) so their addresses live in the segment.
   This is the single easiest way to corrupt the store.


The three kinds of segment
^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 14 26 60

   * - ID
     - Root structure
     - Holds
   * - ``JOB``
     - ``shared_job_data_t``
     - ``local_hashtab`` (all job-level and per-rank job data),
       ``nodeinfo`` and ``appinfo`` lists, and the segment's key index.
   * - ``SESSION``
     - ``shared_session_data_t``
     - ``sessioninfo`` and ``nodeinfo`` lists for the session. Shared
       between every job in that session.
   * - ``MODEX``
     - ``shared_modex_data_t``
     - ``hashtab`` holding one collecting fence's aggregated result, and
       the segment's key index.

Each is placed, created and attached by
``shmem3_segment_create_and_attach()``, and each carries its own
``pmix_tma_t``: an allocation is always made against the segment the
object will live in.


Hash tables here are keyed by rank
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Both ``local_hashtab`` and the modex ``hashtab`` hold **one element per
rank**. ``pmix_hash_store()`` looks up a single ``pmix_proc_data_t`` for
the rank and hangs that rank's values off it in a pointer array; the
values are not table elements. Job-level keys all share the one element
belonging to ``PMIX_RANK_WILDCARD``.

That makes two different numbers matter when pre-sizing a segment, and
they are not interchangeable — the *element* count sizes the table and
the per-rank structures, while the *key/value pair* count sizes the
stored values and the key index. Feeding the pair count into the table
is the original bug in this arithmetic: a 32-rank job built a table with
tens of thousands of elements.

The sizing code asks the code that spends the memory rather than
restating it. Four entry points exist for that, each living beside what
it describes:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Ask
     - For
   * - ``pmix_hash_table_sizeof_storage(n)``
     - the element array ``pmix_hash_table_init(ht, n)`` allocates
   * - ``pmix_hash_sizeof_proc_storage()``
     - one rank's per-proc object and its two pointer arrays
   * - ``pmix_keyindex_sizeof_storage(nkeys)``
     - a key index sized for ``nkeys``
   * - ``pmix_hash_sizeof_key_entry(len)``
     - one newly registered key

Do not open-code any of them here. And whatever is put in a segment must
be in that segment's estimate *before* it is put there: the allocator
cannot grow, so an index that rehashes runs off the end of the segment
and aborts the server partway through ``register_job_info()`` — which
presents as every client hanging forever for job data that is never
coming, not as a crash on the process that actually failed.


Every segment carries its own key index
---------------------------------------

Inside the datastore a key is not a string but an integer (see
:doc:`modex`, "The Key Index"). In a shared segment that integer
crosses a process boundary: the server writes it, every local client
reads it. The two ends agree in two different ways, and only one of them
needs anything stored in the segment.

* **Reserved attributes agree by construction.** Their ids come from the
  generated dictionary and are pinned in ``contrib/dictionary_ids.txt``
  precisely so that they are the same in every process and every
  release. Nothing has to be carried, so nothing is: ``lookup_key()``
  resolves any id below ``PMIX_INDEX_BOUNDARY`` against the
  process-global index instead.

* **Non-reserved keys do not agree.** A key the Standard does not define
  is numbered on first encounter, in an order that depends on what
  arrived and when. Those go in the segment's own index, beside the data
  they describe, where they only have to be consistent within that one
  segment — which they are, because exactly one process writes it.

So ``job->smdata->keyindex`` and ``job->smmodex->keyindex`` are allocated
in their segments through the segment's TMA and hold the non-reserved
keys only. A private index numbers from ``PMIX_INDEX_BOUNDARY`` upward,
so the id itself says which half it came from.

.. important::

   A client maps the segment read-only, so a reader must reach these
   only through the non-registering lookups — ``pmix_hash_find_key()``,
   or ``pmix_hash_lookup_key()`` with a known index. Registering on a
   lookup would mean a reader writing into the segment, and a key that
   was never stored cannot be found anyway.

This replaces an earlier scheme in which the server shipped its whole
key index in the job-info reply and the client renumbered its own global
index to match. Putting the index next to the data removes the
requirement rather than satisfying it; there is no key-index blob on the
wire any more, and it should not come back. If you find yourself needing
to reconcile two dictionaries across a process boundary, put the index
next to the data instead.


The Fixed-Address Contract
--------------------------

In-segment pointers are stored raw. They are valid in a client only
because the client maps the segment at the **same virtual address** the
server used, so no fix-up is needed on attach. The server packs, per
segment, a *seg blob* naming the backing path, the mapped size, and the
header address, and the client maps there.

Choosing an address that a *different* process can also obtain is the
hard part.

Placement
^^^^^^^^^

``pmix_vmem_find_hole()`` parses ``/proc/self/maps`` and locates the
biggest hole in the address space. Two refinements matter:

* **Not the midpoint.** ``VMEM_HOLE_BIGGEST`` aims at the middle of the
  biggest hole, and hwloc and Open MPI — which share this routine's
  ancestry — aim there too. That convergence is exactly what makes an
  address picked in one process likely to be taken in another.
  ``VMEM_HOLE_BIGGEST_OFFSET`` (the ``offset_placement`` parameter, on
  by default) lands a **quarter** of the way in instead: far from the
  crowded midpoint, far from the populated region near the binary, and
  still deep inside tens of terabytes of empty space. Placing at the
  *top* of the hole was tried first and failed immediately — the top of
  that hole is the underside of the executable's load address, where
  ASLR moves things most and where the heap grows.

* **Scattered by namespace.** Deterministic placement is what lets a
  server and its clients agree on an address without exchanging one, but
  it also makes two things that have no reason to agree land on top of
  each other — concurrent jobs on a node being exactly that. The address
  is therefore displaced within a window by a scatter value derived from
  hashing the namespace (FNV-1a): identical for every process of one job,
  different between jobs. The independent-placement path mixes in the
  segment id and the retry count as well, so a job's three segments are
  not all aimed at one address and a retry does not keep naming the
  address it just lost.

The scan itself is not the expensive part, and this is recorded so the
suspicion is not re-derived: measured cost is about **0.17 µs per VMA**,
essentially linear (16 VMAs → 5 µs; 2016 → 345 µs). A PRRTE daemon
carries about 72 VMAs, so the scan costs it roughly 15 µs, under 1% of
the per-segment cost. Caching a hole would trade that for a stale-address
failure mode on a path whose whole correctness rests on client and server
agreeing about an address.

The address-space arena
^^^^^^^^^^^^^^^^^^^^^^^

A client attaches the **job** segment during ``PMIx_Init``, when its
address space is nearly empty. It attaches a **modex** segment at a
*fence*, by which time it has loaded an MPI stack, opened components,
grown arenas and registered fabric memory. The server picks both
addresses from its own map. The first attach almost always works; the
second is a gamble, and openpmix#4156 was that gamble lost — about one
run in 25 on a four-node job, taking every rank on a node at once.

The fix is to stop asking. At ``register_job_info`` time the server
calls ``arena_reserve()``, which claims **one contiguous range** — the
job segment's footprint plus one slot per modex generation that can be
live at once — using ``pmix_vmem_reserve()``, an inaccessible
``PROT_NONE`` mapping that commits no memory and costs one VMA. The
range travels to clients on every seg blob, and each client claims the
same range inside ``PMIx_Init``, the one moment it reliably can. Every
later mapping is then ``MAP_FIXED`` over ground the process already
holds (``PMIX_SHMEM_MAP_OVER_RESERVATION``), which cannot lose a race.

Four things about it are load-bearing:

* **Carve by the footprint, not the requested size.** A segment maps a
  page of header ahead of its data. Carving to the requested size
  overlapped each segment with the next by one page, and the next
  ``MAP_FIXED`` silently replaced it.
* **Detach restores the reservation.** ``pmix_shmem_segment_detach()``
  re-maps ``PROT_NONE`` over an in-arena range rather than unmapping it,
  or the first released segment would punch a hole in the arena for
  something else to take.
* **A modex generation holds its slot for as long as it is readable**,
  which is not just until the next one arrives. ``arena_alloc_modex()``
  therefore reads occupancy off the live segments — the build slot plus
  every published generation — and takes the lowest free slot. Deriving
  it rather than keeping a tally means no path that releases a
  generation can leave the accounting stale.
* **The session segment is deliberately outside the arena.** A session's
  segment is held by jobs other than the one that built it, while
  ``job_destruct()`` unmaps the arena wholesale, so a session placed
  inside it would go away under a live holder.

If the arena cannot be reserved, or a generation does not fit its slot,
every segment places itself independently exactly as before — degraded,
never broken.

When an attach fails
^^^^^^^^^^^^^^^^^^^^

``shmem3_attach()`` turns "could not map at the required address" and
"layout stamp does not match" alike into **``PMIX_ERR_TAKE_NEXT_OPTION``**.
That propagates up unchanged and triggers the framework's GDS fallback
(``fallback_to_next_gds()`` in ``src/client/pmix_client.c``): the client
switches to ``hash`` and re-requests its job data with
``PMIX_GDS_FALLBACK_CMD``. A failed attach is therefore not fatal to
``PMIx_Init``.

**That fallback exists only at init.** It works because ``PMIx_Init``
re-requests job data and the server can re-register it in another
module's format. Nothing equivalent is possible for a modex: a completed
modex cannot be re-delivered in ``hash`` format without a new wire
command. So a failed *modex* attach keeps the tracker (the job and
session segments are perfectly good), clears only that segment's status,
and the client simply misses on modex reads — which then go up to the
server like any other miss.

Clients are read-only, and the MMU enforces it
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

As the last step of attaching, a client calls
``pmix_shmem_segment_protect_data()`` and drops write access to the data
region. A client-side write is then a SIGSEGV on the instruction that
did it, rather than corruption some *other* process trips over later.

The internal header stays writable on purpose: it holds the reference
count that attach and detach maintain, and a reader that could not write
it could not let go of the segment. That is why the protection starts at
the data region, a page-aligned header into the mapping — and why the
geometry lives in ``pmix_shmem.c`` rather than at the call site.

Getting here required removing two writes a reader was making: a
hash-table lookup that recorded the table's key type, and the job
segment's indices being minted against the process-global key index that
the client then rewrote to match the server's. Both are gone. Do not add
a client-side write path; it will not silently work.


How Data Is Written
-------------------

Job and session data
^^^^^^^^^^^^^^^^^^^^

The first client of a namespace to connect drives
``register_job_info``, and the server does the following, all on the
progress thread:

#. ``fetch_local_job_data()`` asks for a complete copy of the job-level
   information the host registered.
#. ``get_local_job_data_info()`` packs it to measure it, producing both
   the element count and the key/value count discussed above.
#. ``prepare_shmem3_stores_for_local_job_data()`` reserves the arena and
   creates the job (and, if the job named a session, session) segments
   at the computed sizes.
#. ``pmix_gds_shmem3_store_local_job_data_in_shmem3()`` walks the fetched
   list and writes it through the segment's TMA. It recognizes
   ``PMIX_APP_INFO_ARRAY``, ``PMIX_NODE_INFO_ARRAY``,
   ``PMIX_PROC_INFO_ARRAY`` and ``PMIX_SESSION_INFO_ARRAY`` and routes
   each to its own store; anything else is a plain job-level key stored
   into ``local_hashtab`` under ``PMIX_RANK_WILDCARD``, against the
   segment's own key index.
#. The segments are marked ``READY_FOR_USE`` and then **published**
   (below). The job's goes first: a client told about the session before
   the job would find a job tracker with nothing in it.
#. ``cache_connection_info_for_job_shmem3()`` packs ``job->conni`` — the
   namespace name, a seg blob per published job segment, the tombstone
   list, and a seg blob per published session segment. That buffer is
   cached and copied into the reply for every later client, so the work
   above happens once per namespace.

On the client, ``store_job_info`` is simply
``client_connect_to_shmem3_from_buffi()``: unpack each seg blob, attach
if not already attached, publish locally. That runs on the client's
progress thread during ``PMIx_Init``.

Modex data
^^^^^^^^^^

``server_store_modex`` delegates to the base envelope walker
(``pmix_gds_base_store_modex``) with ``server_store_modex_cb()`` as the
per-proc callback. Each modex gets a **new segment**, one per
generation:

* An empty build slot means this blob starts a generation. If a
  generation is already published, the counter advances first — it names
  the backing file (``modexdata.<generation>``), so successive
  generations do not collide, and it dates tombstones.
* ``get_modex_sizing_data()`` estimates the segment from the blob, the
  segment is created and attached, and ``modex_smdata_construct()``
  builds the hash table and key index inside it.
* Each unpacked ``pmix_kval_t`` is stored into that table against *that
  segment's* key index.
* A ``NULL`` blob means the walker is done: the generation is marked
  ``READY_FOR_USE`` and published.

A finished generation is never written again. Writing a second modex
into the first one's segment is not a latent problem but a prompt one:
the segment was sized from the payload that created it and the allocator
behind it cannot grow, so a larger second modex overran it and aborted
the server — taking the daemon, and the job, with it.
``examples/modex_twice.c`` reproduces that from four nodes up.

The handoff is safe because the backing file is reference counted:
dropping the server's handle leaves a client that still has it mapped
with a valid mapping, and the file survives until the last holder lets
go.

.. note::

   ``server_store_modex_cb()`` must return ``PMIX_SUCCESS`` for a proc
   blob it consumed. Its natural exit is the unpack end-of-buffer code,
   and the base envelope walker reads any non-success return as failure
   of the whole server contribution — it stops, and then converts the
   same code to success for its own caller. Returning it would store the
   first proc of each contribution, discard the rest, and report success.


How Data Is Updated
-------------------

Since a published segment can never be rewritten, an update is a new
segment carrying only what changed, published at the head of a chain. A
read walks the chain newest-first and stops at the first segment that
answers.

The chain
^^^^^^^^^

.. code-block:: c

   typedef struct pmix_gds_shmem3_seg_t {
       struct pmix_gds_shmem3_seg_t *prior;   /* written once, never again */
       pmix_gds_shmem3_job_shmem3_id_t smid;
       pmix_gds_shmem3_status_t status;
       pmix_shmem_t *shmem3;
       void *smdata;                          /* the shared_*_data_t root */
       uint32_t generation;
       bool is_delta;
   } pmix_gds_shmem3_seg_t;

   typedef _Atomic(pmix_gds_shmem3_seg_t *) pmix_gds_shmem3_chain_t;

It is deliberately **not** a ``pmix_list_t``, and that difference is the
whole point:

* a node is filled in completely *before* it is published, is never
  written again, and is never removed;
* publishing is therefore one release-store of the head
  (``pmix_gds_shmem3_chain_publish()``) and entering it is one
  acquire-load (``pmix_gds_shmem3_chain_head()``);
* so a reader that has the head walks a chain that cannot change beneath
  it, on any thread, with no lock.

A ``pmix_list_t`` cannot do that: it is doubly linked through a sentinel
and carries a length counter, so a prepend is several stores and a
walker can catch any of them. None of the list operations are wanted
anyway — the chain is only ever prepended to and walked one way — which
is why a node carries a bare ``->prior`` rather than deriving from
``pmix_list_item_t``. A node is a plain allocation rather than a
``pmix_object_t``, because a refcount would suggest it can be released
from more than one place, which is precisely what a lock-free chain must
not permit.

**All three kinds of data are chains**, and they behave identically:

.. list-table::
   :header-rows: 1
   :widths: 16 42 42

   * - Chain
     - Published by
     - Grows when
   * - ``job_chain``
     - ``register_job_info()``, ``server_add_job_data()``
     - a host adds to an already-registered job's data
   * - ``session->segments``
     - the first job in the session
     - the session's description changes
   * - ``modex_chain``
     - each collecting fence
     - every fence

In each case the build slot on the tracker (``job->shmem3``/``smdata``,
``job->modex_shmem3``/``smmodex``, ``sesh->shmem3``/``smdata``) holds the
segment being written and **no reader touches it**; publishing onto the
chain is what makes it readable, and it empties the slot. So there is no
separate "current" for a reader to consult first, and no window in which
a reader could catch a segment half-built or half-retired.

That is also why the build fields are not the place to ask whether there
is data, or which segment to describe to a client: both questions are
answered from the chain head.

**Nothing removes from a chain.** The only thing that ever takes nodes
away is ``pmix_gds_shmem3_chain_destruct()`` in the job tracker's
destructor, which runs when the last reference to the tracker has gone
and therefore has no reader to race. Keeping a superseded generation is
redundant rather than wrong — the walk stops at the newest segment
holding the key. Redundant costs address space; unmapping under a reader
costs correctness.

What clients are told
^^^^^^^^^^^^^^^^^^^^^

* The **job** and **session** chains are packed **oldest first**
  (``pack_seg_chain()``, recursively). Order is load-bearing: the client
  publishes each segment as it attaches it, so describing them oldest
  first leaves its chain in the same order as the server's, newest at
  the head.
* The **modex** ships only its newest generation. A client already
  holding older generations keeps them; one attaching now is told about
  the rest at its next fence.
* A running client learns of a new job segment through
  ``pmix_server_notify_gds_update()`` and of a session update through
  ``server_pack_update``/``client_accept_update`` — all of which funnel
  into the same unpacker, whose duplicate check is what makes an update
  idempotent.

Delta modex generations
^^^^^^^^^^^^^^^^^^^^^^^

The flag byte in the modex envelope says whether a contribution is
**cumulative** (it repeats everything its processes have published) or
**delta** (it repeats nothing). A delta generation is not self-contained:
the generations before it are still the only copy of everything it did
not repeat, so they must stay mapped and answerable. That is why the
modex is a chain and not a single current segment, and why the arena
allocates slots rather than alternating between two.

``modex_fetch()`` accordingly walks newest-first. A keyed lookup stops
at the newest generation holding the key. A ``NULL``-key lookup —
"everything this process published" — consults every generation and
drops a copy of a key a newer one already supplied, or the caller sees
that key twice with the stale value second.

The chain is empty of prior generations unless a delta has been stored,
so the ordinary case is the single lookup it has always been.
``examples/modex_twice.c`` is the canary, and
``contrib/dockerswarm/run-gds-tests.sh`` drives it twice: once
cumulatively, once with ``pmix_server_fence_delta_modex=1``.

Deletion: tombstones
^^^^^^^^^^^^^^^^^^^^

This component cannot take a key out — the data is in a segment local
clients have mapped. ``del_key`` records that the key is gone and every
read consults the record.

The record is **process-local**, on ``job->tombstones``, and it is a
chain with the same discipline as the segment chain: a node is complete
before it is published, never rewritten, never removed until the tracker
dies. So recording a deletion and reading one both run without a lock. A
key deleted again at a later generation publishes a *second* node rather
than updating the first; ``tombstoned()`` is satisfied by any matching
entry, so the newer node says everything an update would have. A repeat
that would not change the answer is skipped, which keeps the chain
bounded.

Putting tombstones in shared memory would mean a new segment, mapped by
every local client, for a few bytes per deleted key — and it would not
save the step that matters, because a client attaching afterwards has to
be told either way. Each process builds its own from the notification
its server sends, and ``pack_tombstones()`` adds the list to the cached
job-info reply for later arrivals. ``del_key()`` drops ``job->conni``
for exactly that reason: the cached reply still says the key exists.

**A tombstone carries the modex generation it was recorded at.** Job
data is written once and never re-published, so ``job_fetch()`` asks with
``UINT32_MAX`` and a tombstone against it always applies. Modex data can
legitimately come back, so ``modex_fetch()`` asks with each generation's
own number as it walks, and a tombstone shadows only generations up to
the one it was made at.

Both ends advance ``job->modex_generation``, and only the server's names
anything. It was introduced as a server-side counter naming the next
backing file, but it is also what dates a tombstone, and ``del_key()``
runs on the client too. A client that left it at zero stamped every
tombstone with generation zero, so ``generation <= t->generation`` held
for every generation it ever mapped and the removal never expired there.
The two counters are never compared with each other — each only has to
advance whenever *its* process takes on a new generation.


How Locks Are Avoided
---------------------

``pmix_shmem3_module.is_tsafe`` is ``true``, and that is not decorative.
``try_local_fetch()`` tests it on every keyed ``PMIx_Get``, and
``pmix_client_globals.fast_get`` defaults **on** — so on a client,
``pmix_gds_shmem3_fetch()`` normally runs on whatever thread called
``PMIx_Get``, while the progress thread carries on underneath it.

There is no lock on that path, and that is the design. A lock here is
paid by every ``PMIx_Get``, and a library pulling thousands of values
during its init pays it thousands of times — which is precisely what
this component exists to avoid.

What makes it safe is that everything a read walks is one of three
things:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - What a read walks
     - Why it needs nothing
   * - the contents of a segment
     - written once, before any client can see it, and never again
   * - the segment itself
     - never unmapped while a reader holds the tracker — a mapping
       cannot be withdrawn mid-read
   * - the chains (job, session, modex, tombstones)
     - a node is complete before it is published, never rewritten,
       never removed

Entering a chain is a single acquire-load paired with the release-store
that published its head; the ``->prior`` pointers are immutable, so the
rest of the walk is ordinary pointer chasing.

The synchronization that *does* exist is exactly three things:

#. **The component's ``joblock``.** It guards the *spine* of the job
   list — finding a tracker and taking a reference to it — because a
   reader that is not on the progress thread could otherwise have the
   list mutated under it by ``del_nspace()``. It does **not** cover
   reading a tracker's contents.
#. **The reference a reader holds on the job tracker.**
   ``pmix_gds_shmem3_acquire_job_tracker()`` takes it under the lock and
   the reader releases it when finished. A tracker owns the mappings of
   its segments, so releasing the last reference detaches them; holding
   one means the progress thread can deregister the nspace underneath
   the reader and the memory being walked still stays put. The refcount
   is a C11 atomic, so this works across threads.
#. **The atomics themselves**: the chain heads, the tombstone head, and
   ``job->modex_generation`` (an ``pmix_atomic_uint32_t``, because a
   reader on another thread compares a tombstone against it).

A ``job->datalock`` used to stand here, and it was needed because none
of the three rows above were true: a retired generation could be
unmapped under a reader, the current generation was moved between two
fields in several stores, and the tombstone list was an ordinary
``pmix_list_t`` being appended to. Each was fixed in turn rather than
locked around.

.. important::

   If you add state that a read consults, make it **immutable** or make
   it a **chain**. Do not reach for a mutex: a lock on this path is paid
   by every ``PMIx_Get``.


``PMIx_Get``, Step by Step
--------------------------

This section follows a single ``PMIx_Get`` on a client. The code is
``src/client/pmix_client_get.c`` and, for the datastore half,
``src/mca/gds/shmem3/gds_shmem3_fetch.c``.

Step 0: entry checks
^^^^^^^^^^^^^^^^^^^^

``PMIx_Get`` rejects, in order: an uninitialized library
(``PMIX_ERR_INIT``), a ``NULL`` ``val`` (there is no way to hand back an
answer), a stopped progress thread (``PMIX_ERR_NOT_AVAILABLE``), and a
key longer than ``PMIX_MAX_KEYLEN``. Nothing shared has been touched
yet; all of this is on the caller's thread.

Step 1: pre-computation — the ``lg`` struct
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``lg = PMIX_NEW(pmix_get_logic_t)`` allocates the object that carries
everything the request means, as opposed to what the caller literally
wrote. ``process_request()`` fills it in. This is the whole of the
library's interpretation of the request, and it happens **before** any
thread shift, because it touches nothing shared.

.. code-block:: c

   typedef struct {
       pmix_object_t super;
       pmix_proc_t p;            /* the resolved target */
       bool pntrval;             /* PMIX_GET_POINTER_VALUES */
       bool stval;               /* PMIX_GET_STATIC_VALUES  */
       bool optional;            /* PMIX_OPTIONAL           */
       bool immediate;           /* PMIX_IMMEDIATE (server acts on it) */
       bool refresh_cache;       /* PMIX_GET_REFRESH_CACHE  */
       pmix_scope_t scope;       /* PMIX_DATA_SCOPE         */
       bool sessioninfo, sessiondirective;  uint32_t sessionid;
       bool nodeinfo,    nodedirective;     char *hostname; uint32_t nodeid;
       bool appinfo,     appdirective;      uint32_t appnum;
   } pmix_get_logic_t;

``process_request()`` does six distinct things:

**a. Reject the two unusable combinations.** Both ``proc`` and ``key``
``NULL`` is ``PMIX_ERR_BAD_PARAM``; so is a ``NULL`` key with
``PMIX_RANK_WILDCARD``, since "all data from every rank" has no answer.

**b. Infer the realm from the key.** ``pmix_check_node_info(key)``,
``pmix_check_app_info(key)`` and ``pmix_check_session_info(key)`` set
``lg->nodeinfo`` / ``appinfo`` / ``sessioninfo``. A key like
``PMIX_NUM_SLOTS`` is a node-realm question whether or not the caller
said so.

**c. Walk the caller's info array.** Each recognized qualifier lands in
``lg``. Typed qualifiers are validated here rather than trusted:
``PMIX_DATA_SCOPE`` must really carry a ``PMIX_SCOPE``, ``PMIX_HOSTNAME``
a non-``NULL`` string (and it is ``strdup``\ 'd, because ``lg`` outlives
the call and the caller's array is not ours), and ``PMIX_NODEID`` /
``PMIX_APPNUM`` / ``PMIX_SESSION_ID`` go through
``PMIx_Value_get_number()``. The explicit realm directives
(``PMIX_JOB_INFO``, ``PMIX_NODE_INFO``, ``PMIX_APP_INFO``,
``PMIX_SESSION_INFO``) *override* the inference in (b) and set the
matching ``*directive`` flag, which later distinguishes "the caller
asked for this realm" from "we deduced it".

**d. Answer the requests that need no datastore at all.** Three keys are
served straight from ``pmix_globals``: ``PMIX_PROCID`` (with a ``NULL``
proc), ``PMIX_VERSION_NUMERIC``, and ``PMIX_RANK`` when the caller passed
our own nspace and ``PMIX_RANK_INVALID``. Each returns
``PMIX_OPERATION_SUCCEEDED`` with ``*val`` already prepared, honoring
``lg->stval`` (copy into the caller's storage) and ``lg->pntrval`` (hand
back ``&pmix_globals.myidval`` / ``myrankval``). These never post an
event and touch nothing shared, which is why the progress-thread check
in step 2 sits *below* this point — they remain usable from inside an
event handler.

**e. Resolve the target into ``lg->p``.** A ``NULL`` or empty nspace
means our own. A ``NULL`` ``proc`` means the key is expected to be
globally unique, which resolves to ``PMIX_RANK_UNDEF`` — except when a
node or app realm was selected, where "our own" is the sensible referent
and our rank is used instead.

**f. Translate a group.** If the caller named a *group* in
``proc->nspace``, ``pmix_client_convert_group_procs()`` maps it to the
real proc, and exactly one proc must come back.

Step 2: refuse to deadlock
^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_progress_thread_check_blocking("PMIx_Get")`` returns true if the
caller is *already standing in* the progress thread. Everything below
either blocks on that thread or hands work to it and waits, so such a
caller would be waiting for itself; ``PMIX_ERR_WOULD_BLOCK`` is returned
instead of hanging.

Step 3: optional cache refresh — a blocking round trip
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If ``lg->refresh_cache`` is set, ``refresh_cache(&lg->p)`` packs a
``PMIX_REFRESH_CACHE`` command, sends it, and ``PMIX_WAIT_THREAD``\ s on
the reply — on the caller's thread. The reply is unpacked by ``refcb()``
**on the progress thread** (a PTL receive callback) and stored into
``pmix_globals.mypeer``'s module. Note that ``PMIx_Get_nb`` blocks here
too, which is why it carries the same progress-thread guard scoped to
this branch.

Step 4: the caddy
^^^^^^^^^^^^^^^^^

``cb = PMIX_NEW(pmix_cb_t)``, carrying ``lg``, the caller's ``key`` and
``info``/``ninfo`` **by pointer** (the caller guarantees they stay valid),
and — for the blocking form — ``cb->cbfunc.valuefn = _value_cbfunc`` with
``cb->cbdata = cb``, so completion sets ``cb->status``/``cb->value`` and
wakes ``cb->lock``. The non-blocking form stores the caller's callback
instead and sets ``cb->checked = true`` to route completion through
``gcbfn()``.

Step 5: the local short circuit — no thread shift at all
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``try_local_fetch(cb, lg)`` answers the request on the caller's own
thread when the datastore holding the answer can be read from one. This
is where ``shmem3`` earns its place: the thread shift is the dominant
cost of a get that hits locally — measured at roughly **4.9 µs** against
about **130 ns** of actual lookup underneath it.

The gating is deliberately narrow. It declines if:

* ``pmix_client_globals.fast_get`` is off (MCA ``pmix_client_fast_get``);
* the key is ``NULL`` — "everything this proc put" is an aggregate
  across scopes, not a lookup;
* any realm redirection is in play (``nodeinfo``/``appinfo``/
  ``sessioninfo``, any ``*directive``, a ``hostname``, a ``nodeid``) —
  resolving those takes further fetches and an info-array rebuild that
  belong on the progress thread;
* ``refresh_cache`` was asked for;
* this is a singleton, or we are not connected;
* ``PMIX_GDS_FETCH_IS_TSAFE(pmix_client_globals.myserver)`` says no.

That last check is re-resolved **every time** rather than cached:
``fallback_to_next_gds()`` can re-point this peer at a different module
at run time, and a stale answer would be a read of a store this process
has abandoned.

On a pass it sets ``cb->proc = &lg->p``, ``cb->scope = lg->scope``,
calls ``PMIX_GDS_FETCH_KV`` — which lands in
``pmix_gds_shmem3_fetch()``, described in step 8, running on the
caller's thread — and shapes the result with ``process_values()``. A
miss, or a ``process_values()`` that declines, drains ``cb->kvs`` and
falls through to the ordinary path. This is a short circuit, never a
partial one: the drain matters because ``process_values()`` distinguishes
"the value" from "an aggregate of everything this proc put" by *counting*
that list, so one entry left behind by a failed fetch would turn a scalar
get into a data array.

.. admonition:: Thread shift 1
   :class: note

   ``PMIX_THREADSHIFT(cb, get_data)`` — taken only when the short
   circuit above declines. The blocking form then does
   ``PMIX_WAIT_THREAD(&cb->lock)``; the non-blocking form returns
   immediately.

Step 6: ``get_data()`` on the progress thread
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Everything from here runs on the progress thread.

**Realm resolution.** If ``lg->nodeinfo``, ``lg->appinfo`` or
``lg->sessioninfo`` is set, the request first has to be given the
identity of the node, app or session it is about. Each branch:

* uses what we already know if the target is us
  (``pmix_globals.hostname``, ``nodeid``, ``appnum``, ``sessionid``);
* otherwise issues a small ``PMIX_OPTIONAL`` sub-fetch on a stack caddy
  for ``PMIX_HOSTNAME`` / ``PMIX_NODEID`` / ``PMIX_APPNUM`` /
  ``PMIX_SESSION_ID`` — against ``pmix_client_globals.myserver``'s
  module on a client, ``pmix_globals.mypeer``'s on a server, because a
  client's job-level data is stored against the server's module;
* short-circuits when the identifier *is* what was asked for (a get of
  ``PMIX_HOSTNAME`` in the node realm is answered right there);
* otherwise copies the caller's directives into a fresh array with room
  for two or three more (``copy_directives()``), appends the realm
  directive if it was inferred rather than given, the identifier, and
  ``PMIX_OPTIONAL``, and sets ``cb->infocopy``. That array is what
  ``_pack_get()`` will put on the wire.

The transfer status matters here and is checked: ``PMIx_Info_xfer()``
sets the destination's type before it can fail, so a directive that
would not copy leaves a slot naming a type with nothing behind it — not
an empty slot the server can skip.

**The local lookups (label ``doget``).** In order:

#. ``PMIX_GDS_FETCH_KV(pmix_client_globals.myserver, cb)`` — the data the
   server provided. For ``shmem3`` this is the shared segments.
#. On a miss, ``DRAIN_KVS(cb)``, then — unless both ends are on ``hash``,
   in which case the two peers point at the same tables —
   ``PMIX_GDS_FETCH_KV(pmix_globals.mypeer, cb)``, our own hash tables
   (where our own puts and any cached direct-modex answers live).

Either hit goes to ``process_values()`` and completes.

.. note::

   What is deliberately **not** here is a retry at
   ``PMIX_RANK_WILDCARD``. ``PMIX_RANK_UNDEF`` means "anything in this
   nspace", and job-level data is part of that even though it is filed
   under no rank — answering it is the datastore's job. Both in-tree
   modules once searched only the per-rank tables, and a client-side
   retry hid that for years. The retry is gone;
   ``test/unit/get_api.c`` holds the module behavior directly.

**Going to the server.** On a miss the caddy records
``cb->pname`` (the nspace/rank the reply will be matched against) and:

* a ``NULL`` key does *not* go up, unless the server is pre-v3.2 or we
  are asking about another namespace's job-level info, in which case the
  request rank is rewritten to ``PMIX_RANK_WILDCARD``;
* a reserved key goes up like any other — the server's *host* frequently
  knows values it chose not to push down, and it is asked only if the
  request is surfaced to it;
* a server that is not a tool, a disconnected process,
  ``PMIX_ERR_EXISTS_OUTSIDE_SCOPE``, or ``lg->optional`` all end the
  request here;
* otherwise the pending-request list is scanned with ``same_target()``
  (exact rank *and* nspace). A matching outstanding request means this
  caddy is simply appended and no message is sent — the reply will
  satisfy both.

Finally ``_pack_get()`` builds a ``PMIX_GETNB_CMD`` message — nspace,
rank, the (possibly rebuilt) info array, and the key if there is one —
the caddy is appended to ``pmix_client_globals.pending_requests``, and
``PMIX_PTL_SEND_RECV`` sends it with ``_getnb_cbfunc`` as the reply
handler.

Step 7: the reply
^^^^^^^^^^^^^^^^^

``_getnb_cbfunc()`` is a PTL receive callback, so it is **already on the
progress thread — no shift is needed**. It unpacks the status, calls
``PMIX_GDS_ACCEPT_KVS_RESP`` to store the payload (for ``shmem3`` this
is where a client takes delivery of segment information rather than
data), then walks every pending request matching the same target and
answers each: fetch from ``mypeer``, retry at ``PMIX_RANK_WILDCARD`` if
the request was at ``PMIX_RANK_UNDEF``, fall back to ``myserver``'s
module, then ``process_values()``.

An empty buffer means the connection was lost, and every waiter is told
``PMIX_ERR_LOST_CONNECTION`` rather than the "not found" the status was
seeded with — those are different facts.

Completion for each waiter is either ``gcbfn()`` (non-blocking:
invoke the caller's callback, release ``lg`` and the caddy) or
``cb->cbfunc.valuefn`` — ``_value_cbfunc()`` for the blocking form,
which sets ``cb->status``/``cb->value`` and calls
``PMIX_WAKEUP_THREAD(&cb->lock)``, releasing the caller in step 5.

Step 8: inside ``pmix_gds_shmem3_fetch()``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This is the read of the shared segments, and it may be running on the
application's thread (step 5) or the progress thread (step 6).

**a. Acquire the tracker.** ``pmix_gds_shmem3_acquire_job_tracker()``
takes ``joblock``, finds the tracker for the nspace, ``PMIX_RETAIN``\ s
it, and drops the lock. No such tracker means
``PMIX_ERR_INVALID_NAMESPACE`` — for a reader, simply "nothing to read
here". The reference is released at the end, and it is the only
synchronization the rest of the read has.

**b. Is there anything to read?** ``shmem3_fetch_from_job()`` asks the
*chain* whether a job segment has been published. A tracker exists from
``PMIx_server_register_nspace`` time, long before ``register_job_info()``
builds a segment, so a fetch arriving in that window is
``PMIX_ERR_NOT_FOUND``, not a fault. Whether there is modex data is the
same question asked of ``modex_chain``.

**c. The whole-job case.** ``NULL`` key with ``PMIX_RANK_WILDCARD``
means "a complete copy of the job-level info": job data under
``PMIX_RANK_WILDCARD``, then session info, then node info and app info
across the job chain, then one ``PMIX_PROC_INFO_ARRAY`` per rank built
from that rank's entries.

**d. Realm dispatch.** The qualifiers are scanned for
``PMIX_SESSION_INFO`` / ``PMIX_NODE_INFO`` / ``PMIX_APP_INFO``; failing
that, the *key* is classified the same way ``process_request()`` did.
Session requests go to ``fetch_sessioninfo()`` (which walks the
session's own chain newest-first). Node and app requests with a
non-valid rank go to ``chain_fetch_nodeinfo()`` /
``chain_fetch_appinfo()``, which consult every job segment, since an
addition published later carries its own lists.

**e. Choose the store.** ``PMIX_INTERNAL``, ``PMIX_LOCAL``,
``PMIX_GLOBAL``, ``PMIX_SCOPE_UNDEF`` and any wildcard-rank request read
the **job** chain; ``PMIX_REMOTE`` reads the **modex** chain. Anything
else is ``PMIX_ERR_BAD_PARAM``.

**f. The lookup (label ``doover``).** For ``PMIX_RANK_UNDEF`` the fetch
sweeps ranks ``0..nprocs-1``; a keyed request returns at the first hit.
On the job pass only, it then also asks ``PMIX_RANK_WILDCARD``, because
job-level data is filed under no rank and ``PMIX_RANK_UNDEF`` means
"anything in this nspace". That extra lookup is confined to the job
pass on purpose — the ``doover`` retry returns here for the modex, where
job data never is, and repeating it would collect the same value twice,
which the client's ``process_values()`` would shape into a
``PMIX_DATA_ARRAY`` of duplicates.

For a specific rank it is a single ``job_fetch()`` or ``modex_fetch()``.

**g. The scope retry.** On success with ``PMIX_GLOBAL``, and on failure
with ``PMIX_GLOBAL`` or ``PMIX_SCOPE_UNDEF``, ``useremote`` is set and
control jumps back to ``doover`` to try the other store. Everything past
that label dispatches on this flag rather than on a table pointer,
because the modex is a chain of generations rather than one table —
arriving there with the flag unset ran the job fetch against the modex
data paired with the wrong key index, which missed on the *ordinary*
path (an unqualified get arrives as ``PMIX_SCOPE_UNDEF`` and reaches the
modex only through here).

**h. The chain walks themselves.**

* ``job_fetch()`` walks ``job->job_chain`` newest-first; per segment it
  calls ``pmix_hash_fetch()`` with **that segment's** key index (an index
  is minted per segment, so table and index cannot be separated), then
  ``drop_tombstoned(..., UINT32_MAX, mark)``. A keyed request stops at
  the newest segment that has the key; a ``NULL``-key request continues
  and ``drop_shadowed()`` removes anything a newer segment already
  supplied.
* ``modex_fetch()`` does the same over ``job->modex_chain``, passing
  each segment's own ``generation`` to ``drop_tombstoned()``, and calls
  ``strip_undef()`` to remove entries whose value is ``PMIX_UNDEF`` —
  those are not data but the contributing process saying the key is
  gone, which ``hash`` acts on by removing the key and this component
  cannot.

**i. Not found, but elsewhere.** If nothing was collected and the rank
was valid, a ``PMIX_LOCAL`` request checks the modex and a
``PMIX_REMOTE`` request checks the job data; if the key exists there,
the result is discarded and ``PMIX_ERR_EXISTS_OUTSIDE_SCOPE`` is
returned, so the caller is told rather than left to time out.

**j. Final status.** A non-empty list is success, whichever pass filled
it. (``modex_fetch()`` can report ``PMIX_ERR_NOT_FOUND`` after a
successful ``NULL``-key read of the current generation; this final check
is what overrides it. It is fragile rather than wrong — if that check is
ever made narrower, fix ``modex_fetch()`` first.)

Step 9: shaping the answer
^^^^^^^^^^^^^^^^^^^^^^^^^^

``process_values()`` turns ``cb->kvs`` into a single ``pmix_value_t``:

* exactly one entry **and** a non-``NULL`` key → that value, with
  ownership taken from the kval. A ``PMIX_QUALIFIED_VALUE`` is unwrapped
  to the value at element 0 of its embedded info array — after the shape
  is verified, because the datastore is also filled from the server and
  nothing screens it on the way out;
* an empty list → ``PMIX_ERR_NOT_FOUND``;
* anything else → a ``PMIX_DATA_ARRAY`` of ``pmix_info_t``, which is
  what a ``NULL``-key get returns.

Back in ``PMIx_Get``, ``lg->stval`` copies into the caller's storage
(and reports a failed transfer rather than handing back a type with
nothing behind it), otherwise ``*val`` takes the value. A success with
nothing behind it is converted to ``PMIX_ERR_NOT_FOUND``: ``PMIx_Get``
entitles the caller to dereference ``*val`` when it says success.

Summary of thread shifts
^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 34 30 36

   * - Point
     - Mechanism
     - When it happens
   * - ``PMIx_Get`` → ``get_data()``
     - ``PMIX_THREADSHIFT``, then ``PMIX_WAIT_THREAD``
     - only when ``try_local_fetch()`` declines
   * - ``PMIx_Get_nb`` → ``get_data()``
     - ``PMIX_THREADSHIFT``, returns immediately
     - same
   * - ``PMIx_Get_nb`` answered by ``process_request()``
     - ``PMIX_THREADSHIFT(cb, gcbfn)``
     - ``PMIX_PROCID`` / ``PMIX_VERSION_NUMERIC`` / ``PMIX_RANK``, so the
       callback still fires from the progress thread
   * - ``refresh_cache()``
     - no shift; blocking ``PMIX_WAIT_THREAD`` on a PTL round trip
     - ``PMIX_GET_REFRESH_CACHE``
   * - ``_getnb_cbfunc()``, ``refcb()``
     - none needed — PTL receive callbacks already run there
     - every server reply
   * - ``try_local_fetch()`` → ``pmix_gds_shmem3_fetch()``
     - **none** — runs on the caller's thread
     - keyed get, no realm redirection, ``is_tsafe`` module

Note what ``PMIx_Get`` never does: it never takes a lock on the datastore
it reads. The only lock in the whole path is the brief ``joblock`` around
finding a tracker, and the only wait is the one a *blocking* caller does
on its own completion.


MCA Parameters
--------------

.. list-table::
   :header-rows: 1
   :widths: 34 12 54

   * - Parameter
     - Default
     - Meaning
   * - ``gds_shmem3_segment_size_multiplier``
     - ``1.0``
     - Scales the computed segment sizes. Raise it if a job overflows a
       segment (which aborts); prefer this over shaving the estimates.
   * - ``gds_shmem3_arena_slot_size``
     - 1 GiB
     - Address space reserved per modex slot. ``0`` disables the arena
       and restores independent placement. Virtual address space only —
       nothing is committed.
   * - ``gds_shmem3_arena_modex_slots``
     - ``4``
     - How many modex generations the arena can hold at once, capped at
       32. More than one is live only where deltas are in play; past the
       last slot a generation is placed outside the arena.
   * - ``gds_shmem3_offset_placement``
     - ``true``
     - Place segments a quarter of the way into the biggest hole rather
       than at its midpoint.
   * - ``gds_shmem3_force_client_attach_failure``
     - ``false``
     - **Testing only** — force *every* client attach to fail, so the
       init-time GDS fallback can be exercised.
   * - ``gds_shmem3_force_modex_attach_failure``
     - ``false``
     - **Testing only** — force only the *modex* attach to fail. This is
       the case the parameter above cannot reach, since that one leaves
       the client on ``hash`` before it ever reaches a fence.
   * - ``pmix_client_fast_get``
     - ``true``
     - Answer a keyed ``PMIx_Get`` on the caller's thread when the
       module is ``is_tsafe``. Turning it off routes every get through
       the progress thread.

``pmix_hash_proc_alloc`` (``src/runtime/pmix_params.c``) is also
relevant: it sets how many key slots each rank's pointer array starts
with, and for ``shmem3`` that inflates the segment estimate for every
rank in the job.


Testing
-------

``shmem3`` gets **no coverage at all on macOS** — ``configure.m4`` gates
it on a 64-bit, non-Apple host with no ``--enable-test-build`` escape,
so it is not even compiled there.

* ``contrib/dockerswarm/run-gds-tests.sh`` is the suite that builds it
  on Linux and then exercises it: server and clients on ``shmem3``, a
  client forced onto the fallback path, cross-node fetches that reach
  the modex, and ``examples/modex_twice.c`` run both cumulatively and
  with ``pmix_server_fence_delta_modex=1``.
* ``test/unit/gds_datastore`` covers the framework contracts this
  component has to honor, against whichever module is assigned. Its last
  case, ``test_shmem3_job_segment()``, asks for ``shmem3`` by name and
  drives it end to end in one process — register an nspace, build the
  job and session segments, read a job-level key and the whole job back
  out, deregister — printing ``SKIP`` where the component is not
  available.
* ``test/unit/get_api.c`` holds the ``PMIX_RANK_UNDEF`` behavior
  described in step 8f directly, from a real client.
* ``test/unit/util/util_vmem.c`` pins the placement and scatter
  properties against the placement function, where they are decidable —
  ASLR hides them in any live test.
