.. _man3-PMIx_Process_monitor:

PMIx_Process_monitor
====================

.. include_body

``PMIx_Process_monitor``, ``PMIx_Process_monitor_nb`` |mdash| Request that
processes, files, or resources be monitored for activity or usage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Process_monitor(const pmix_info_t *monitor,
                                      pmix_status_t error,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_t **results, size_t *nresults);

   pmix_status_t PMIx_Process_monitor_nb(const pmix_info_t *monitor,
                                         pmix_status_t error,
                                         const pmix_info_t directives[], size_t ndirs,
                                         pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the monitor action is a single Python ``pmix_info_t`` dictionary
  monitor = {'key': PMIX_MONITOR_HEARTBEAT,
             'value': {'value': True, 'val_type': PMIX_BOOL}}
  # the directives are a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_MONITOR_HEARTBEAT_TIME,
             'value': {'value': 5, 'val_type': PMIX_UINT32}}]
  rc, results = foo.monitor(monitor, PMIX_MONITOR_HEARTBEAT_ALERT, pydirs)


INPUT PARAMETERS
----------------

* ``monitor``: Pointer to a single :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structure whose key specifies the monitoring **action** being requested |mdash|
  for example ``PMIX_MONITOR_HEARTBEAT`` to have the server watch the requestor for
  periodic heartbeats, or ``PMIX_MONITOR_FILE`` to monitor a file for signs of
  life. A ``NULL`` value returns ``PMIX_ERR_BAD_PARAM``.
* ``error``: The ``pmix_status_t`` code the monitor is to use when generating an
  event notification alerting that the monitored condition has been triggered (or
  to carry a periodic resource-usage update). The default range of the resulting
  event is ``PMIX_RANGE_NAMESPACE``; this can be changed with a ``PMIX_RANGE``
  directive.
* ``directives``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures characterizing the request |mdash| e.g., the checking frequency or the
  set of target processes, nodes, or files (see `DIRECTIVES`_). A ``NULL`` value is
  supported when no directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.


OUTPUT PARAMETERS
-----------------

* ``results`` (blocking form): Address where a pointer to an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures containing the results of the
  request is returned. Initialized to ``NULL`` on entry; any returned array must be
  released by the caller with ``PMIX_INFO_FREE``.
* ``nresults`` (blocking form): Address where the number of elements in the
  ``results`` array is returned.

The non-blocking form replaces ``results``/``nresults`` with a callback:

* ``cbfunc``: Callback function of type ``pmix_info_cbfunc_t`` invoked with the
  final status and any returned data once the request has been processed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request that application processes, files, and/or resources be monitored.
``PMIx_Process_monitor`` is the blocking form: it does not return until the
request has been processed, at which point any measured values are returned in the
``results`` array. ``PMIx_Process_monitor_nb`` is the non-blocking form: it returns
immediately, and the provided ``cbfunc`` is invoked with the final status and any
returned data.

This API serves two purposes:

* **Liveness monitoring.** Request that the server watch a process or file for
  activity |mdash| for example, that it monitor a given process for periodic
  heartbeats as an indication that the process has not become "wedged". When a
  monitor detects the specified alarm condition, it generates an event
  notification using the provided ``error`` code and passes along any relevant
  information. It is up to the caller to register a corresponding event handler
  (e.g., for ``PMIX_MONITOR_HEARTBEAT_ALERT`` or ``PMIX_MONITOR_FILE_ALERT``).
* **Resource-usage reporting.** Report resource-usage statistics for processes,
  nodes, disks, or network interfaces. This can be done on a per-request basis, or
  updated periodically on a time interval specified by the
  ``PMIX_MONITOR_RESOURCE_RATE`` directive.

The ``monitor`` argument is an attribute naming the type of monitor being
requested. The ``directives`` argument characterizes the request (e.g., which file
to monitor, or the checking interval and number of allowed misses).

A process that is being watched for heartbeats emits them with the companion
:ref:`PMIx_Heartbeat(3) <man3-PMIx_Heartbeat>` function; a heartbeat may also be
sent by passing the ``PMIX_SEND_HEARTBEAT`` attribute as the ``monitor`` action.

As with all non-blocking PMIx APIs, callers of ``PMIx_Process_monitor_nb`` **must**
keep the ``monitor`` and ``directives`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The ``monitor`` argument may carry any of the following actions:

* ``PMIX_MONITOR_HEARTBEAT`` (bool) |mdash| register to have the server monitor the
  requestor for heartbeats.
* ``PMIX_SEND_HEARTBEAT`` (bool) |mdash| send a heartbeat to the local server
  (equivalent to calling :ref:`PMIx_Heartbeat(3) <man3-PMIx_Heartbeat>`). A server
  is not permitted to use this action and receives ``PMIX_ERR_BAD_PARAM``.
* ``PMIX_MONITOR_FILE`` (char*) |mdash| register to monitor the named file for signs
  of life.
* ``PMIX_MONITOR_PROC_RESOURCE_USAGE`` / ``PMIX_MONITOR_NODE_RESOURCE_USAGE`` /
  ``PMIX_MONITOR_DISK_RESOURCE_USAGE`` / ``PMIX_MONITOR_NET_RESOURCE_USAGE``
  (pmix_data_array_t*) |mdash| report usage of the resources specified in the
  provided array for processes, nodes, disks, or network interfaces respectively.
* ``PMIX_MONITOR_CANCEL`` (char*) |mdash| cancel a previously requested monitoring
  action, identified by the string supplied via ``PMIX_MONITOR_ID`` (``NULL``
  cancels all).

Action-specific directives (placed in the ``directives`` array) include:

* ``PMIX_MONITOR_HEARTBEAT_TIME`` (uint32_t) |mdash| time, in seconds, before a
  missed heartbeat is declared.
* ``PMIX_MONITOR_HEARTBEAT_DROPS`` (uint32_t) |mdash| number of heartbeats that may
  be missed before the alarm is raised.
* ``PMIX_MONITOR_FILE_SIZE`` (bool) |mdash| track whether the file's size is growing
  to determine that the application is running.
* ``PMIX_MONITOR_FILE_ACCESS`` (char*) |mdash| track time since the file was last
  accessed.
* ``PMIX_MONITOR_FILE_MODIFY`` (char*) |mdash| track time since the file was last
  modified.
* ``PMIX_MONITOR_FILE_CHECK_TIME`` (uint32_t) |mdash| time, in seconds, between file
  checks.
* ``PMIX_MONITOR_FILE_DROPS`` (uint32_t) |mdash| number of file checks that may be
  missed before the alarm is raised.
* ``PMIX_MONITOR_RESOURCE_RATE`` (uint32_t) |mdash| report resource usage every N
  seconds.
* ``PMIX_MONITOR_TARGET_PROCS`` (pmix_data_array_t*) |mdash| array of process IDs
  identifying the processes to be monitored.
* ``PMIX_MONITOR_TARGET_PIDS`` (pmix_data_array_t*) |mdash| array of
  ``pmix_node_pid_t`` structures to be monitored.
* ``PMIX_MONITOR_TARGET_NODES`` (pmix_data_array_t*) |mdash| array of string host
  names to be monitored.
* ``PMIX_MONITOR_TARGET_NODEIDS`` (pmix_data_array_t*) |mdash| array of ``uint32_t``
  node IDs to be monitored.
* ``PMIX_MONITOR_TARGET_DISKS`` (pmix_data_array_t*) |mdash| array of disk
  identifiers to be monitored.
* ``PMIX_MONITOR_TARGET_NETS`` (pmix_data_array_t*) |mdash| array of network
  identifiers to be monitored.

General directives that may accompany any action include:

* ``PMIX_MONITOR_ID`` (char*) |mdash| string identifier for this request, used later
  to cancel it.
* ``PMIX_MONITOR_APP_CONTROL`` (bool) |mdash| the application wishes to control the
  response when the monitor is triggered.
* ``PMIX_MONITOR_LOCAL_ONLY`` (bool) |mdash| restrict data collection to the local
  host, regardless of any provided targets.
* ``PMIX_MONITOR_PROXY`` (pmix_proc_t*) |mdash| the process on whose behalf the
  monitoring is being requested, if different from the caller.
* ``PMIX_RANGE`` (pmix_data_range_t) |mdash| non-default range to use when
  generating the associated event for this monitoring action.

When a monitoring request involves other nodes, the library adds the
``PMIX_USERID`` and ``PMIX_GRPID`` of the requesting process to the directives it
passes to its host environment.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the request was processed
without error and that any results have been placed in the ``results`` array. For
the non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request
was accepted for processing; the final status and any data are delivered to
``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the request was processed successfully.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, a ``NULL`` ``monitor``, or a server attempting to use
  ``PMIX_SEND_HEARTBEAT``.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the request involves other nodes but the host
  environment provides no monitoring support.
* ``PMIX_ERR_UNREACH`` |mdash| the caller is not a server and its local PMIx server
  could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

A ``PMIX_SUCCESS`` return only indicates that the request was executed without
error; it does not guarantee a non-``NULL`` ``results`` array when requesting
resource-usage statistics. For example, a request for the resource usage of
processes on a node that is not currently running any user-level processes returns
success with an empty ``results`` array because no statistics were produced.

Support for individual monitoring actions and directives is implementation- and
host-environment-dependent.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Heartbeat(3) <man3-PMIx_Heartbeat>`,
   :ref:`PMIx_Log(3) <man3-PMIx_Log>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
