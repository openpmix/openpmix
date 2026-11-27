.. _man3-PMIx_Get:

PMIx_Get
========

.. include_body

``PMIx_Get``, ``PMIx_Get_nb`` |mdash| Retrieve a value posted by a process against
a specified key.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Get(const pmix_proc_t *proc, const char key[],
                          const pmix_info_t info[], size_t ninfo,
                          pmix_value_t **val);

   pmix_status_t PMIx_Get_nb(const pmix_proc_t *proc, const char key[],
                             const pmix_info_t info[], size_t ninfo,
                             pmix_value_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the proc is a Python ``pmix_proc_t`` dictionary
  proc = {'nspace': "testnspace", 'rank': 0}
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TIMEOUT,
             'value': 10, 'val_type': PMIX_INT}]
  rc, value = foo.get(proc, "mykey", pydirs)
  # the Python form always answers a copy, so PMIX_GET_POINTER_VALUES
  # is refused with PMIX_ERR_NOT_SUPPORTED by both get() and get_nb()

  # the non-blocking form returns as soon as the request has been accepted
  # and reports the result by executing a callback on the PMIx progress
  # thread. The callback is run if and only if the call returned
  # PMIX_SUCCESS, and must not itself make a blocking PMIx call.
  def valuecb(status, value, cbdata):
      # value is a Python ``pmix_value_t`` dictionary, or None if the key
      # was not found
      print("get completed:", foo.error_string(status), value)
  rc = foo.get_nb(proc, "mykey", pydirs, valuecb, "mycbdata")


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a ``pmix_proc_t`` identifying the process whose posted data
  is to be retrieved. A rank of ``PMIX_RANK_WILDCARD`` retrieves job-level
  information for the process's namespace, and never data a process posted. A
  rank of ``PMIX_RANK_UNDEF`` retrieves a key that any process of the namespace
  may have posted (see `KEYS THAT NAME NO PROCESS`_). A ``NULL`` value is the
  same as the caller's own namespace with ``PMIX_RANK_UNDEF``. An empty
  namespace is the caller's own.
* ``key``: A NULL-terminated string, no longer than ``PMIX_MAX_KEYLEN``
  characters, naming the value to retrieve. A ``NULL`` key requests **all** data
  associated with the identified process.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``val`` (blocking form): Address of a ``pmix_value_t`` pointer. On success it is
  set as described under `DESCRIPTION`_. When the ``PMIX_GET_STATIC_VALUES``
  directive is used, the caller must instead provide storage and pass a pointer to
  it here as an input.

The non-blocking form replaces ``val`` with a callback:

* ``cbfunc``: Callback function of type :ref:`pmix_value_cbfunc_t <man5-pmix_value_cbfunc_t>` invoked with the
  retrieved value once it is available.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Retrieve information for the specified ``key`` as posted by the process identified
in ``proc``. ``PMIx_Get`` is the blocking form: it does not return until the data
is available (or the operation fails or times out). ``PMIx_Get_nb`` is the
non-blocking form: it returns immediately, and the provided ``cbfunc`` is invoked
with the final status and value once the data has been retrieved by the local
server.

The precedence rules governing where the library looks for the data |mdash| the
local client cache, the local PMIx server, and the host resource manager |mdash|
differ for reserved keys (those provided by the host environment, beginning with
``"pmix"``) and non-reserved keys (those posted by the application via
:ref:`PMIx_Put(3) <man3-PMIx_Put>`). Those rules are governed by the directives
described below.

Return of the value is controlled by the directives:

* In the absence of any directive, the returned ``pmix_value_t`` is a freshly
  allocated memory object. The caller is responsible for releasing it with
  ``PMIX_VALUE_RELEASE`` when done.
* With ``PMIX_GET_POINTER_VALUES``, the function returns a pointer directly into
  the PMIx library's key-value store. The caller **must not** release the returned
  data.
* With ``PMIX_GET_STATIC_VALUES``, the value is returned in caller-provided
  storage: the caller must pass a pointer to a ``pmix_value_t`` it owns in ``val``
  and destruct it with ``PMIX_VALUE_DESTRUCT`` when done. If the implementation
  cannot return a static value, ``PMIx_Get`` returns ``PMIX_ERR_NOT_SUPPORTED``.

The same rules govern the value delivered to ``PMIx_Get_nb``'s ``cbfunc``, and in
particular the first of them: **the value passed to the callback belongs to the
caller**, and a callback that simply drops it leaks one ``pmix_value_t`` per get.
The library hands the value over and does not free it. The exception is the same
one |mdash| with ``PMIX_GET_POINTER_VALUES`` the callback receives a pointer into
the library's own store and must not release it. ``PMIX_GET_STATIC_VALUES`` cannot
be used with the non-blocking form, since there is no ``val`` argument through
which to pass the storage.

As with all non-blocking PMIx APIs, callers of ``PMIx_Get_nb`` **must** keep the
``proc`` and ``info`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The following attributes are relevant to this operation. All but ``PMIX_TIMEOUT``
are required to be supported by all PMIx libraries; ``PMIX_TIMEOUT`` is optional
for host environments.

* ``PMIX_OPTIONAL`` (bool) |mdash| look only in the client's local data store; do
  not request the data from the PMIx server if it is not found locally.
* ``PMIX_IMMEDIATE`` (bool) |mdash| direct the PMIx server to immediately return an
  error if the requested data is not in its store, rather than requesting it from
  the host resource manager.
* ``PMIX_GET_REFRESH_CACHE`` (bool) |mdash| refresh the local cache for the target
  process from the server before returning, picking up values put and committed
  since the last refresh. A ``NULL`` key refreshes all values for the process; a
  rank of ``PMIX_RANK_WILDCARD`` refreshes job-related information.
* ``PMIX_DATA_SCOPE`` (pmix_scope_t) |mdash| restrict the search to a target on
  the caller's node (``PMIX_LOCAL``), to a target on another node
  (``PMIX_REMOTE``), or to either (``PMIX_GLOBAL``). See `DATA SCOPE`_ below.
* ``PMIX_GET_POINTER_VALUES`` (bool) |mdash| return a pointer to the value in the
  library's key-value store instead of a copy; the caller must not free it.
* ``PMIX_GET_STATIC_VALUES`` (bool) |mdash| return the value in storage provided by
  the caller via the ``val`` parameter.
* ``PMIX_SESSION_INFO`` / ``PMIX_JOB_INFO`` / ``PMIX_APP_INFO`` /
  ``PMIX_NODE_INFO`` (bool) |mdash| direct that the requested key be retrieved from
  the session-, job-, application-, or node-level information, respectively.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the get to complete
  before returning an error. Helps avoid "hangs" when the target process never
  exposes the requested data.


DATA SCOPE
----------

What a caller may see of the data another process posted depends on where that
process is relative to the caller. This follows from the scopes defined in
:ref:`PMIx_Put(3) <man3-PMIx_Put>`:

* A target on the **same node** as the caller shows the data it posted at
  ``PMIX_LOCAL`` and ``PMIX_GLOBAL`` scope. Its ``PMIX_REMOTE`` data is not
  shared with processes on its own node, and is never returned to them.
* A target on **another node** shows the data it posted at ``PMIX_REMOTE`` and
  ``PMIX_GLOBAL`` scope. Its ``PMIX_LOCAL`` data never leaves its node.

A process may post the same key at both ``PMIX_LOCAL`` and ``PMIX_REMOTE`` scope
with different values |mdash| for example, a full transport address for peers on
its node and a network-only one for everyone else. A ``PMIx_Get`` of that key
returns the ``PMIX_LOCAL`` value to a caller on the target's node and the
``PMIX_REMOTE`` value to a caller on any other node. It never returns both.

A ``PMIX_DATA_SCOPE`` directive names where the target is, not which of its
values to read:

* ``PMIX_LOCAL`` |mdash| the target is on the caller's node.
* ``PMIX_REMOTE`` |mdash| the target is on another node.
* ``PMIX_GLOBAL`` |mdash| the target may be on either. This is the same as
  giving no scope.

A scope that names the location the target is not at finds nothing, including
data the target posted at ``PMIX_GLOBAL`` scope. ``PMIX_REMOTE`` for a target on
the caller's node, or ``PMIX_LOCAL`` for a target on another node, therefore
returns ``PMIX_ERR_NOT_FOUND``. So does a key the caller is not allowed to see,
such as a same-node target's ``PMIX_REMOTE``-only key. The library returns these
at once rather than waiting for data that will never become visible.

Two processes share a node when the host environment gave them the same
``PMIX_NODEID``. When the target's node is not known, these rules are not
applied. The rules apply to data a process posted with ``PMIx_Put``. They do
not apply to the reserved keys the host environment provides, which are the
same for every caller, nor to a caller reading its own data.


KEYS THAT NAME NO PROCESS
-------------------------

A key that is unique across a namespace can be read without knowing which
process posted it, as with the key-value space of PMI-1 and PMI-2. Pass a
``NULL`` ``proc``, or the namespace with ``PMIX_RANK_UNDEF``. The library
searches every process of the namespace, applying the rules above to each
one, and returns the copy from the lowest-numbered rank the caller may see.
Job-level data under the key is also found.

A key meant for every reader should therefore be posted at ``PMIX_GLOBAL``
scope. One posted at ``PMIX_LOCAL`` is found only on the poster's node, and one
posted at ``PMIX_REMOTE`` only on other nodes.

A ``PMIX_DATA_SCOPE`` directive narrows the search: ``PMIX_LOCAL`` searches
only the processes on the caller's node, and ``PMIX_REMOTE`` only those on
other nodes. If no process has posted the key yet, the request waits for it,
subject to ``PMIX_TIMEOUT``, unless ``PMIX_OPTIONAL`` or ``PMIX_IMMEDIATE`` is
given.

``PMIX_RANK_WILDCARD`` is not a substitute: it reads job-level information
only, and a key posted with ``PMIx_Put`` is never found that way.


RESERVED KEYS
-------------

In addition to keys posted by applications via
:ref:`PMIx_Put(3) <man3-PMIx_Put>`, the host environment provides a range of
reserved keys (those beginning with ``"pmix"``) that describe the process,
its job, and the resources it occupies. These are passed as the ``key``
parameter and, unless otherwise noted, are retrieved for the process
identified in ``proc``. A rank of ``PMIX_RANK_WILDCARD`` retrieves the
job-level value for the process's namespace. The following are commonly
available:

**Process identity and rank**

* ``PMIX_NSPACE`` (char*) |mdash| namespace of the process's job.
* ``PMIX_JOBID`` (char*) |mdash| job identifier assigned by the scheduler.
* ``PMIX_PROCID`` (pmix_proc_t\*) |mdash| process identifier (namespace and
  rank). Self-referential: see below.
* ``PMIX_RANK`` (pmix_rank_t) |mdash| rank of the process within its job.
  Self-referential: see below.
* ``PMIX_GLOBAL_RANK`` (pmix_rank_t) |mdash| rank of the process spanning across
  all jobs in this session.
* ``PMIX_APP_RANK`` (pmix_rank_t) |mdash| rank of the process within its
  application.
* ``PMIX_LOCAL_RANK`` (uint16_t) |mdash| rank of the process on its node within
  its job.
* ``PMIX_NODE_RANK`` (uint16_t) |mdash| rank of the process on its node spanning
  all jobs.
* ``PMIX_PACKAGE_RANK`` (uint16_t) |mdash| rank of the process within its job on
  the package where it resides.
* ``PMIX_NPROC_OFFSET`` (pmix_rank_t) |mdash| starting global rank of this job.
* ``PMIX_LOCALLDR`` (pmix_rank_t) |mdash| lowest rank on this node within this
  job.
* ``PMIX_APPLDR`` (pmix_rank_t) |mdash| lowest rank in this application within
  this job.
* ``PMIX_APPNUM`` (uint32_t) |mdash| application number within the job in which
  the process is included.
* ``PMIX_SESSION_ID`` (uint32_t) |mdash| session identifier.
* ``PMIX_PROC_PID`` (pid_t) |mdash| operating-system process identifier (pid) of
  the specified process.

.. note::

   ``PMIX_RANK`` and ``PMIX_PROCID`` are *self-referential* |mdash| they name
   the very thing the ``proc`` parameter must already supply, so asking for
   them about a process identified in ``proc`` conveys no information and is
   not supported. Each is instead retrieved by leaving the corresponding part
   of ``proc`` unspecified, which asks "who am I?":

   * ``PMIX_RANK`` |mdash| pass a ``proc`` whose namespace is the caller's own
     and whose rank is ``PMIX_RANK_INVALID``. The caller's own rank is
     returned, as a ``PMIX_PROC_RANK`` value.
   * ``PMIX_PROCID`` |mdash| pass ``NULL`` for ``proc``. The caller's own
     process identifier is returned.

   Passing a fully specified ``proc`` with either key returns
   ``PMIX_ERR_NOT_FOUND``. Note in particular that ``PMIX_RANK`` is not
   retrievable for *another* process: it serves as the identifier of the
   process-realm information supplied by the host (see
   ``PMIX_PROC_INFO_ARRAY``) rather than as a stored value. A process's rank
   is already known to any caller able to name it. The remaining rank keys in
   this section |mdash| ``PMIX_GLOBAL_RANK``, ``PMIX_APP_RANK``,
   ``PMIX_LOCAL_RANK``, ``PMIX_NODE_RANK`` and ``PMIX_PACKAGE_RANK`` |mdash|
   are ordinary stored values and are retrieved for the process identified in
   ``proc`` in the usual way.

   In the same vein, ``PMIX_VERSION_NUMERIC`` reports the version of the
   library the caller is linked against and ignores ``proc`` entirely.

**Node and host information**

* ``PMIX_HOSTNAME`` (char*) |mdash| name of the host on which the specified
  process is located.
* ``PMIX_HOSTNAME_ALIASES`` (char*) |mdash| comma-delimited list of names by
  which the node is known.
* ``PMIX_HOSTNAME_KEEP_FQDN`` (bool) |mdash| indicates that fully-qualified
  domain-name hostnames are being retained.
* ``PMIX_NODEID`` (uint32_t) |mdash| node identifier where the specified process
  is located.
* ``PMIX_CLUSTER_ID`` (char*) |mdash| string name of the cluster on which the
  process is executing.
* ``PMIX_NODE_LIST`` (char*) |mdash| comma-delimited list of nodes running
  processes for the specified namespace.
* ``PMIX_ALLOCATED_NODELIST`` (char*) |mdash| comma-delimited list of all nodes
  in this allocation, regardless of whether or not they currently host
  processes.
* ``PMIX_LOCAL_PEERS`` (char*) |mdash| comma-delimited string of ranks on this
  node within the specified namespace.
* ``PMIX_LOCAL_PROCS`` (pmix_data_array_t\*) |mdash| array of
  :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structures identifying the processes
  on the specified node.
* ``PMIX_LOCAL_CPUSETS`` (char*) |mdash| colon-delimited cpusets of the local
  peers within the specified namespace.

**Sizes and counts**

* ``PMIX_UNIV_SIZE`` (uint32_t) |mdash| number of slots in this session.
* ``PMIX_JOB_SIZE`` (uint32_t) |mdash| number of processes in this job.
* ``PMIX_JOB_NUM_APPS`` (uint32_t) |mdash| number of applications in this job.
* ``PMIX_APP_SIZE`` (uint32_t) |mdash| number of processes in the application.
* ``PMIX_LOCAL_SIZE`` (uint32_t) |mdash| number of processes in this job on the
  local node.
* ``PMIX_NODE_SIZE`` (uint32_t) |mdash| number of processes across all jobs on
  this node.
* ``PMIX_MAX_PROCS`` (uint32_t) |mdash| maximum number of processes for this
  job.
* ``PMIX_NUM_SLOTS`` (uint32_t) |mdash| number of slots allocated.
* ``PMIX_NUM_NODES`` (uint32_t) |mdash| number of nodes currently hosting
  processes in the specified realm.
* ``PMIX_NUM_ALLOCATED_NODES`` (uint32_t) |mdash| number of nodes in the
  specified realm, regardless of whether or not they currently host processes.

**Scratch directories**

* ``PMIX_TMPDIR`` (char*) |mdash| top-level temporary directory assigned to the
  session.
* ``PMIX_NSDIR`` (char*) |mdash| sub-directory of the session temporary
  directory assigned to the namespace.
* ``PMIX_PROCDIR`` (char*) |mdash| sub-directory of the namespace directory
  assigned to the process.

**Process state and launch**

* ``PMIX_SPAWNED`` (bool) |mdash| true if this process resulted from a call to
  :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`.
* ``PMIX_PARENT_ID`` (pmix_proc_t\*) |mdash| identifier of the process that
  called :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>` to launch this process's
  application.
* ``PMIX_EXIT_CODE`` (int) |mdash| exit code returned when the specified process
  terminated.
* ``PMIX_REINCARNATION`` (uint32_t) |mdash| number of times this process has
  been instantiated |mdash| i.e., a count of the number of times it has been
  restarted.
* ``PMIX_NODE_OVERSUBSCRIBED`` (bool) |mdash| true if the number of processes
  from this job on this node exceeds the number of slots allocated to it.
* ``PMIX_CPUSET_BITMAP`` (pmix_cpuset_t\*) |mdash| bitmap applied to the process
  at launch.
* ``PMIX_CREDENTIAL`` (char*) |mdash| security credential assigned to the
  process.

**Process set membership**

* ``PMIX_PSET_NAMES`` (pmix_data_array_t\*) |mdash| array of the string names of
  the process sets in which the specified process is a member.

**Fabric locality**

* ``PMIX_SWITCH_PEERS`` (pmix_data_array_t\*) |mdash| the peer ranks that share a
  switch with the specified process. Returns an array of ``pmix_info_t``, one per
  local fabric device; each element is itself a ``PMIX_SWITCH_PEERS`` entry
  holding a three-element array that gives the device's
  ``PMIX_DEVICE_ID``, the ``PMIX_FABRIC_SWITCH`` to which it is connected,
  and a comma-delimited string of the peer ranks sharing that switch.

**Device assignment**

* ``PMIX_DEVICE_ID`` (pmix_data_array_t\*) |mdash| the device(s) the host
  assigned to the specified process, e.g., the devices it was mapped against.
  The value is an array of :ref:`pmix_device_t(5) <man5-pmix_device_t>`, and is
  an array even when it holds a single device; each element gives the
  device's UUID (e.g., ``gpu://node01::cuda0``; every form is listed in
  :ref:`pmix_device_t(5) <man5-pmix_device_t>`), OS name and type. The key is
  absent when the process was assigned no device. This form is particular to
  a process's own information: everywhere else |mdash| as a directive to
  :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`, or inside a
  ``PMIX_FABRIC_DEVICE`` or ``PMIX_SWITCH_PEERS`` array |mdash|
  ``PMIX_DEVICE_ID`` is a ``char*`` naming a single device. To ask how far
  the process is from an assigned device, pass that element's ``uuid`` to
  :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>` as a
  ``PMIX_DEVICE_ID``.

  .. code-block:: c

     pmix_value_t *val = NULL;
     pmix_device_t *devs;
     size_t n;

     if (PMIX_SUCCESS == PMIx_Get(&myproc, PMIX_DEVICE_ID, NULL, 0, &val)) {
         if (PMIX_DATA_ARRAY == val->type &&
             PMIX_DEVICE == val->data.darray->type) {
             devs = (pmix_device_t *) val->data.darray->array;
             for (n = 0; n < val->data.darray->size; n++) {
                 printf("assigned %s (%s)\n", devs[n].osname, devs[n].uuid);
             }
         }
         PMIX_VALUE_RELEASE(val);
     }


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the requested data was
returned in the manner requested. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing; the
final status and value are delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the requested data has been returned.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the requested data was not available. This
  includes a key that *was* available earlier and has since been deleted
  |mdash| see `NOTES`_.
* ``PMIX_ERR_EXISTS_OUTSIDE_SCOPE`` |mdash| the requested key exists, but was
  posted in a scope that does not include the requester. A key another process
  posted with :ref:`PMIx_Put(3) <man3-PMIx_Put>` that the caller may not see is
  reported as ``PMIX_ERR_NOT_FOUND`` instead (see `DATA SCOPE`_).
* ``PMIX_ERR_NO_PERMISSIONS`` |mdash| the caller's user may not access the job
  the data belongs to. See :doc:`/hosts/access-control`.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, an over-length key, or the ``PMIX_GET_STATIC_VALUES`` directive with a
  ``NULL`` storage location.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the implementation cannot satisfy a requested
  directive (e.g., ``PMIX_GET_STATIC_VALUES``).
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

For non-reserved keys, retrieval blocks until the target process has posted the
key with :ref:`PMIx_Put(3) <man3-PMIx_Put>`, committed it with
:ref:`PMIx_Commit(3) <man3-PMIx_Commit>`, and the data has been circulated |mdash|
typically via a :ref:`PMIx_Fence(3) <man3-PMIx_Fence>` |mdash| unless the
``PMIX_OPTIONAL`` or ``PMIX_IMMEDIATE`` directives are used to bound the search.

**Qualified values.** When a key was posted together with qualifiers (using the
``PMIX_QUALIFIED_VALUE`` key to :ref:`PMIx_Put(3) <man3-PMIx_Put>`), supply the
desired qualifiers in the ``info`` array to select among the values stored under
that key. ``PMIx_Get`` returns the primary value of the matching entry; the
qualifiers themselves are used only for matching and are not returned.

**A key that was there can go away.** Published data used only to
accumulate, so a key that was once retrievable stayed retrievable. That is no
longer true: a process can retract one of its own keys with the ``PMIX_DEL_*``
scopes of :ref:`PMIx_Put(3) <man3-PMIx_Put>`, and a host environment can
retract job-level information it registered with
:ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`.
A call that succeeded for a key can therefore return ``PMIX_ERR_NOT_FOUND``
later in the same job, and callers that cannot tolerate that must be prepared
for it.

The removal reaches this process as a notification from its local PMIx server,
which drops the cached copy the caller was given. For a key belonging to a
process on another node it arrives with the collecting
:ref:`PMIx_Fence(3) <man3-PMIx_Fence>` that carries the removal. The
notification is one-way and unacknowledged, so it arrives promptly rather than
synchronously: a ``PMIx_Get`` that races it can return the old value once more.
Once it has landed, a request for the key behaves exactly like one for a key
that was never published |mdash| for a non-reserved key that means the call
blocks until someone publishes it, unless ``PMIX_OPTIONAL`` or
``PMIX_IMMEDIATE`` bounds the search.

**Values from another process are cached, and a later exchange does not
refresh them.** Once a process's data has been retrieved, subsequent calls for
that process are answered from the local cache without consulting the server.
So if the target process posts a *new* value under a key it had already
published, and the data is circulated again, this call keeps returning the
value read the first time. That is deliberate |mdash| it is what makes a
lookup after a full exchange cost nothing |mdash| but it means a caller that
expects an updated value must ask for one with ``PMIX_GET_REFRESH_CACHE``:

.. code-block:: c

   pmix_info_t info;
   bool refresh = true;

   PMIX_INFO_LOAD(&info, PMIX_GET_REFRESH_CACHE, &refresh, PMIX_BOOL);
   rc = PMIx_Get(&proc, "mykey", &info, 1, &val);

Note that a refresh for a non-reserved key brings across everything the target
process has published, not only the key named in the call, so one refresh
updates the caller's whole view of that process.


.. include:: /man/no-blocking-in-progress-thread.rst


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :doc:`Modex: Exchanging Process Data </how-things-work/modex>`
