.. _man3-PMIx_Query_info:

PMIx_Query_info
===============

.. include_body

``PMIx_Query_info``, ``PMIx_Query_info_nb`` |mdash| Query information about the
system in general.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Query_info(pmix_query_t queries[], size_t nqueries,
                                 pmix_info_t **results, size_t *nresults);

   pmix_status_t PMIx_Query_info_nb(pmix_query_t queries[], size_t nqueries,
                                    pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # each query is a dict with a list of key strings and a list of
  # ``pmix_info_t`` qualifier dictionaries
  pyq = [{'keys': [PMIX_QUERY_NAMESPACES],
          'qualifiers': []}]
  rc, results = foo.query(pyq)
  # results is a list of Python ``pmix_info_t`` dictionaries


INPUT PARAMETERS
----------------

* ``queries``: Array of ``pmix_query_t`` structures. Each structure carries a
  NULL-terminated array of string ``keys`` identifying the information being
  requested, plus an optional array of ``qualifiers`` (a ``pmix_info_t`` array
  with ``nqual`` elements) that refine or scope those keys (see `DIRECTIVES`_).
* ``nqueries``: Number of elements in the ``queries`` array. Must be non-zero, and
  ``queries`` must be non-``NULL``.


OUTPUT PARAMETERS
-----------------

* ``results`` (blocking form): Address where a pointer to a freshly allocated
  array of ``pmix_info_t`` holding the results is returned. Set to ``NULL`` when no
  data is returned. The caller must release the array with ``PMIX_INFO_FREE``.
* ``nresults`` (blocking form): Address where the number of elements in
  ``results`` is returned; set to zero when no data is returned.

The non-blocking form replaces ``results``/``nresults`` with a callback:

* ``cbfunc``: Callback function of type :ref:`pmix_info_cbfunc_t <man5-pmix_info_cbfunc_t>` invoked with the
  status and result array once the query completes.
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Query information about the system in general. This can include a list of active
namespaces, the fabric topology, or node-specific information such as the list of
peers executing on a given node. Unlike :ref:`PMIx_Get(3) <man3-PMIx_Get>`, which
retrieves a single key value scoped to a specific session, job, application,
process, or node, ``PMIx_Query_info`` supports multiple keys per request, complex
search criteria expressed through qualifiers, and dynamic system-level information
that is not part of the job data provided at start of execution.

``PMIx_Query_info`` is the blocking form: it does not return until the query has
completed, at which point ``results`` and ``nresults`` are set. ``PMIx_Query_info_nb``
is the non-blocking form: it returns immediately after accepting the request for
processing, and the provided ``cbfunc`` is invoked with the final status and result
array. The blocking form is implemented in terms of the non-blocking form.

The library first attempts to satisfy each key from its local cache. Keys that
cannot be resolved locally are forwarded to the host environment (via the PMIx
server) for resolution. A small number of keys |mdash| notably the ABI version
queries |mdash| are always resolved locally. Results returned from the host
environment are cached so that subsequent retrievals can succeed with minimal
overhead; include the ``PMIX_QUERY_REFRESH_CACHE`` qualifier to bypass the cache
and obtain fresh values.

``PMIx_Query_info`` is normally called between initialization
(:ref:`PMIx_Init(3) <man3-PMIx_Init>`) and finalization
(:ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`). As an exception, the blocking form
may be called **before** initialization when used exclusively with the
``PMIX_QUERY_STABLE_ABI_VERSION`` and/or ``PMIX_QUERY_PROVISIONAL_ABI_VERSION``
keys, which are computed entirely within the local library.

As with all non-blocking PMIx APIs, callers of ``PMIx_Query_info_nb`` **must** keep
the ``queries`` array valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

Each ``pmix_query_t`` combines a list of **keys** (the information requested) with
a list of **qualifiers** (attributes that scope or refine the request). The most
commonly used keys are listed first, followed by the remaining defined keys grouped
by purpose, and then the qualifiers. An implementation is not required to support
any particular key, and an unsupported key is handled the same way as a key that
could not be found. The value type shown for a key is the type of the value it
*returns*; the type shown for a qualifier is the type of data provided with it.

Commonly used **keys**:

* ``PMIX_QUERY_NAMESPACES`` |mdash| return a comma-delimited list of the active
  namespaces known to the server.
* ``PMIX_QUERY_JOB_STATUS`` |mdash| return the status of a specified, currently
  executing job.
* ``PMIX_QUERY_QUEUE_LIST`` |mdash| return a list of scheduler queues.
* ``PMIX_QUERY_PROC_TABLE`` / ``PMIX_QUERY_LOCAL_PROC_TABLE`` |mdash| return the
  process table (all processes, or only those local to a node) for a namespace.
* ``PMIX_QUERY_SPAWN_SUPPORT`` |mdash| return a list of the attributes supported by
  the ``PMIx_Spawn`` API.
* ``PMIX_QUERY_MEMORY_USAGE`` |mdash| return memory usage statistics.
* ``PMIX_QUERY_ATTRIBUTE_SUPPORT`` |mdash| return the attributes supported by a
  named function. Must appear first in the ``keys`` array, followed by the
  user-level API names (e.g., ``"PMIx_Get"``) whose support is being queried.
* ``PMIX_QUERY_NUM_PSETS`` / ``PMIX_QUERY_PSET_NAMES`` |mdash| return the number,
  or the names, of the defined process sets.
* ``PMIX_GROUP_NAMES`` (pmix_data_array_t*) |mdash| return an array of the string
  names of the process groups in which the given process is a member. The target
  process is identified with the usual ``PMIX_PROCID`` (or ``PMIX_NSPACE`` /
  ``PMIX_RANK``) qualifier. This key may also be passed directly to
  :ref:`PMIx_Get(3) <man3-PMIx_Get>` to retrieve the same information for a single
  process.
* ``PMIX_QUERY_AVAIL_SERVERS`` |mdash| scan the local node for PMIx servers the
  caller could connect to.
* ``PMIX_QUERY_STABLE_ABI_VERSION`` / ``PMIX_QUERY_PROVISIONAL_ABI_VERSION``
  |mdash| return the stable or provisional Standard ABI version supported by the
  library. These are resolved locally and may be queried before initialization.

Additional keys, grouped by purpose |mdash| introspection and support:

* ``PMIX_QUERY_SUPPORTED_KEYS`` (char*) |mdash| return a comma-delimited list of the
  keys supported by the query function.
* ``PMIX_QUERY_DEBUG_SUPPORT`` (char*) |mdash| return a comma-delimited list of the
  supported debugger attributes.
* ``PMIX_QUERY_AUTHORIZATIONS`` (pmix_data_array_t*) |mdash| return the operations
  the tool is authorized to perform.

Namespaces and processes:

* ``PMIX_QUERY_NAMESPACE_INFO`` (pmix_data_array_t*) |mdash| return an array of
  information about the active namespaces; each element includes the namespace and
  the command line of the application executing within it. Accepts an optional
  ``PMIX_NSPACE`` qualifier to select one namespace.
* ``PMIX_QUERY_RESOLVE_PEERS`` (pmix_data_array_t*) |mdash| return the process IDs of
  the procs from a given namespace executing on a specified node, as if answering a
  :ref:`PMIx_Resolve_peers(3) <man3-PMIx_Resolve_peers>` request on the caller's
  behalf.
* ``PMIX_QUERY_RESOLVE_NODE`` (char*) |mdash| return a comma-delimited list of the
  nodes on which procs from a specified namespace have been placed, as if answering
  a :ref:`PMIx_Resolve_nodes(3) <man3-PMIx_Resolve_nodes>` request on the caller's
  behalf.

Scheduler, queues, and allocations:

* ``PMIX_QUERY_QUEUE_STATUS`` (pmix_data_array_t*) |mdash| return the name and status
  of each scheduler queue. Accepts an optional ``PMIX_ALLOC_QUEUE`` qualifier to
  select one queue.
* ``PMIX_QUERY_ALLOC_STATUS`` (char*) |mdash| return a string reporting the status of
  an allocation request. Requires a ``PMIX_ALLOC_REQUEST_ID`` qualifier.
* ``PMIX_QUERY_ALLOCATION`` (pmix_data_array_t*) |mdash| return an array describing
  the nodes in an allocation. Accepts a ``PMIX_ALLOC_ID`` qualifier to select a
  specific allocation.
* ``PMIX_QUERY_ALLOC_IDS`` (pmix_data_array_t*) |mdash| return the allocation IDs
  known to the server.
* ``PMIX_QUERY_ALLOC_PROPERTIES`` (pmix_data_array_t*) |mdash| return the properties
  of a specific allocation. Requires a ``PMIX_ALLOC_ID`` qualifier, and accepts an
  optional ``PMIX_ALLOC_PROPERTY`` qualifier to select a single property.
* ``PMIX_QUERY_MIN_ALLOC_UNIT`` (pmix_data_array_t*) |mdash| return the resources
  composing the minimum allocatable unit for the system.
* ``PMIX_QUERY_RES_BLOCKS`` (pmix_data_array_t*) |mdash| return the names of the
  currently defined resource blocks.
* ``PMIX_QUERY_RES_BLOCK_DEF`` (pmix_data_array_t*) |mdash| given a resource block
  name as its qualifier, return an array of ``pmix_resource_unit_t`` describing the
  resources contained in that block.
* ``PMIX_QUERY_AVAILABLE_SLOTS`` (uint32_t) |mdash| return the number of slots
  currently available in the session (a point-in-time snapshot). Accepts an optional
  ``PMIX_SESSION_ID`` qualifier.

Process sets and groups:

* ``PMIX_QUERY_PSET_MEMBERSHIP`` (pmix_data_array_t*) |mdash| return an array of
  ``pmix_proc_t`` naming the members of a specified process set.
* ``PMIX_QUERY_NUM_GROUPS`` (size_t) |mdash| return the number of process groups
  defined in the specified range (defaults to the session). Accepts an optional
  ``PMIX_RANGE`` qualifier.
* ``PMIX_QUERY_GROUP_NAMES`` (pmix_data_array_t*) |mdash| return the string names of
  the process groups defined in the specified range (defaults to the session).
  Accepts an optional ``PMIX_RANGE`` qualifier. Distinct from ``PMIX_GROUP_NAMES``
  above, which returns the groups a *given process* belongs to.
* ``PMIX_QUERY_GROUP_MEMBERSHIP`` (pmix_data_array_t*) |mdash| return an array of
  ``pmix_proc_t`` naming the members of a specified process group. Requires a
  ``PMIX_GROUP_ID`` qualifier.

Resource usage, devices, and storage:

* ``PMIX_QUERY_PROC_RESOURCE_USAGE`` (pmix_proc_t*) |mdash| return the resource-usage
  statistics for the specified process(es). A namespace combined with
  ``PMIX_RANK_WILDCARD`` returns an array with one assembly per process. Accepts
  ``PMIX_SESSION_ID``, ``PMIX_NODEID``, or ``PMIX_HOSTNAME`` qualifiers.
* ``PMIX_QUERY_NODE_RESOURCE_USAGE`` (char*) |mdash| return the resource-usage
  statistics for the specified node(s). Accepts ``PMIX_SESSION_ID``, ``PMIX_NSPACE``,
  or ``PMIX_JOBID`` qualifiers.
* ``PMIX_QUERY_DEVICES`` (pmix_data_array_t*) |mdash| return an array of
  ``pmix_device_t`` for the devices in the current topology that meet the query
  qualifications, e.g., a ``PMIX_DEVICE_TYPE`` qualifier.
* ``PMIX_QUERY_STORAGE_LIST`` (char*) |mdash| return a comma-delimited list of
  identifiers for all available storage systems.

Storage system information. ``PMIX_QUERY_STORAGE_LIST`` returns the available
storage-system identifiers; the following keys return the properties of a
particular system, selected with a ``PMIX_STORAGE_ID`` qualifier:

* ``PMIX_STORAGE_PATH`` (char*) |mdash| the mount point corresponding to the
  specified storage ID.
* ``PMIX_STORAGE_VERSION`` (char*) |mdash| the version string for the storage
  system.
* ``PMIX_STORAGE_MEDIUM`` (pmix_storage_medium_t) |mdash| the storage medium(s) the
  system uses (e.g., SSD, HDD, tape). See
  :ref:`pmix_storage_medium_t(5) <man5-pmix_storage_medium_t>`.
* ``PMIX_STORAGE_ACCESSIBILITY`` (pmix_storage_accessibility_t) |mdash| the
  accessibility level of the storage system (e.g., node, session). See
  :ref:`pmix_storage_accessibility_t(5) <man5-pmix_storage_accessibility_t>`.
* ``PMIX_STORAGE_PERSISTENCE`` (pmix_storage_persistence_t) |mdash| the persistence
  level of the storage system (e.g., scratch, archive). See
  :ref:`pmix_storage_persistence_t(5) <man5-pmix_storage_persistence_t>`.
* ``PMIX_STORAGE_CAPACITY_LIMIT`` (uint64_t) |mdash| the overall capacity of the
  storage system, in base-2 megabytes.
* ``PMIX_STORAGE_CAPACITY_USED`` (double) |mdash| the overall used capacity of the
  storage system, in bytes.
* ``PMIX_STORAGE_OBJECT_LIMIT`` (uint64_t) |mdash| the overall limit on the number
  of objects (e.g., inodes) in the storage system.
* ``PMIX_STORAGE_OBJECTS_USED`` (uint64_t) |mdash| the number of objects currently
  in use in the storage system.
* ``PMIX_STORAGE_BW_CUR`` (double) |mdash| the recently observed bandwidth of the
  storage system, in bytes/sec.
* ``PMIX_STORAGE_BW_MAX`` (double) |mdash| the maximum (theoretical or observed)
  bandwidth of the storage system, in bytes/sec.
* ``PMIX_STORAGE_IOPS_CUR`` (double) |mdash| the recently observed I/O operations
  per second for the storage system.
* ``PMIX_STORAGE_IOPS_MAX`` (double) |mdash| the maximum (theoretical or observed)
  I/O operations per second for the storage system.
* ``PMIX_STORAGE_MINIMAL_XFER_SIZE`` (double) |mdash| the storage system's atomic
  unit of transfer (e.g., block size), in bytes.
* ``PMIX_STORAGE_SUGGESTED_XFER_SIZE`` (double) |mdash| the suggested transfer size
  for the storage system, in bytes.

The bandwidth, IOPS, and suggested-transfer-size keys may be further qualified by
``PMIX_STORAGE_ACCESS_TYPE`` to select read, write, or read/write access.

The following two keys appear only in returned results and **cannot** be used as
input to ``PMIx_Query_info`` or ``PMIx_Get``:

* ``PMIX_QUERY_RESULTS`` (pmix_data_array_t*) |mdash| the array of results produced
  for a given ``pmix_query_t``. When the query carried qualifiers, the first element
  is a ``PMIX_QUERY_QUALIFIERS`` entry; each remaining element is a ``pmix_info_t``
  pairing a queried key with its returned value.
* ``PMIX_QUERY_QUALIFIERS`` (pmix_data_array_t*) |mdash| the qualifiers that were
  included in the query that produced the accompanying results.

Commonly used **qualifiers**:

* ``PMIX_QUERY_REFRESH_CACHE`` (bool) |mdash| bypass the local cache and retrieve a
  fresh value, refreshing the cache on return. Required on each query for which
  current (non-cached) data is needed.
* ``PMIX_SESSION_INFO`` / ``PMIX_JOB_INFO`` / ``PMIX_APP_INFO`` /
  ``PMIX_NODE_INFO`` / ``PMIX_PROC_INFO`` (bool) |mdash| scope the query to
  session-, job-, application-, node-, or process-level information, respectively.
* ``PMIX_PROCID`` (pmix_proc_t) |mdash| identify the specific process whose
  information is being requested. The identifier applies to every key in that
  ``pmix_query_t``.
* ``PMIX_NSPACE`` (char*) / ``PMIX_RANK`` (pmix_rank_t) |mdash| identify a specific
  process by namespace and rank. These must be used together. Combining
  ``PMIX_PROCID`` with either ``PMIX_NSPACE`` or ``PMIX_RANK`` in the same query
  yields ``PMIX_ERR_BAD_PARAM``.
* ``PMIX_CLIENT_ATTRIBUTES`` / ``PMIX_SERVER_ATTRIBUTES`` /
  ``PMIX_HOST_ATTRIBUTES`` / ``PMIX_TOOL_ATTRIBUTES`` (bool) |mdash| when used with
  ``PMIX_QUERY_ATTRIBUTE_SUPPORT``, select the level(s) of attribute support to
  report. Omitting all levels is equivalent to requesting every level.
* ``PMIX_QUERY_LOCAL_ONLY`` (bool) |mdash| constrain the query to locally available
  information only, rather than forwarding it to the host environment.
* ``PMIX_QUERY_REPORT_AVG`` (bool) |mdash| report average values.
* ``PMIX_QUERY_REPORT_MINMAX`` (bool) |mdash| report minimum and maximum values.
* ``PMIX_QUERY_SUPPORTED_QUALIFIERS`` (bool) |mdash| return a comma-delimited list of
  the qualifiers supported for the provided key, instead of performing the query on
  that key.
* ``PMIX_STORAGE_ID`` (char*) |mdash| identify the storage system whose information
  is being requested (used with the storage keys above).
* ``PMIX_STORAGE_TYPE`` (char*) |mdash| restrict a storage query to systems of the
  given type (e.g., ``lustre``, ``gpfs``, ``fabric-attached``).
* ``PMIX_STORAGE_ACCESS_TYPE`` (pmix_storage_access_type_t) |mdash| for the storage
  bandwidth, IOPS, and suggested-transfer-size keys, select which access type
  (read, write, or read/write) to report. See
  :ref:`pmix_storage_access_type_t(5) <man5-pmix_storage_access_type_t>`.

Qualifiers that are not applicable to a given key are ignored.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that all requested data was found
and returned in ``results``. For the non-blocking form, ``PMIX_SUCCESS`` indicates
only that the request was accepted for processing; the final status and results are
delivered to ``cbfunc``. In both cases the possible completion statuses include:

* ``PMIX_SUCCESS`` |mdash| all requested data was found and returned.
* ``PMIX_ERR_PARTIAL_SUCCESS`` |mdash| only some of the requested data was found;
  the result array contains an element for each key that returned a value.
* ``PMIX_ERR_NOT_FOUND`` |mdash| none of the requested data was available.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not support this
  operation.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, ``nqueries`` of zero, a ``NULL`` ``queries`` array, or a conflicting
  combination of process-identifier qualifiers.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized (and the
  query was not one of the locally resolvable ABI-version queries).

When a value other than ``PMIX_SUCCESS`` or ``PMIX_ERR_PARTIAL_SUCCESS`` is the
completion status, the result array is ``NULL`` and its count is zero. Any other
negative value indicates an appropriate error condition. PMIx error constants are
defined in ``pmix_common.h``.


NOTES
-----

The returned ``results`` array (blocking form) or the array delivered to ``cbfunc``
(non-blocking form) is owned by the caller and must be released with
``PMIX_INFO_FREE`` when no longer needed.

Because a query can carry multiple keys and qualifiers, it is well suited to
retrieving a multi-attribute block of data in a single request |mdash| something
the single-key :ref:`PMIx_Get(3) <man3-PMIx_Get>` API cannot do. For a single
static key value, however, ``PMIx_Get`` is typically faster because it avoids the
overhead of constructing and processing the ``pmix_query_t`` structure.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Resolve_peers(3) <man3-PMIx_Resolve_peers>`,
   :ref:`PMIx_Resolve_nodes(3) <man3-PMIx_Resolve_nodes>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`
