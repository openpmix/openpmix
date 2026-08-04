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
copy of the key string and the value — compressing large strings into a
``PMIX_COMPRESSED_STRING`` — and hands it to the datastore::

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

``modex_cbfunc`` may run on the host's own thread, so it does nothing but
thread-shift onto the progress thread, landing in ``_mdxcbfunc``
(``src/server/pmix_server.c``). That function stores the returned blob
and then replies to each waiting participant::

    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &xfer, scd->data, scd->ndata);
    PMIX_GDS_STORE_MODEX(rc, &xfer, tracker);

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
whole published set up to date, not just the key named in the call.

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
