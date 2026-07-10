.. _man3-PMIx_Job_control:

PMIx_Job_control
================

.. include_body

``PMIx_Job_control``, ``PMIx_Job_control_nb`` |mdash| Request a job control
action be applied to a set of target processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Job_control(const pmix_proc_t targets[], size_t ntargets,
                                  const pmix_info_t directives[], size_t ndirs,
                                  pmix_info_t **results, size_t *nresults);

   pmix_status_t PMIx_Job_control_nb(const pmix_proc_t targets[], size_t ntargets,
                                     const pmix_info_t directives[], size_t ndirs,
                                     pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the targets is a list of Python ``pmix_proc_t`` dictionaries
  targets = [{'nspace': "testnspace", 'rank': PMIX_RANK_WILDCARD}]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_JOB_CTRL_KILL,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc, results = foo.job_control(targets, pydirs)


INPUT PARAMETERS
----------------

* ``targets``: Pointer to an array of ``pmix_proc_t`` structures identifying the
  processes to which the requested job control action is to be applied. A
  ``NULL`` value indicates that the action is to be applied to all processes in
  the caller's own namespace. A rank of ``PMIX_RANK_WILDCARD`` in any element
  indicates that all processes in that namespace are to be included. All
  *clones* of an identified process have the requested action applied to them.
* ``ntargets``: Number of elements in the ``targets`` array.
* ``directives``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures describing the job control action(s) being requested (see
  `DIRECTIVES`_). A ``NULL`` value is supported when no directives are desired,
  though a request with no directives has no defined action.
* ``ndirs``: Number of elements in the ``directives`` array.

The blocking form returns any results directly:

* ``results``: Address at which a pointer to an array of ``pmix_info_t``
  structures containing the results of the request is returned. May be ``NULL``
  if the caller does not wish to receive returned data.
* ``nresults``: Address at which the number of elements in the returned
  ``results`` array is stored. May be ``NULL`` if ``results`` is ``NULL``.

The non-blocking form replaces ``results`` and ``nresults`` with a callback:

* ``cbfunc``: Callback function of type ``pmix_info_cbfunc_t`` invoked with the
  final status and any returned data once the request has been processed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request a job control action. ``PMIx_Job_control`` is the blocking form: it does
not return until the request has been processed (or fails), returning any
resulting data in the ``results`` array. ``PMIx_Job_control_nb`` is the
non-blocking form: it returns immediately, and the provided ``cbfunc`` is invoked
with the final status and any returned data once the request completes.

The ``targets`` array identifies the processes to which the requested action is
to be applied. Passing ``NULL`` for ``targets`` indicates that the action is to
be applied to all processes in the caller's namespace, equivalent to passing a
single ``pmix_proc_t`` containing the caller's namespace and a rank of
``PMIX_RANK_WILDCARD``. All clones of an identified process receive the action.

The requested action itself is expressed entirely through the ``directives``
array: each entry names a job control operation (for example, terminate, pause,
signal, or checkpoint) and any qualifying values. The PMIx library is not
required to interpret these directives itself |mdash| it passes them to the host
environment for processing |mdash| but it is required to add the
``PMIX_USERID`` and ``PMIX_GRPID`` of the requesting process so the host can
apply appropriate authorization. Actual support for any given directive depends
on the host environment.

The returned status indicates whether or not the request was granted; when a
request is denied, information as to the reason is returned in the ``results``
array (blocking form) or delivered to ``cbfunc`` (non-blocking form).

As with all non-blocking PMIx APIs, callers of ``PMIx_Job_control_nb`` **must**
keep the ``targets`` and ``directives`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The following attributes are used to describe the requested job control action.
PMIx libraries are not required to directly support any of these; they are passed
to the host environment, and support therefore depends on the implementation and
host. Host environments that implement this operation are required to support the
first group; the remainder are optional.

Required of supporting host environments:

* ``PMIX_JOB_CTRL_ID`` (char*) |mdash| a string identifier for this request,
  allowing its status to later be queried or the operation to be cancelled.
* ``PMIX_JOB_CTRL_PAUSE`` (bool) |mdash| pause the specified processes.
* ``PMIX_JOB_CTRL_RESUME`` (bool) |mdash| resume ("un-pause") the specified
  processes.
* ``PMIX_JOB_CTRL_KILL`` (bool) |mdash| forcibly terminate the specified
  processes and clean up.
* ``PMIX_JOB_CTRL_TERMINATE`` (bool) |mdash| politely terminate the specified
  processes.
* ``PMIX_JOB_CTRL_SIGNAL`` (int) |mdash| send the given signal to the specified
  processes.
* ``PMIX_REGISTER_CLEANUP`` (char*) |mdash| comma-delimited list of files to be
  removed upon termination of the specified processes.
* ``PMIX_REGISTER_CLEANUP_DIR`` (char*) |mdash| comma-delimited list of
  directories to be removed upon termination of the specified processes.
* ``PMIX_CLEANUP_RECURSIVE`` (bool) |mdash| recursively clean up all
  subdirectories under the specified one(s).
* ``PMIX_CLEANUP_EMPTY`` (bool) |mdash| only remove empty subdirectories.
* ``PMIX_CLEANUP_IGNORE`` (char*) |mdash| comma-delimited list of filenames that
  are not to be removed.
* ``PMIX_CLEANUP_LEAVE_TOPDIR`` (bool) |mdash| when recursively cleaning
  subdirectories, do not remove the top-level directory.

Optional for supporting host environments:

* ``PMIX_JOB_CTRL_CANCEL`` (char*) |mdash| cancel the request whose
  ``PMIX_JOB_CTRL_ID`` matches the provided value; a ``NULL`` value cancels all
  requests from this requestor.
* ``PMIX_JOB_CTRL_RESTART`` (char*) |mdash| restart the specified processes using
  the given checkpoint ID.
* ``PMIX_JOB_CTRL_CHECKPOINT`` (char*) |mdash| checkpoint the specified processes
  and assign the given ID to it.
* ``PMIX_JOB_CTRL_CHECKPOINT_EVENT`` (bool) |mdash| use event notification to
  trigger the process checkpoint.
* ``PMIX_JOB_CTRL_CHECKPOINT_SIGNAL`` (int) |mdash| use the given signal to
  trigger the process checkpoint.
* ``PMIX_JOB_CTRL_CHECKPOINT_TIMEOUT`` (int) |mdash| time, in seconds, to wait
  for the checkpoint to complete.
* ``PMIX_JOB_CTRL_CHECKPOINT_METHOD`` (pmix_data_array_t*) |mdash| array of
  ``pmix_info_t`` declaring each checkpoint method and value supported by the
  application.
* ``PMIX_JOB_CTRL_PROVISION`` (char*) |mdash| regular expression identifying nodes
  that are to be provisioned.
* ``PMIX_JOB_CTRL_PROVISION_IMAGE`` (char*) |mdash| name of the image that is to
  be provisioned.
* ``PMIX_JOB_CTRL_PREEMPTIBLE`` (bool) |mdash| register the job as available for
  preemption.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the request was granted;
any accompanying data is returned in the ``results`` array. For the non-blocking
form, a return of ``PMIX_SUCCESS`` indicates only that the request was accepted
for processing and the final status will be delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the request was granted (or accepted for processing,
  for the non-blocking form).
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the request was
  processed immediately and returned success; ``cbfunc`` will **not** be called.
* ``PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES`` |mdash| conflicting directives were
  given for job or process cleanup.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the operation is not supported (for example,
  a server whose host environment provides no job control module).
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_COMM_FAILURE`` |mdash| the connection to the server was lost before
  a response was received.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Job control is a request to the host environment, not a guarantee: the PMIx
library forwards the directives (annotated with the requester's ``PMIX_USERID``
and ``PMIX_GRPID``) to the host resource manager, which decides whether and how
to honor them. A request may therefore be denied for authorization or policy
reasons even when the named directives are individually supported.

The job control APIs are frequently used together with event notification and
the allocation APIs to build coordinated responses to failures and other events
|mdash| for example, requesting replacement resources with
``PMIx_Allocation_request(3)`` while directing that a checkpoint event be
delivered to the affected processes.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`,
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Session_control(3) <man3-PMIx_Session_control>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
