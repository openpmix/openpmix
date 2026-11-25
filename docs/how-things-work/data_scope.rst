.. _data-scope-label:

Data Scope: Who Sees What a Process Posts
=========================================

A process posts a value with ``PMIx_Put`` at one of four scopes, and other
processes read it with ``PMIx_Get``. Which value a reader gets, and
whether it gets one at all, depends on two things: the scope the value
was posted at, and where the reader is relative to the process that
posted it. This page states the rules, shows the full answer for every
combination, covers the legacy "globally unique key" form, and says
where in the library each rule is applied.

The user-facing statement of the same rules is in
:ref:`PMIx_Put(3) <man3-PMIx_Put>` ("SCOPE") and
:ref:`PMIx_Get(3) <man3-PMIx_Get>` ("DATA SCOPE"). :doc:`modex` follows
a value from ``PMIx_Put`` to the aggregated result, and :doc:`shmem3`
describes where that result lives on a node.


Posting: who a value is for
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 18 41 41

   * - Posted at
     - Visible to processes on the poster's node
     - Visible to processes on other nodes
   * - ``PMIX_LOCAL``
     - yes
     - no |mdash| it never leaves the node
   * - ``PMIX_REMOTE``
     - no |mdash| it is not shared with the poster's own node
     - yes
   * - ``PMIX_GLOBAL``
     - yes
     - yes
   * - ``PMIX_INTERNAL``
     - no |mdash| only the poster itself
     - no

Read the same table from the reader's side:

.. list-table::
   :header-rows: 1
   :widths: 30 35 35

   * - Target (the poster) is
     - The reader may see the target's
     - The reader never sees the target's
   * - on the reader's node
     - ``PMIX_LOCAL`` and ``PMIX_GLOBAL`` data
     - ``PMIX_REMOTE`` data
   * - on another node
     - ``PMIX_REMOTE`` and ``PMIX_GLOBAL`` data
     - ``PMIX_LOCAL`` data

**A key may be posted at both** ``PMIX_LOCAL`` **and** ``PMIX_REMOTE``
**with different values.** That is legitimate and useful. Open MPI's
``pml/ucx`` posts its full worker address (which carries the shared-memory
transports) at ``PMIX_LOCAL`` and a network-only one at ``PMIX_REMOTE``.
A reader on the poster's node gets the first, and a reader anywhere else
gets the second. It never gets both, and it never gets the wrong one.
Handing a same-node reader the ``PMIX_REMOTE`` copy sends node-local
traffic through the network device (openmpi#14873).


Asking: what a scope on ``PMIx_Get`` means
------------------------------------------

A ``PMIX_DATA_SCOPE`` directive on a get names **where the target is**,
not which of its values to read:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - ``PMIX_DATA_SCOPE``
     - Means
   * - (none)
     - The target may be anywhere. Return what is visible from where it
       is.
   * - ``PMIX_LOCAL``
     - The target is on my node.
   * - ``PMIX_REMOTE``
     - The target is on another node.
   * - ``PMIX_GLOBAL``
     - The target may be on either. The same as giving no scope.

A scope that contradicts where the target actually is finds nothing,
including data the target posted at ``PMIX_GLOBAL``. ``PMIX_LOCAL`` asks
for data posted by a process on my node, and a process on another node is
not one. It does not ask for that process's ``PMIX_LOCAL`` data, which
would make no sense: that data never left its node.

Only the scope the **reader** gave counts. The library picks a table or a
segment internally, and a request it parked is later woken by data
arriving at some scope. Neither of those is the reader's scope, and
neither is ever judged against the rule.


The full answer
---------------

Suppose a process ``P`` posts four keys:

* ``dual`` |mdash| ``"L"`` at ``PMIX_LOCAL`` and ``"R"`` at ``PMIX_REMOTE``
* ``remote_only`` |mdash| ``"R"`` at ``PMIX_REMOTE``
* ``local_only`` |mdash| ``"L"`` at ``PMIX_LOCAL``
* ``glob`` |mdash| ``"G"`` at ``PMIX_GLOBAL``

commits them, and takes part in a fence. Another process asks for each
key of ``P``. NF is ``PMIX_ERR_NOT_FOUND``.

A reader on ``P``'s node:

.. list-table::
   :header-rows: 1
   :widths: 20 20 20 20 20

   * - Key
     - no scope
     - ``PMIX_LOCAL``
     - ``PMIX_REMOTE``
     - ``PMIX_GLOBAL``
   * - ``dual``
     - ``"L"``
     - ``"L"``
     - NF
     - ``"L"``
   * - ``remote_only``
     - NF
     - NF
     - NF
     - NF
   * - ``local_only``
     - ``"L"``
     - ``"L"``
     - NF
     - ``"L"``
   * - ``glob``
     - ``"G"``
     - ``"G"``
     - NF
     - ``"G"``

A reader on another node:

.. list-table::
   :header-rows: 1
   :widths: 20 20 20 20 20

   * - Key
     - no scope
     - ``PMIX_LOCAL``
     - ``PMIX_REMOTE``
     - ``PMIX_GLOBAL``
   * - ``dual``
     - ``"R"``
     - NF
     - ``"R"``
     - ``"R"``
   * - ``remote_only``
     - ``"R"``
     - NF
     - ``"R"``
     - ``"R"``
   * - ``local_only``
     - NF
     - NF
     - NF
     - NF
   * - ``glob``
     - ``"G"``
     - NF
     - ``"G"``
     - ``"G"``

A not-visible answer is returned at once. The server does not park the
request waiting for data that will never become visible to the reader,
and a direct-modex request for a key the target posted only at
``PMIX_LOCAL`` is answered "not found" rather than left to time out.

These are exactly the answers ``examples/get_scope.c`` checks, from every
rank of a job against every other rank. Each answer is asked both as the
first question to that peer and after the reader has cached earlier
answers, and both with and without the fence collecting data.

What the rules do not cover:

* **Reserved keys.** The host's job, application, node and session data
  (keys beginning with ``"pmix"``) are the same for every reader and are
  not subject to these rules.
* **Per-process data the host supplied at registration.** The server
  stores it at ``PMIX_GLOBAL``, so every reader sees it.
* **A process's own data.** A process reading what it posted itself gets
  it back whatever the scope and whatever rank form it asked with.
* **The host's own** ``PMIx_Get``. The host environment is not a process
  of the job, and it reads a job's data as stored. Its direct-modex
  check depends on that: it is on the target's node, and is looking for
  the data the target posted for other nodes.
* **``PMIX_INTERNAL``.** It never leaves the poster, so no other process
  ever sees it.


Legacy: a key that names no process
-----------------------------------

PMI-1 and PMI-2 had one key-value space per job, and a key there had to be
unique across the job. Anyone could read it without knowing who posted
it. PMIx keeps that form, and MPI libraries still use it:

* **Posting** is an ordinary ``PMIx_Put``. The legacy shims this tree used
  to carry, and MPICH's PMIx support today, post such keys at
  ``PMIX_GLOBAL``. MPICH posts node-only values at ``PMIX_LOCAL``.
* **Reading** names no rank: ``PMIx_Get`` with a ``NULL`` ``proc``, or
  with the namespace and ``PMIX_RANK_UNDEF``. The client turns a ``NULL``
  ``proc`` into its own namespace with ``PMIX_RANK_UNDEF``
  (``process_request()`` in ``src/client/pmix_client_get.c``).

``PMIX_RANK_UNDEF`` means "any process of this namespace". The rules are
applied process by process: a rank on the reader's node is searched for
its ``PMIX_LOCAL`` and ``PMIX_GLOBAL`` data, and any other rank for its
``PMIX_REMOTE`` and ``PMIX_GLOBAL`` data. A scope on the get narrows the
search the same way it narrows a get of one rank: ``PMIX_LOCAL`` searches
only the ranks on the reader's node, ``PMIX_REMOTE`` only the others, and
``PMIX_GLOBAL`` or no scope searches every rank. Job-level data under the
key is found too, as it always was.

For a globally unique key ``K`` posted by one process ``P``:

.. list-table::
   :header-rows: 1
   :widths: 25 25 25 25

   * - ``K`` posted at
     - Reader on ``P``'s node
     - Reader on another node
     - Note
   * - ``PMIX_GLOBAL``
     - found
     - found
     - the legacy form
   * - ``PMIX_LOCAL``
     - found
     - NF
     - never left the node
   * - ``PMIX_REMOTE``
     - NF
     - found
     - not shared with ``P``'s node

The last row is the only behaviour this form changed. Before the rules
were enforced, a same-node reader was handed the ``PMIX_REMOTE`` copy. A
key meant for every reader belongs at ``PMIX_GLOBAL``.

Three more properties hold:

* **A key not posted yet.** The server parks the request until the key
  arrives, as it always has. When it does arrive, it is judged with the
  scope the reader gave, not the scope it arrived at.
* **A key the reader may never see.** If the reader's server holds the
  key in a copy the reader may not see - a same-node poster's
  ``PMIX_REMOTE`` copy - it answers NF at once. A key that never reaches
  the reader's server at all - a poster on another node's
  ``PMIX_LOCAL`` key - looks to that server like one not posted yet, and
  the request waits. A legacy reader that may look for such a key should
  bound the wait with ``PMIX_IMMEDIATE`` or ``PMIX_TIMEOUT``.
* **Data that was not collected.** A get naming no rank has nobody to ask
  on demand, so it finds only what a collecting fence (or the reader's
  own node) brought to the reader's server. Direct modex serves only gets
  that name the rank.
* **Two posters of one "unique" key.** The lowest rank whose copy the
  reader may see answers. ``gds/hash`` and ``gds/shmem3`` search in the
  same order, so the answer does not depend on the datastore.
* **The reader's own key.** It always finds it.

``PMIX_RANK_WILDCARD`` **is not this form.** A get with
``PMIX_RANK_WILDCARD`` asks for the namespace's job-level data, which the
host provides. Data a process posted with ``PMIx_Put`` is never returned
for it, on either the client or the server. A caller that does not know
which rank posted a key must use a ``NULL`` ``proc`` or
``PMIX_RANK_UNDEF``.

How MPICH uses PMIx today, for reference, since each pattern must keep
working:

* Every value is put and committed at once, at ``PMIX_GLOBAL``, or at
  ``PMIX_LOCAL`` for node-only data. Reads name a specific rank (its
  broadcast, allgather and business-card exchanges) and give no scope.
* A broadcast is put, commit, fence, get, and MPICH runs many of them
  during start-up. So a job sees many fences, each adding a little data.
* A node-only broadcast fences just the processes on one node, with
  ``PMIX_COLLECT_DATA``. The server completes such a fence itself.
* Job attributes are read with ``PMIX_RANK_WILDCARD`` and
  ``PMIX_OPTIONAL``.


Who is "on my node"
-------------------

A target is on the reader's node when its ``PMIX_HOSTNAME`` is the
reader's own hostname or one of its aliases. The target's hostname comes
from the job data the host registered: stated per process, or derived
from the node and proc maps when the host does not state it. The same
test is used on the client and on the server, so the two always agree,
and it reads one stored value rather than walking a list.

Node IDs are not used for this. A host may number its nodes as it likes,
and the IDs a datastore derives are indices into one job's node map, so
two jobs can give one node different IDs. A hostname names the same node
in every job.

When the target's node is not known, a ``PMIX_LOCAL`` or ``PMIX_REMOTE``
scope on the get is taken as the reader's word for where the target is.
With no scope, every copy is searched, local first, as before the rules
existed. That covers a member of a group whose job data the reader does
not hold. A rank that is restarted on another node is registered with its
new host, so it stops counting as local on the node it left.

``pmix_gds_base_host_locality()`` and ``pmix_gds_base_locality_or_scope()``
(``src/mca/gds/base/base.h``) give that answer from a hostname.


Where the rules are applied
---------------------------

The rule is a property of **answering a** ``PMIx_Get`` **from a process
of the job**, not of the datastore. The library's own fetches read a
scope's stored data exactly as stored, and they must:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Fetch
     - Applies the rule?
   * - ``PMIx_Get`` on a client or tool (``try_local_fetch``,
       ``get_data``)
     - Yes.
   * - The server answering a client's get (``pmix_server_get``,
       ``_satisfy_request``)
     - Yes. A contradicting scope is answered ``PMIX_ERR_NOT_FOUND``
       before anything is fetched, parked or sent to the host.
   * - The server refreshing a client's cache
       (``pmix_server_refresh_cache``, for ``PMIX_GET_REFRESH_CACHE``)
     - Yes. The client caches the reply with no scope, so it carries what
       a get by that client may see: a same-node target's ``PMIX_LOCAL``
       and ``PMIX_GLOBAL`` data, never its ``PMIX_REMOTE`` copy.
   * - The host's own ``PMIx_Get``
     - No. The host is not a process of the job.
   * - A direct-modex answer to another node
       (``PMIx_server_dmodex_request``)
     - It answers a reader on another node, so it returns the target's
       ``PMIX_REMOTE`` and ``PMIX_GLOBAL`` data. When the request names a
       ``PMIX_REQUIRED_KEY``, the server waits for that key at those
       scopes. A key the target posted only at ``PMIX_LOCAL`` is answered
       ``PMIX_ERR_EXISTS_OUTSIDE_SCOPE``, which the requester reports as
       ``PMIX_ERR_NOT_FOUND``. The reader's own ``PMIX_DATA_SCOPE`` is not
       sent: it means nothing to a server on another node, and the request
       is shared by readers who may have asked with different scopes.
   * - A fence's contribution (``pmix_server_collect_data``)
     - Does not read the datastore at all. It packs each process's
       committed ``PMIX_REMOTE`` and ``PMIX_GLOBAL`` data from its modex
       log (see :ref:`modex-delta`).

The get paths set ``pmix_cb_t.get_visibility``, and ``PMIX_GDS_FETCH_KV``
hands it to the datastore as the ``visibility`` argument of its fetch
(``src/mca/gds/gds.h``). Both datastore components honor it:

* **gds/hash** keeps a process's ``PMIX_LOCAL`` data in a ``local`` table
  and its ``PMIX_REMOTE`` data in a ``remote`` table, with ``PMIX_GLOBAL``
  in both. A get reads ``local`` for a same-node target and ``remote`` for
  any other, never both.
* **gds/shmem3** keeps the same split in shared memory: a **local chain**
  holding the server's own clients' ``PMIX_LOCAL`` and ``PMIX_GLOBAL``
  data, beside the **modex chain** holding every process's
  ``PMIX_REMOTE`` and ``PMIX_GLOBAL`` data. A get reads the local chain
  for a same-node target and the modex chain for any other, never both.
  See :doc:`shmem3`, "Local data".

The client keeps a cache of server replies with no scope attached, which
is safe only because the server never sends a reader anything it may not
see. Before these rules were enforced it did. A ``PMIX_REMOTE``-scoped
get of a same-node peer cached that peer's ``PMIX_REMOTE`` data, and every
later plain get of the same key then returned it.


Testing
-------

* ``test/unit/run_getscope.pl`` drives ``examples/get_scope`` under
  ``test/simple/simptest``, so every process is on one node. It runs the
  same-node half of the tables and the legacy table twice:

  * once with the server completing the fence itself, as it does by
    default when all participants are its own clients;
  * once with ``pmix_server_fence_localonly_opt`` off, so the fence goes
    through the host and a modex is stored as well.

  On Linux both runs read ``gds/shmem3``'s local chain. The second also
  has a modex chain beside it, which a same-node get must not read.
* ``examples/get_scope`` also checks:

  * a cpuset value posted at ``PMIX_LOCAL`` and read by a same-node
    peer;
  * a ``NULL``-key get of every peer, which must return each visible key
    once and nothing else;
  * the legacy table, read with a ``NULL`` proc and with
    ``PMIX_RANK_UNDEF``, unscoped and scoped, and a
    ``PMIX_RANK_WILDCARD`` get of a posted key, which must find nothing;
  * a key overwritten after a fence: read with ``PMIX_GET_REFRESH_CACHE``
    until the new value shows, then with a plain get, and again after the
    next fence;
  * twenty put-commit-fence rounds in a row, as MPICH's start-up does.
* ``test/unit/server_dmodex.c`` holds the get rule and the direct-modex
  answer apart for one rank's data, including ``PMIX_REQUIRED_KEY`` for a
  key posted only at ``PMIX_LOCAL``. It fails if the rule is applied to
  the wrong one.
* ``contrib/dockerswarm/run-gds-tests.sh`` runs ``examples/get_scope``
  with two ranks on each of two nodes, under both datastore components,
  with and without the fence collecting data, so both tables and the
  legacy table are checked across a real node boundary.
