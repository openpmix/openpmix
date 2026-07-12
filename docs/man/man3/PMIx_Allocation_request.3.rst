.. _man3-PMIx_Allocation_request:

PMIx_Allocation_request
=======================

.. include_body

``PMIx_Allocation_request``, ``PMIx_Allocation_request_nb`` |mdash| Request an
allocation operation from the host resource manager or scheduler.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Allocation_request(pmix_alloc_directive_t directive,
                                         pmix_info_t *info, size_t ninfo,
                                         pmix_info_t **results, size_t *nresults);

   pmix_status_t PMIx_Allocation_request_nb(pmix_alloc_directive_t directive,
                                            pmix_info_t *info, size_t ninfo,
                                            pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pyinfo = [{'key': PMIX_ALLOC_NUM_NODES,
             'value': {'value': 4, 'val_type': PMIX_UINT64}}]
  rc, results = foo.allocation_request(PMIX_ALLOC_EXTEND, pyinfo)


INPUT PARAMETERS
----------------

* ``directive``: An allocation directive of type ``pmix_alloc_directive_t``
  identifying the requested operation (see `DIRECTIVES`_).
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures describing and qualifying the request (see `ATTRIBUTES`_). A
  ``NULL`` value is supported when no attributes are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``results`` (blocking form): Address where a pointer to an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures containing the results of
  the request is returned. Set to ``NULL`` when no data is returned. The caller
  is responsible for releasing the returned array with ``PMIX_INFO_FREE`` when
  done.
* ``nresults`` (blocking form): Address where the number of elements in the
  returned ``results`` array is stored.

The non-blocking form replaces ``results`` and ``nresults`` with a callback:

* ``cbfunc``: Callback function of type :ref:`pmix_info_cbfunc_t <man5-pmix_info_cbfunc_t>` invoked with the
  final status and any returned data once the request has been processed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request an allocation operation from the host resource manager or scheduler.
Several broad categories of operation are envisioned, including the ability to:

* Request allocation of additional resources, including memory, bandwidth, and
  compute. Note that the new allocation will be disjoint from (i.e., not
  affiliated with) the allocation of the requestor |mdash| thus the termination
  of one allocation will not impact the other.
* Extend the reservation on currently allocated resources, subject to
  scheduling availability and priorities. This includes extending the time
  limit on current resources and/or requesting that additional resources be
  allocated to the requesting job. Any additional allocated resources will be
  considered part of the current allocation, and thus will be released at the
  same time.
* Release currently allocated resources that are no longer required. This is
  intended to support partial release of resources, since all resources are
  normally released upon termination of the job.
* "Lend" resources back to the scheduler with an expectation of getting them
  back at some later time in the job, together with the corresponding ability
  to "reacquire" resources previously released.

``PMIx_Allocation_request`` is the blocking form: it does not return until the
operation is complete. If successful, results are returned in the ``results``
and ``nresults`` parameters. ``PMIx_Allocation_request_nb`` is the non-blocking
form: it returns immediately, and the provided ``cbfunc`` is invoked with the
final status and any returned data once the operation has been processed.

If the request is for additional resources and it succeeds, the returned
results include the host environment's allocation identifier
(``PMIX_ALLOC_ID``) that the requester can use to reference the resulting
resources in, for example, a later spawn request.

As with all non-blocking PMIx APIs, callers of
``PMIx_Allocation_request_nb`` **must** keep the ``info`` array valid until
``cbfunc`` is invoked.


DIRECTIVES
----------

The ``directive`` argument is a value of type ``pmix_alloc_directive_t`` that
identifies the requested operation:

* ``PMIX_ALLOC_NEW`` |mdash| a new allocation is being requested. The resulting
  allocation will be disjoint (i.e., not connected in a job sense) from the
  requesting allocation.
* ``PMIX_ALLOC_EXTEND`` |mdash| extend the existing allocation, either in time
  or by adding resources.
* ``PMIX_ALLOC_RELEASE`` |mdash| release part or all of the existing
  allocation. Attributes in the accompanying ``info`` array may be used to
  specify "lending" of those resources for some period of time.
* ``PMIX_ALLOC_REAQUIRE`` |mdash| reacquire resources that were previously
  "lent" back to the scheduler.
* ``PMIX_ALLOC_REQ_CANCEL`` |mdash| cancel the indicated allocation request.

Values at or above ``PMIX_ALLOC_EXTERNAL`` are reserved for
implementer-defined directives.


ATTRIBUTES
----------

PMIx libraries are not required to directly support any attributes for this
operation. However, any provided attributes must be passed to the host
environment for processing, and the PMIx library is required to add the
``PMIX_USERID`` and ``PMIX_GRPID`` attributes of the client process making the
request.

Host environments that implement support for this operation are required to
support the following attributes:

* ``PMIX_ALLOC_REQ_ID`` (char*) |mdash| user-provided string identifier for
  this allocation request, which can later be used to query its status.
* ``PMIX_ALLOC_NUM_NODES`` (uint64_t) |mdash| number of nodes being requested.
* ``PMIX_ALLOC_NUM_CPUS`` (uint64_t) |mdash| number of CPUs being requested.
* ``PMIX_ALLOC_TIME`` (char*) |mdash| time for which the resources are being
  requested, specified in the usual ``months:days:hours:minutes:seconds``
  format, scanned from right to left (i.e., a value of ``"2"`` equates to two
  seconds).

The following attributes are optional for host environments that support this
operation.

Identifying an existing allocation (for the ``PMIX_ALLOC_EXTEND``,
``PMIX_ALLOC_RELEASE``, ``PMIX_ALLOC_REAQUIRE``, and ``PMIX_ALLOC_REQ_CANCEL``
directives):

* ``PMIX_ALLOC_ID`` (char*) |mdash| the host-provided string identifier of the
  allocation to be operated upon (used both as a qualifier on input and, for a
  new-resource request, returned on output |mdash| see below).

Describing the resources being requested:

* ``PMIX_ALLOC_NODE_LIST`` (char*) |mdash| regular expression of the specific
  nodes being requested.
* ``PMIX_ALLOC_EXCLUDE`` (char*) |mdash| regular expression of nodes to exclude
  from scheduling consideration.
* ``PMIX_ALLOC_NUM_CPU_LIST`` (char*) |mdash| regular expression of the number
  of CPUs being requested on each node.
* ``PMIX_ALLOC_CPU_LIST`` (char*) |mdash| regular expression of the specific
  CPUs being requested on each node.
* ``PMIX_ALLOC_MEM_SIZE`` (float) |mdash| amount of memory being requested, in
  Mbytes.
* ``PMIX_ALLOC_RES_BLOCK`` (char*) |mdash| name of a previously-defined
  resource block to be allocated.
* ``PMIX_ALLOC_NUM_BLOCKS`` (uint64_t) |mdash| number of the specified resource
  blocks to allocate.
* ``PMIX_ALLOC_MAU`` (pmix_data_array_t*) |mdash| minimum allocatable unit for
  the system, expressed as an array of ``pmix_resource_unit_t`` structures.
* ``PMIX_ALLOC_IMAGE`` (char*) |mdash| name of the image that the requested
  nodes are to have on them.
* ``PMIX_MEM_ALLOC_KIND`` (char*) |mdash| comma-delimited list of memory kinds
  being allocated (e.g., ``system,mpi,rocm:device``).
* ``PMIX_GPU_SUPPORT`` (bool) |mdash| direct the application to enable (true)
  or disable (false) its internal library's GPU support.

Describing fabric resources being requested:

* ``PMIX_ALLOC_FABRIC`` (pmix_data_array_t*) |mdash| array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures describing a fabric
  resource request. It must include at least ``PMIX_ALLOC_FABRIC_ID``,
  ``PMIX_ALLOC_FABRIC_TYPE``, and ``PMIX_ALLOC_FABRIC_ENDPTS``, plus any other
  desired descriptors.
* ``PMIX_ALLOC_FABRIC_ID`` (char*) |mdash| key to be used when accessing this
  requested fabric allocation.
* ``PMIX_ALLOC_FABRIC_TYPE`` (char*) |mdash| type of desired transport (e.g.,
  ``tcp``, ``udp``).
* ``PMIX_ALLOC_FABRIC_PLANE`` (char*) |mdash| identifier string for the NIC
  (fabric plane) to be used for this allocation.
* ``PMIX_ALLOC_FABRIC_ENDPTS`` (size_t) |mdash| number of endpoints to allocate
  per process.
* ``PMIX_ALLOC_FABRIC_ENDPTS_NODE`` (size_t) |mdash| number of endpoints to
  allocate per node.
* ``PMIX_ALLOC_FABRIC_QOS`` (char*) |mdash| quality-of-service level being
  requested.
* ``PMIX_ALLOC_BANDWIDTH`` (float) |mdash| bandwidth being requested, in
  Mbits/sec.
* ``PMIX_ALLOC_FABRIC_SEC_KEY`` (pmix_byte_object_t) |mdash| fabric security
  key.

Controlling scheduling, timing, and lifecycle:

* ``PMIX_ALLOC_QUEUE`` (char*) |mdash| name of the queue being referenced.
* ``PMIX_ALLOC_PREEMPTIBLE`` (bool) |mdash| whether the jobs in the resulting
  allocation are to be considered preemptible (overridable at the per-job
  level).
* ``PMIX_ALLOC_BEGIN`` (char*) |mdash| direct the scheduler to defer the
  allocation until the specified time (see the attribute definition in
  ``pmix_common.h`` for the accepted time formats).
* ``PMIX_ALLOC_DEPENDENCY`` (char*) |mdash| defer the start of the allocation
  until the specified dependencies have successfully completed.
* ``PMIX_ALLOC_WAIT_ALL_NODES`` (bool) |mdash| whether to wait for all nodes to
  be scheduled before beginning to initialize and release nodes for use.
* ``PMIX_ALLOC_WARN_TIMEOUT`` (uint32_t) |mdash| request that the scheduler
  provide advance warning of the impending expiration of the allocation. The
  value is the number of seconds prior to expiration at which the warning is to
  be delivered via a ``PMIX_ALLOC_TIMEOUT_WARNING`` event.
* ``PMIX_ALLOC_LEND`` (char*) |mdash| estimated time before the lent resources
  shall be available for return, in the same time format as ``PMIX_ALLOC_TIME``
  (used with the ``PMIX_ALLOC_RELEASE`` directive).
* ``PMIX_ALLOC_RELEASABLE`` (bool) |mdash| true if the entire allocation may be
  released.
* ``PMIX_ALLOC_NOSHELL`` (bool) |mdash| immediately exit after allocating
  resources, without running a command.
* ``PMIX_ALLOC_NOT_WAITING`` (bool) |mdash| the requestor is not waiting for
  the allocation to be issued; it will discover the allocation by monitoring
  for the "allocated" event or by querying the system.
* ``PMIX_ALLOC_PROPERTY`` (char*) |mdash| name of an allocation property.

Controlling ownership, sharing, and inheritance:

* ``PMIX_ALLOC_TARGET`` (char*) |mdash| namespace to which the allocated
  resources are to be assigned; the host reserves the resources to members of
  that namespace.
* ``PMIX_ALLOC_SHARE`` (bool) |mdash| if true, make the allocated resources
  generally available within the requestor's session rather than reserving them
  solely for the requestor's namespace (false by default).
* ``PMIX_ALLOC_CHILD_SEP`` (bool) |mdash| treat the resulting allocation as
  independent from its parent |mdash| i.e., do not terminate the allocation
  upon termination of the parent.
* ``PMIX_ALLOC_INHERITANCE`` (pmix_alloc_inheritance_t) |mdash| inheritance
  rules to be applied to the allocated resources. If not provided, defaults to
  ``PMIX_ALLOC_INHERIT_DEFAULT``.

On successful completion of a request for additional resources, the returned
data includes ``PMIX_ALLOC_ID`` (char*), a host-provided string identifier for
the resulting allocation.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the request was
processed and any results were returned in ``results`` and ``nresults``. For
the non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the
request was accepted for processing; the final status and any data are
delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the request was successfully processed.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the operation is not supported in the
  caller's role or environment |mdash| for example, the caller is itself the
  scheduler, or is a system controller with no scheduler attached.
* ``PMIX_ERR_UNREACH`` |mdash| the caller is not connected to a server capable
  of servicing the request.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This operation is only meaningful when the caller can reach a scheduler, either
because the caller's server is the scheduler or because the caller is a server
whose host environment provides an allocation entry point that can forward the
request. A process hosted directly by the scheduler cannot issue an allocation
request against itself and will receive ``PMIX_ERR_NOT_SUPPORTED``.

Upon completion of an allocation request, the host environment may generate an
event to notify processes other than the requestor |mdash| for example, the
members of a job affected by the change |mdash| of the result. The result is
conveyed in that event through the ``PMIX_ALLOC_STATUS`` (pmix_status_t)
attribute, which carries the completion status of the allocation request.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`PMIx_Session_control(3) <man3-PMIx_Session_control>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_alloc_directive_t(5) <man5-pmix_alloc_directive_t>`
