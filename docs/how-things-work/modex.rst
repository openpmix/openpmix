Modex: Exchanging Process Data
==============================

This document describes how data published by one process with
``PMIx_Put`` reaches every other process in a job — the operation
universally called the **modex** (short for "module exchange", a name
inherited from the MPI runtimes that first needed it). It follows a
single value from the ``PMIx_Put`` that creates it, through
``PMIx_Commit``, the server's collection of local contributions, the
handoff to the host environment, and the storage of the aggregated
result. It then explains the **key index** — the integer that PMIx
substitutes for a key string inside the datastore — because that index
is the reason modex data is stored where it is, and the constraint that
governs any attempt to move it.

For a code-oriented orientation aimed at contributors working *inside*
the datastore, ``src/mca/gds/AGENTS.md`` covers the framework and each
component directory carries its own.


The Problem
-----------

A process that wants to communicate with its peers must publish
something about itself — a network endpoint, a shared-memory key, a
device identifier — and must learn the same about everyone else. PMIx
offers two ways to do this:

* **Direct modex**, in which a process asks for one specific peer's data
  and the request is routed to whichever server owns that peer. This is
  lazy and cheap when only a few peers are of interest.

* **Full modex**, in which every process contributes its data to a
  collective (``PMIx_Fence`` with ``PMIX_COLLECT_DATA``) and every
  server ends up holding the data of every process. This costs one
  all-gather but makes every subsequent lookup local.

This document is about the second. The distinguishing property of a full
modex is that the result is *bulk* data — on a large job, the single
largest block of memory PMIx holds — which is why where and how it is
stored matters so much.


Scope and Roles
---------------

Three roles appear throughout:

* the **client**, an application process that calls ``PMIx_Put``,
  ``PMIx_Commit``, ``PMIx_Fence`` and ``PMIx_Get``;
* the **PMIx server**, embedded in the resource manager's local daemon,
  which holds data on behalf of its local clients;
* the **host environment**, the RM itself, which performs the actual
  cross-node all-gather. PMIx does not move the bytes between nodes; it
  hands the host a blob and receives an aggregated blob back.

Note that a PMIx server is also a client of itself: it has its own
``pmix_globals.mypeer`` peer object and its own datastore module.


Following a Value
-----------------

Publication: ``PMIx_Put``
^^^^^^^^^^^^^^^^^^^^^^^^^

``PMIx_Put`` (``src/client/pmix_client.c``) validates its arguments and
thread-shifts to ``_putfn``, which builds a ``pmix_kval_t`` holding a
copy of the key string and the value exactly as the caller supplied it,
and hands it to the datastore::

    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, cb->scope, kv);
    ...
    pmix_globals.commits_pending = true;

Two things about the destination are fixed and worth knowing:

#. A client pins its **own** peer to the ``hash`` module during
   ``PMIx_Init`` unless the application explicitly asked for another via
   ``PMIX_GDS_MODULE``.
#. The ``shmem3`` module leaves its ``store`` slot ``NULL`` on purpose,
   and ``PMIX_GDS_STORE_KV`` routes a ``NULL`` slot to the local
   module.

So a ``PMIx_Put`` **always** lands in ``gds/hash``, whatever module the
namespace otherwise uses. ``pmix_gds_hash_store`` files the value by
scope into one of three hash tables on the namespace's tracker:
``internal`` (for ``PMIX_INTERNAL``), ``local``, and ``remote``, with
``PMIX_GLOBAL`` stored into both ``local`` and ``remote``.

The value is *not* sent anywhere yet. ``PMIx_Put`` is purely local; only
the ``commits_pending`` flag records that something is outstanding.

Transmission: ``PMIx_Commit``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``_commitfn`` reads the process's own contributions back out of the
datastore — once for ``PMIX_LOCAL`` scope and once for ``PMIX_REMOTE``
— and packs each set into a sub-buffer preceded by its scope::

    PMIX_COMMAND (PMIX_COMMIT_CMD)
    PMIX_SCOPE (PMIX_LOCAL)   PMIX_BUFFER < kval, kval, ... >
    PMIX_SCOPE (PMIX_REMOTE)  PMIX_BUFFER < kval, kval, ... >

The message is sent even when the process contributed nothing, so the
server always learns that this client has finished contributing.

**The payload is cumulative.** ``cb->key`` is left ``NULL``, so each of
those two fetches returns everything the process has ever put at that
scope — not just what is new. ``pmix_globals.commits_pending`` gates
*whether* to fetch, never *what*, so a single new ``PMIx_Put`` causes the
whole accumulated set to be shipped again, and n put/commit cycles move
O(n²) bytes. The same is true one level up, in ``pmix_server_collect_data``
below. See :ref:`the delta design <modex-delta>` for the planned
replacement and for why the cumulative path has to survive it.

**The wire carries key strings, not indices.**
``pmix_bfrops_base_pack_kval`` packs ``kval->key`` as a ``PMIX_STRING``
followed by the value. This is the single most important fact about the
modex format, and the rest of this document depends on it.

Server ingest: ``pmix_server_commit``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_server_commit`` (``src/server/pmix_server_fence.c``) unpacks each
scope block and stores its kvals, splitting them by destination::

    if (PMIX_LOCAL == scope || PMIX_GLOBAL == scope) {
        PMIX_GDS_STORE_KV(rc, peer, &proc, scope, kp);
    }
    if (PMIX_REMOTE == scope || PMIX_GLOBAL == scope) {
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, scope, kp);
    }

Local-scope data goes to the contributing peer's module, so sibling
processes on the same node can read it. Remote-scope data goes to the
*server's own* module — that copy is what is later harvested for the
collective, and what answers a direct-modex request from another node.

The server then marks the peer as having contributed, and immediately
services any direct-modex requests that were waiting on this exact
process.

Collection: ``pmix_server_collect_data``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When a fence carrying ``PMIX_COLLECT_DATA`` has gathered all of its
local participants, ``pmix_server_collect_data`` builds this server's
contribution. For each local participant it fetches the
``PMIX_REMOTE``-scope kvals and packs a per-rank blob — the
``pmix_proc_t`` followed by that process's kvals — then wraps the
collection, compresses it, and wraps it again.

The result is nested three deep::

    buff
     └── PMIX_BYTE_OBJECT          one per contributing server
          ├── bool                 was the rank-level block compressed?
          └── PMIX_BYTE_OBJECT     the rank-level block
               ├── byte            collect flag (PMIX_COLLECT_YES / _NO)
               └── PMIX_BYTE_OBJECT   one per contributing process
                    └── pmix_proc_t, then that process's kvals

The collect flag is recorded per server so the receiving end can detect
participants that disagreed about whether data was being collected; a
mismatch raises the ``collection-mismatch`` help message rather than
silently producing a short result.

Handoff to the host
^^^^^^^^^^^^^^^^^^^

The assembled bucket is handed to the resource manager::

    rc = pmix_host_server.fence_nb(trk->pcs, trk->npcs, trk->info, trk->ninfo,
                                   data, sz, trk->modexcbfunc, trk);

PMIx takes no part in the cross-node exchange. The host performs the
all-gather and returns the concatenation of every server's contribution
— which is why the outermost layer of the envelope repeats once per
server.

Several paths short-circuit this and invoke the callback directly with a
``NULL`` blob: a fence whose participants are all local, a host with no
``fence_nb`` entry point, a host that returns
``PMIX_OPERATION_SUCCEEDED``, and the error paths.

Return storage
^^^^^^^^^^^^^^

``pmix_server_modex_cbfunc`` may run on the host's own thread, so it does
nothing but thread-shift onto the progress thread, landing in
``_mdxcbfunc`` (``src/server/pmix_server_op_replies.c``). That function stores the returned blob
and then replies to each waiting participant::

    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &xfer, scd->data, scd->ndata);
    PMIX_GDS_STORE_MODEX(rc, nspeer, nptr->ns->nspace, &xfer, tracker);

Both datastore components delegate the unwrapping to
``pmix_gds_base_store_modex`` (``src/mca/gds/base/gds_base_fns.c``), so
neither reimplements the envelope. The walker decompresses where
flagged, checks the collect flag for consistency across servers, and
invokes a component-supplied callback once per process blob — plus once
per namespace with a ``NULL`` buffer, to signal that the modex is
complete.

.. note::

   The callback must return ``PMIX_SUCCESS`` for a blob it consumed.
   Running off the end of a blob is how a callback knows it has finished,
   but the walker reads any non-success return as a failure of the whole
   server contribution and abandons the remaining processes in it — while
   still reporting success to its own caller. Convert the end-of-buffer
   status inside the callback.

For ``gds/hash``, the callback stores each kval into the namespace
tracker's ``remote`` table under the contributing process's rank. From
there ``PMIx_Get`` for a remote peer is answered locally.

Reading the result, and re-publishing a key
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The point of a full modex is that the answer is already local, so
``PMIx_Get`` for a peer's key is served from the requesting process's own
copy without touching the network. Two consequences are worth knowing,
because both surprise people.

**Asking for one non-reserved key fetches the peer's whole set.** When a
request does have to go to the server, ``_satisfy_request()``
(``src/server/pmix_server_get.c``) narrows the lookup to a single key only
for *reserved* keys; for a non-reserved key it returns everything that
process published. The comment in the code gives the reasoning — a request
for one value from a proc is usually followed by requests for more — and
the effect is that the first miss for a peer populates the cache for all
of that peer's data at once.

**A second fence does not, by itself, update what a reader sees.**
Suppose a process publishes ``mykey``, fences, and every peer reads it;
then it publishes a *new value* under the same ``mykey`` and fences again.
The new value does reach the other servers — the commit and the collective
work exactly as described above. But a peer that already read ``mykey`` has
it cached, and a plain ``PMIx_Get`` is answered from that cache, so it
keeps returning the first value.

That is deliberate, not an oversight, and ``PMIX_GET_REFRESH_CACHE`` is
the documented way to override it. Its definition in
``include/pmix_common.h.in`` says so directly:

   when retrieving data for a remote process, refresh the existing local
   data cache for the process in case new values have been put and
   committed by it since the last refresh

So a reader that expects an *updated* value has to ask for it:

.. code-block:: c

   pmix_info_t info;
   bool refresh = true;

   PMIX_INFO_LOAD(&info, PMIX_GET_REFRESH_CACHE, &refresh, PMIX_BOOL);
   rc = PMIx_Get(&peer, "mykey", &info, 1, &val);

Because of the first point above, one such refresh brings that peer's
whole published set up to date, not just the key named in the call. See
:ref:`PMIx_Get(3) <man3-PMIx_Get>` for the directive's full description.

This is easy to get wrong. The in-tree fence test has a ``--use-same-keys``
mode that re-publishes the same key names before each fence, and it read
them back with a plain ``PMIx_Get`` — so it saw the previous fence's values
and reported the library broken. The mode had never worked, and nothing
noticed because the harness printed the mismatch and still exited zero.
If you are chasing a "stale modex value," check for this before suspecting
the datastore.


The Key Index
-------------

Everything above describes strings moving between processes. Inside the
datastore, however, a key is not a string.

``pmix_hash_store`` (``src/util/pmix_hash.c``) converts the key before
storing anything::

    p = pmix_hash_lookup_key(UINT32_MAX, kin->key, keyindex);
    ...
    kid = p->index;

and the stored record carries only the number:

.. code-block:: c

   typedef struct {
       uint32_t index;
       uint32_t qualindex;
       pmix_value_t *value;
   } pmix_dstor_t;

The mapping lives in a ``pmix_keyindex_t``: a ``pmix_pointer_array_t``
of ``pmix_regattr_input_t`` entries in which an entry's **index is its
slot number**, plus a ``next_id`` counter. Retrieval reverses the
substitution — ``make_copy`` rebuilds a ``pmix_kval_t`` from
``p->string`` — so the conversion is invisible from outside.

There is exactly **one** key index per process,
``pmix_globals.keyindex``. Every caller in the tree passes ``NULL`` for
the ``kidx`` argument, and ``get_keyindex_ptr`` turns that ``NULL`` into
the global.

Reserved and non-reserved keys
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The index space has two halves, divided at ``PMIX_INDEX_BOUNDARY``:

* **Below the boundary** are the *reserved* keys — the ``PMIX_*``
  attributes defined by the Standard. ``pmix_init_registered_attrs``
  pre-loads them from the generated ``pmix_dictionary[]`` table at
  startup, in dictionary order, so their indices are fixed for the
  lifetime of the process and are identical in any two processes built
  from the *same* PMIx release.

* **At and above the boundary** are the *non-reserved* keys — anything an
  application invents and publishes with ``PMIx_Put``. These cannot be
  known in advance, so ``pmix_hash_lookup_key`` registers them on first
  sight, and ``pmix_hash_register_key`` assigns ``next_id++``.

The consequence is the crux of this document:

.. important::

   A non-reserved key's index is assigned **per process, in order of
   first encounter**. Two processes that publish different keys, or the
   same keys in a different order, assign different indices to the same
   key string. Nothing reconciles them.

Nor is the reserved half as stable as it first appears. The dictionary
is generated from the public headers, so a client and a server built
from *different* PMIx releases have different dictionaries — the same
attribute can occupy a different slot in each. Version interoperability
is a requirement, so no process may assume another's numbering.

Why this is normally invisible
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Because indices never cross a process boundary. Every wire format —
commit, the modex envelope, a ``PMIx_Get`` reply — carries the key
string, and each process re-derives its own index on receipt. A client,
its server, and a remote server therefore run three unrelated index
spaces that never need to agree.

The one place indices *are* shared is ``gds/shmem3``, and that is
precisely what makes it a special case.


Storing the Modex in Shared Memory
----------------------------------

On a node running many processes, every local client holding its own
copy of the full modex is pure duplication. The ``gds/shmem3`` component
exists to remove it: the server builds the data structures inside an
``mmap``'d segment and every local client maps that same segment
**read-only at the same virtual address**, so in-segment pointers
resolve without fix-up and N clients share one physical copy.

Applying that to modex data runs straight into the key index. The shared
hash table stores ``pmix_dstor_t`` records keyed by integer, the server
writes those integers, and every client reads them — so for the first
time, an index really does cross a process boundary.

What the job segment does today
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

For *job-level* data, ``shmem3`` handles this by having the server
dictate its numbering to its clients. ``register_job_info`` packs the
server's dictionary into the reply, and the client rebuilds
``pmix_globals.keyindex`` to match, appending any entries of its own that
the server did not send. The code's own comment states the reason
plainly:

.. code-block:: text

   We can _never_ assume that the indices in our dictionary match the
   ones in the server's dictionary. Even if the number of entries is the
   same, there is no guarantee that they are in the same order.

That works for job data because it happens once, during ``PMIx_Init``,
before the application has published anything. It does not generalize to
the modex:

* it ships only the reserved dictionary, and a modex is largely
  non-reserved keys;
* the server assigns non-reserved indices in *modex arrival* order, which
  no client can predict, so no scheme that assigns indices earlier can
  converge on it; and
* re-running the rebuild after a fence would renumber indices already
  recorded in the client's own local tables, silently invalidating its
  own stored data.

The approach that does work
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Rather than making every process agree on one global numbering, give the
shared data its own numbering and put it next to the data:

.. code-block:: text

   MODEX SEGMENT (server writes; clients map read-only)
    +---------------------------------------------+
    | pmix_shmem_header_t   (layout id stamp)     |
    +---------------------------------------------+
    | shared modex data                           |
    |   tma, current_addr                         |
    |   keyindex        <-- describes the table   |
    |   hashtab         (pmix_dstor_t.index ->)   |
    +---------------------------------------------+

The key index becomes a property of the segment, not of the process. The
server populates it as it stores; clients read it in place and pass it as
the ``kidx`` argument that ``pmix_hash_store`` and ``pmix_hash_fetch``
already accept. Indices then need only be consistent *within one
segment*, which is guaranteed because exactly one process writes them.

Three existing properties make this practical:

* ``pmix_keyindex_t`` is already allocator-aware — its constructor
  allocates through the ``pmix_tma_t`` of the object it hangs off, and
  ``pmix_pointer_array_t`` is likewise — so it can be built inside a
  segment with no new machinery.
* The segment index needs only the keys the modex actually carries, not
  the whole reserved dictionary.
* The job-data path above is untouched and keeps working as it does now.

One rule follows from the client's mapping being read-only:
**the retrieval path must never register a key.** The lookup used by
``pmix_hash_fetch`` has to fail rather than insert, because inserting
would mean writing into a read-only mapping. A key that was never stored
is simply not found, which is the correct answer anyway.

Per-namespace routing
^^^^^^^^^^^^^^^^^^^^^

All local clients *of a given namespace* share a datastore module, but
clients of *different* namespaces need not. A fence can therefore span a
namespace using ``shmem3`` and another using ``hash``, and the returned
modex has to be stored into each participating namespace's module.
``shmem3`` is built for this — its modex segment is per-namespace, so
each namespace's data lands in its own segment, and
``mark_modex_complete`` hands each client the segment information for
every namespace that took part.

Reaching the data from elsewhere
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A connected **tool** only ever uses ``hash``, but it may still ask for a
value that now lives in a namespace's shared segment. The server answers
on the tool's behalf: when a request cannot be satisfied from the
server's own module, it consults the module assigned to the namespace
that owns the target process. Retrieval returns fully-formed
``pmix_kval_t`` objects with their key strings restored, so the reply is
packed exactly as it always was — the requester never learns which
module produced it.


.. _modex-delta:

Planned: Delta Exchange and Data Deletion
-----------------------------------------

Everything above describes the library as it is today. This section
describes work that is designed but not yet implemented, tracked as
`openpmix#4087 <https://github.com/openpmix/openpmix/issues/4087>`_. It is
recorded here because the two halves are one problem: **the mechanism that
lets a modex carry only what changed is the same mechanism that lets a key
be deleted**, and neither can be built without the other.

Why deletion needs this
^^^^^^^^^^^^^^^^^^^^^^^

:ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`
removes entries from the server's global cache, but that cache is copied
into a namespace's datastore exactly once — in ``hash_cache_job_info``,
guarded by the per-namespace ``gdata_added`` flag — and nothing re-reads
it. A deregistration therefore governs namespaces registered *afterwards*
while a running job keeps its copy.

Closing that needs a delete-a-key path, and the components do not admit
one symmetrically. For ``hash`` the server can remove the key and message
its local clients. For ``shmem3`` it cannot: a client reads the shared
segment directly, so removing data from it would mean putting a lock on a
read path that is lock-free by design and is the whole reason the
component exists.

The way out is the discipline ``shmem3`` already follows for the modex:
**never write a segment a client can see — write a new one and search
back.** A deletion becomes a *tombstone* in a newer segment rather than an
erasure in an older one, and the search that finds it is the same search
that finds delta data.

Delta with a full resync
^^^^^^^^^^^^^^^^^^^^^^^^

The essential design point is that neither half is "delta only":

.. important::

   Both the client's commit and the server's fence contribution keep the
   cumulative fetch described above as a **full-resync fallback**, selected
   by a flag. Every case a delta cannot express — a tool that switched
   servers, a commit whose send failed, a singleton that later connected, a
   fence whose participant set is not covered by the previous one, data
   written by a path other than a commit — sets that flag and takes the old
   path. The delta is the fast path, not the only path.

Two watermarks, at two levels:

* **The client** sends what has been put since its last successful commit.
  It cannot use the fence as its boundary even if that were desirable,
  because a client never sees ``PMIX_COLLECT_DATA`` — the directive is
  packed verbatim into the fence message and interpreted only by the
  server.
* **The server** contributes what has arrived since this process last
  contributed to a collecting fence. A barrier-only fence exchanges
  nothing and so does not move the watermark.

**The server's watermark is qualified by the participant set.** A per-process
watermark alone is not sound: a process that contributed to a fence over one
set of peers would contribute nothing to a later fence over a different set,
and the servers holding only the second set's processes would never learn its
keys — two sub-communicators fencing independently is enough to reach it.
Each process is therefore stamped with the participant set of the fence it
last contributed to, and a delta is sent only when the current fence's set is
contained in that stamp. Otherwise the contribution is cumulative and the
stamp is replaced.

Telling a delta from a full contribution
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A receiving server must know which kind of contribution it is storing,
because ``gds/shmem3`` may drop the previous modex generation for a
cumulative one and must not for a delta. That is carried by the per-server
flag byte already in the envelope — the one that today distinguishes
``PMIX_COLLECT_YES`` from ``PMIX_COLLECT_NO``.

Reusing that byte is what makes the change safe across versions, and the
guard costs nothing because the *existing* code already implements it.
``pmix_gds_base_store_modex`` compares the byte pairwise across the
contributing servers and raises the ``collection-mismatch`` help message
when they disagree. In a job mixing releases, the older servers emit the
old value and the newer ones the new value, so the comparison fails and the
fence returns an error — loud, on both old and new receivers, rather than
silently storing a partial modex.

.. note::

   The byte must be a distinct constant, not an overloading of the
   tracker's ``collect_type``: that field is compared against
   ``PMIX_COLLECT_YES`` in several places that decide whether to collect at
   all.

**This much is implemented.** ``PMIX_MODEX_DELTA`` is defined, and
``pmix_gds_base_store_modex`` now screens the flag byte before comparing
it across servers — refusing a delta contribution with
``PMIX_ERR_NOT_SUPPORTED`` and the ``delta-modex-unsupported`` help
message, and an undefined value with ``PMIX_ERR_BAD_PARAM``. Nothing emits
the marker yet, so no behavior changes for a job whose nodes all run this
release. What it buys is that the refusal is in place *before* anything can
send one, which is why it landed first and on its own. Regression coverage
is ``test_store_modex_blob_info()`` in ``test/unit/gds_datastore.c``.

Screening the value matters independently of the delta work: the
cross-server comparison only asks whether the senders agree with each
other, so before this a byte they all agreed on and no datastore could act
on passed straight through and its blobs were stored as though they were an
ordinary full contribution.

Generations in shared memory
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``shmem3`` already gives each modex its own segment and hands the previous
one off, which is correct only while every generation is self-contained —
the component's own source says so. Once a contribution can be a delta,
generation N+1 no longer stands alone, so the segments become a
**newest-to-oldest chain** and a lookup walks it until it finds the key.

Three properties make that work without disturbing the read path:

* The backing file is reference counted, so releasing the server's handle
  leaves a client that still has the segment mapped with a valid mapping.
* Clients already tell generations apart by backing path, which the segment
  blob carries, so **nothing on the wire changes**.
* Each segment carries its own key index, so a generation is self-describing
  and no two generations need to agree about numbering.

Because the flag byte says which kind of contribution arrived, a cumulative
modex keeps today's behavior — drop the previous generation, one segment
live — and only a delta grows the chain. The interim state, after the chain
exists but before contributions are deltas, therefore costs nothing.

Deleting a key
^^^^^^^^^^^^^^

Deletion is expressed through ``PMIx_Put`` rather than a new API, using four
additional ``pmix_scope_t`` values — ``PMIX_DEL_LOCAL``, ``PMIX_DEL_REMOTE``,
``PMIX_DEL_GLOBAL`` and ``PMIX_DEL_INTERNAL`` — that name the same audiences
as their storing counterparts but direct that the key be removed. The value
is ``NULL``. This is a sanctioned extension rather than a deviation:
:ref:`PMIx_Put(3) <man3-PMIx_Put>` already states that an implementation may
support additional scope values and must answer ``PMIX_ERR_NOT_SUPPORTED``
for one it does not.

``PMIX_DEL_INTERNAL`` takes effect immediately, since nothing leaves the
process. The other three are applied locally and then travel in the commit
stream — the commit message is already a sequence of ``{scope, buffer}``
blocks, so a deletion block needs no format change at all, only the new
scope value.

The two datastores then diverge, exactly as the deletion problem always
predicted:

* ``hash`` removes the key outright and the server messages each local
  client to do the same in its own tables.
* ``shmem3`` writes a **tombstone** — the key with a ``PMIX_UNDEF`` value —
  into a new segment in the chain. A lookup walking newest to oldest that
  reaches a tombstone stops and reports the key as not found, without any
  published segment ever being written.

.. warning::

   The tombstone walk has to be consulted by the **job-data** lookup, not
   only the modex one. ``deregister_resources`` targets job-level
   information, which lives in the job segment rather than the modex
   segment — so a chain that covers only the modex closes none of the
   problem this work exists to solve.

An older server silently discards a scope it does not recognize rather than
reporting an error, so a delete issued against one would appear to succeed
and do nothing. ``PMIx_Put`` therefore checks the server's version and
refuses with ``PMIX_ERR_NOT_SUPPORTED`` up front, and a ``PMIX_CAP_``
capability flag lets companion projects detect support at build time.


Summary
-------

* ``PMIx_Put`` is local; ``PMIx_Commit`` sends; ``PMIx_Fence`` with
  ``PMIX_COLLECT_DATA`` exchanges.
* Every wire format carries **key strings**. Indices are an internal
  storage detail.
* Inside the datastore a key is a ``uint32_t`` index. Reserved keys are
  pre-loaded from the generated dictionary; non-reserved keys are
  numbered on first sight, **per process**, so no two processes can be
  assumed to agree — and neither can two PMIx releases.
* That is harmless while indices stay inside one process, and becomes the
  governing constraint the moment a datastore is shared between
  processes.
* ``gds/shmem3`` resolves it for job data by having the server dictate
  its numbering once at initialization, and for modex data by giving each
  modex segment its own key index, written by the one process that owns
  the segment.
* Both the commit and the fence contribution are **cumulative today**. The
  planned delta exchange keeps the cumulative path as a resync fallback,
  marks a delta in the envelope's existing flag byte so a mixed-version job
  fails loudly, and turns ``shmem3``'s modex generations into a chain that
  is searched newest to oldest.
* **Deleting a key is the same mechanism seen from the other side**: a
  tombstone in a newer segment, found by that same search — which is what
  finally lets a deregistration retract information a running job already
  holds.
