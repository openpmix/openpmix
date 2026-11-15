.. _man3-PMIx_Session_control:

PMIx_Session_control
====================

.. include_body

``PMIx_Session_control`` |mdash| Request a control action be applied to a
session.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Session_control(uint32_t sessionID,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  # session control is bound on PMIxServer and inherited by PMIxTool and
  # PMIxScheduler
  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_SESSION_APP,
             'value': "myapp", 'val_type': PMIX_STRING}]
  rc = foo.session_control(1, pydirs)


INPUT PARAMETERS
----------------

* ``sessionID``: A ``uint32_t`` identifying the session to which the control
  action is to be applied. A value of ``UINT32_MAX`` indicates that the action
  applies to all sessions under the caller's control.
* ``directives``: Pointer to an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures describing the control
  action(s) to be taken (see `DIRECTIVES`_). A ``NULL`` value is supported when
  no directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type :ref:`pmix_info_cbfunc_t <man5-pmix_info_cbfunc_t>`. If ``NULL``, the
  call is treated as a blocking operation; otherwise it is non-blocking and the
  callback is invoked once the request has been processed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request that a control action be applied to the session identified by
``sessionID``. The specific action(s) to be taken are conveyed as attributes in
the ``directives`` array.

``PMIx_Session_control`` supports both blocking and non-blocking operation
through a single entry point, selected by the ``cbfunc`` argument:

* If ``cbfunc`` is ``NULL``, the call is **blocking**: it does not return until
  the request has been processed, and the return status reflects the overall
  outcome of the operation.
* If ``cbfunc`` is non-``NULL``, the call is **non-blocking**: it returns
  immediately, and ``cbfunc`` is invoked with the final status and any returned
  information once the request has been processed. The callback reports whether
  the request was granted and may supply additional information |mdash| for
  example, the reason for any denial |mdash| in its array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures.

As with all non-blocking PMIx APIs, when a ``cbfunc`` is supplied the caller
**must** keep the ``directives`` array valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The requested action is expressed entirely through the ``directives`` array.
The PMIx library is not required to interpret these directives itself |mdash|
it passes them to the host environment (typically the scheduler) for
processing. Actual support for any given directive depends on the host
environment.

Identifying the request:

* ``PMIX_SESSION_CTRL_ID`` (char*) |mdash| a string identifier for this request,
  allowing its status to later be queried or the operation to be cancelled.
* ``PMIX_REQUESTOR`` (pmix_proc_t\*) |mdash| identifier of the process on whose
  behalf the request is being made, used when one process relays the operation
  to the PMIx library on behalf of another (the API itself takes no requestor
  parameter).

Instantiating a session (typically issued by a scheduler):

* ``PMIX_SESSION_INSTANTIATE`` (bool) |mdash| instantiate the specified session.
* ``PMIX_ALLOC_UID`` (uint32_t) |mdash| the user id the session belongs to.
* ``PMIX_ALLOC_GID`` (uint32_t) |mdash| the group id the session belongs to.
  A scheduler instantiates a session on behalf of a user, and these say which
  one. ``PMIX_USERID`` and ``PMIX_GRPID`` cannot: the library sets them to the
  calling process's own user and group, which are the scheduler's.
* ``PMIX_SESSION_RESOURCES`` (pmix_data_array_t*) |mdash| array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures describing the resources
  to be included in the session (e.g., a regular expression of node names, an
  array of resource blocks, etc.).
* ``PMIX_SESSION_APP`` (pmix_data_array_t*) |mdash| array of ``pmix_app_t``
  structures to be executed in the assigned session upon instantiation.
* ``PMIX_SESSION_JOB`` (pmix_data_array_t*) |mdash| array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures describing the job
  information to be applied in conjunction with the specified
  ``PMIX_SESSION_APP`` definitions. It is an error to provide this without a
  ``PMIX_SESSION_APP`` description.
* ``PMIX_SESSION_PROVISION`` (pmix_data_array_t*) |mdash| description of the
  nodes to be provisioned with a specified image.
* ``PMIX_SESSION_PROVISION_NODES`` (char*) |mdash| regular expression
  identifying nodes that are to be provisioned.
* ``PMIX_SESSION_PROVISION_IMAGE`` (char*) |mdash| name of the image that is to
  be provisioned.

Controlling a session's operation:

* ``PMIX_SESSION_PAUSE`` (bool) |mdash| pause all jobs in the specified session.
* ``PMIX_SESSION_RESUME`` (bool) |mdash| resume ("un-pause") all jobs in the
  specified session.
* ``PMIX_SESSION_PREEMPT`` (bool) |mdash| preempt the indicated jobs (named via
  a ``PMIX_NSPACE`` attribute in the accompanying array) in the specified
  session and recover all their resources. If no ``PMIX_NSPACE`` is given, all
  jobs in the session are preempted.
* ``PMIX_SESSION_RESTORE`` (bool) |mdash| restore the indicated jobs (named via
  a ``PMIX_NSPACE`` attribute in the accompanying array) in the specified
  session, including all their resources. If no ``PMIX_NSPACE`` is given, all
  jobs in the session are restored.
* ``PMIX_SESSION_SIGNAL`` (int) |mdash| send the given signal to all processes
  of every job in the session.
* ``PMIX_SESSION_EXTEND`` (bool) |mdash| extend the specified session, either in
  time or in resources, based on additional provided attributes.
* ``PMIX_SESSION_SEP`` (bool) |mdash| separate the specified session from its
  parent |mdash| i.e., allow the two to terminate independently.
* ``PMIX_SESSION_TERMINATE`` (bool) |mdash| terminate all jobs in the specified
  session and recover all resources included in the session.
* ``PMIX_SESSION_COMPLETE`` (bool) |mdash| indicate that the specified session
  has completed and all of its resources have been recovered and are available
  for scheduling. The accompanying array must include
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures reporting the returned
  status of any jobs executed in the session.


RETURN VALUE
------------

For the blocking form (``cbfunc == NULL``), a successful request returns
``PMIX_OPERATION_SUCCEEDED``. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status and any data are delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| (non-blocking form) the request was accepted for
  processing.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (blocking form) the request was processed
  successfully.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the operation is not supported in the
  caller's role or environment |mdash| for example, the caller is a system
  controller that is not connected to the scheduler.
* ``PMIX_ERR_UNREACH`` |mdash| the caller is not connected to a server capable
  of servicing the request.
* ``PMIX_ERR_COMM_FAILURE`` |mdash| the connection to the server was lost before
  a result could be returned.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Unlike most PMIx operations, ``PMIx_Session_control`` does not provide a
separate ``_nb`` entry point; the single API selects blocking versus
non-blocking behavior based on whether ``cbfunc`` is ``NULL``.

``PMIx_Session_control`` is a PMIx library extension and is not part of the
published PMIx Standard.


.. include:: /man/no-blocking-in-progress-thread.rst


SESSION CONTROL AND ALLOCATION REQUESTS
---------------------------------------

``PMIx_Session_control`` issues directives to a session. Most of them
|mdash| pause, resume, signal, terminate |mdash| act on a session that
already exists, and may come from the user the session belongs to as well as
from the scheduler.

``PMIX_SESSION_INSTANTIATE`` and ``PMIX_SESSION_EXTEND`` are different. They
do not ask for resources: a user asks for resources with
:ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`, and the
scheduler decides. These two directives are how the scheduler then tells the
runtime managing the session |mdash| a distributed virtual machine (DVM), for
example |mdash| what it decided: start a session over these resources for
this user, or give this session more time or more resources. A runtime would
typically accept them only from the scheduler (or, where there is none, from
whoever acts with the scheduler's authority) |mdash| not from the user the
session is for, who asks with an allocation request instead.

The scheduler's answer to an allocation request goes to the requester, and
may carry much the same information as the directive, but the directive is
still needed:

* **The runtime may never see the answer.** The host environment that
  passed the request on is not required to watch for the scheduler's reply,
  and the scheduler cannot know whether it will intercept the answer and
  act on it. A runtime that is told directly needs no such arrangement.
* **The runtime may not know a request was made.** A request can come from a
  tool connected to the scheduler, or to some other server, rather than from
  one of the runtime's own processes. The runtime then has no record of the
  request at all, and only a directive from the scheduler tells it what to
  do.
* **Not every change starts with a request.** A scheduler sometimes needs a
  runtime to change a session for its own reasons |mdash| for example, to
  modify a job's resources to keep the whole system within an overall energy
  limit. There is no allocation request to answer in that case; there is
  only the directive.


EXAMPLES
--------

A workflow tool, connected to the scheduler rather than to any runtime, asks
for four nodes for two hours on behalf of the user running it:

.. code-block:: c

    pmix_info_t info[2];
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    uint64_t nnodes = 4;
    pmix_status_t rc;

    PMIx_Info_load(&info[0], PMIX_ALLOC_NUM_NODES, &nnodes, PMIX_UINT64);
    PMIx_Info_load(&info[1], PMIX_ALLOC_TIME, "2:00:00", PMIX_STRING);
    rc = PMIx_Allocation_request(PMIX_ALLOC_NEW, info, 2, &results, &nresults);
    /* on success, results include the PMIX_ALLOC_ID of the new allocation */
    PMIx_Info_destruct(&info[0]);
    PMIx_Info_destruct(&info[1]);
    if (NULL != results) {
        PMIx_Info_free(results, nresults);
    }

The runtime that will host the work saw none of this. The scheduler chooses
the nodes, answers the tool with the allocation's identifier, and directs the
runtime to instantiate a session over them for the user who asked |mdash|
identified by the ``PMIX_USERID`` and ``PMIX_GRPID`` the library attached to
the tool's request (here, 1001):

.. code-block:: c

    pmix_info_t dirs[4], *res;
    pmix_data_array_t resources;
    uint32_t uid = 1001, gid = 1001;
    bool flag = true;
    pmix_status_t rc;
    size_t n;

    PMIx_Data_array_construct(&resources, 3, PMIX_INFO);
    res = (pmix_info_t *) resources.array;
    PMIx_Info_load(&res[0], PMIX_ALLOC_NODE_LIST,
                   "node01,node02,node03,node04", PMIX_STRING);
    PMIx_Info_load(&res[1], PMIX_ALLOC_ID, "alloc-57", PMIX_STRING);
    PMIx_Info_load(&res[2], PMIX_ALLOC_TIME, "2:00:00", PMIX_STRING);

    PMIx_Info_load(&dirs[0], PMIX_SESSION_INSTANTIATE, &flag, PMIX_BOOL);
    PMIx_Info_load(&dirs[1], PMIX_ALLOC_UID, &uid, PMIX_UINT32);
    PMIx_Info_load(&dirs[2], PMIX_ALLOC_GID, &gid, PMIX_UINT32);
    /* the info takes its own copy of the array */
    PMIx_Info_load(&dirs[3], PMIX_SESSION_RESOURCES, &resources, PMIX_DATA_ARRAY);
    rc = PMIx_Session_control(12, dirs, 4, NULL, NULL);
    /* PMIX_OPERATION_SUCCEEDED once the runtime has created session 12 */
    for (n = 0; n < 4; n++) {
        PMIx_Info_destruct(&dirs[n]);
    }
    PMIx_Data_array_destruct(&resources);

The tool can now refer to the allocation by its ``PMIX_ALLOC_ID`` |mdash|
for example, to spawn a job into it. See :ref:`PMIx_Allocation_request(3)
<man3-PMIx_Allocation_request>` for an extension requested by a process
already running in a session.

.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
