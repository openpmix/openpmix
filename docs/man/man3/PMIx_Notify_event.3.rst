.. _man3-PMIx_Notify_event:

PMIx_Notify_event
=================

.. include_body

``PMIx_Notify_event`` |mdash| Report an event for distribution to any registered
event handlers.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Notify_event(pmix_status_t status,
                                   const pmix_proc_t *source,
                                   pmix_data_range_t range,
                                   const pmix_info_t info[], size_t ninfo,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the source is a Python ``pmix_proc_t`` dictionary
  src = {'nspace': "testnspace", 'rank': 0}
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pyinfo = [{'key': PMIX_EVENT_NON_DEFAULT,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc = foo.notify_event(PMIX_ERR_PROC_ABORTED, src, PMIX_RANGE_NAMESPACE, pyinfo)


INPUT PARAMETERS
----------------

* ``status``: The ``pmix_status_t`` code identifying the event being reported.
  Callers are not constrained to the status values defined by the PMIx Standard;
  any integer value may be used, allowing applications to define and notify their
  own internal events.
* ``source``: Pointer to a ``pmix_proc_t`` identifying the process that generated
  the event. A ``NULL`` value indicates that the caller itself is the source of the
  event.
* ``range``: A ``pmix_data_range_t`` value specifying the range across which the
  event is to be delivered (see `RANGE`_).
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures provided by the event generator to pass any additional information
  about the event |mdash| for example, the processes affected by the event, its
  nature, and its severity (see `DIRECTIVES`_). The precise contents depend on the
  event generator. A ``NULL`` value is supported when no additional information is
  provided.
* ``ninfo``: Number of elements in the ``info`` array.

The blocking behavior is selected by the callback parameters:

* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` to be invoked upon
  completion of the notification's local actions. If ``cbfunc`` is ``NULL``, the
  call is treated as **blocking** and the result of the operation is returned in
  the function's status code.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Report an event so that it can be distributed to any registered event handlers
according to the specified ``range``. ``PMIx_Notify_event`` may be called by any
PMIx process |mdash| application clients, tools, PMIx servers, and host environment
(SMS) daemons alike.

A client application calls this function to notify the resource manager and/or
other processes of an event it encountered. It may also be used to asynchronously
notify other parts of the caller's own process |mdash| for example, allowing one
library to signal another when it has completed initialization |mdash| by using a
``range`` of ``PMIX_RANGE_PROC_LOCAL``. A PMIx server calls this function to report
events it has detected, and host SMS daemons call it to pass events down to the
embedded PMIx server for delivery to local clients and for the host's own
registered handlers.

When ``cbfunc`` is non-``NULL`` this is a non-blocking call: it returns immediately
and ``cbfunc`` is invoked once the notification's local actions are complete. At
that point any messages required to carry the notification (e.g., to the local
PMIx server) will have been queued, but may not yet have been transmitted. When
``cbfunc`` is ``NULL`` the call blocks and the result is returned directly.

A successful return reflects only that the request was accepted (or completed)
locally; it does **not** indicate the success or failure of delivering the event to
any recipient.

As with all non-blocking PMIx APIs, the caller **must** keep the ``source`` and
``info`` arrays valid until ``cbfunc`` is invoked |mdash| the sole purpose of the
callback is to indicate when the input data is no longer required.


RANGE
-----

The ``range`` argument constrains which processes are eligible to receive the
event notification:

* ``PMIX_RANGE_PROC_LOCAL`` |mdash| restrict delivery to the caller's own process;
  the event is not sent to the local PMIx server.
* ``PMIX_RANGE_LOCAL`` |mdash| deliver only to processes on the same local node as
  the event generator.
* ``PMIX_RANGE_NAMESPACE`` |mdash| deliver only to processes in the same namespace
  as the source.
* ``PMIX_RANGE_SESSION`` |mdash| deliver to all processes in the session.
* ``PMIX_RANGE_GLOBAL`` |mdash| deliver to all processes.
* ``PMIX_RANGE_CUSTOM`` |mdash| deliver to a set of processes specified in the
  ``info`` array (see the ``PMIX_EVENT_CUSTOM_RANGE`` directive).
* ``PMIX_RANGE_RM`` |mdash| the event is intended for the host resource manager.
* ``PMIX_RANGE_UNDEF`` |mdash| no range was specified; ``PMIX_RANGE_INVALID``
  denotes an invalid value.


DIRECTIVES
----------

The following attributes are required to be supported by all PMIx libraries and
will be passed, when relevant, to all invoked handlers:

* ``PMIX_EVENT_NON_DEFAULT`` (bool) |mdash| the event is not to be delivered to
  default event handlers, only to those that explicitly registered for it.
* ``PMIX_EVENT_CUSTOM_RANGE`` (pmix_data_array_t*) |mdash| array of
  ``pmix_proc_t`` identifying the processes that are to receive the notification
  when a ``range`` of ``PMIX_RANGE_CUSTOM`` is specified.
* ``PMIX_EVENT_DO_NOT_CACHE`` (bool) |mdash| instruct the PMIx server not to cache
  the event for later delivery to processes that have not yet started.
* ``PMIX_EVENT_PROXY`` (pmix_proc_t*) |mdash| the process that relayed the event on
  behalf of the original source.
* ``PMIX_EVENT_TEXT_MESSAGE`` (char*) |mdash| a human-readable text message
  describing the event, suitable for reporting to the user.

Host environments that implement support for PMIx event notification are required
to provide the following attributes for all events they generate:

* ``PMIX_EVENT_AFFECTED_PROC`` (pmix_proc_t*) |mdash| single process affected by
  the event.
* ``PMIX_EVENT_AFFECTED_PROCS`` (pmix_data_array_t*) |mdash| array of
  ``pmix_proc_t`` identifying the processes affected by the event.

The following attributes may optionally be included by a host environment to
indicate its intended response to an environmental or SMS event:

* ``PMIX_EVENT_TERMINATE_SESSION`` (bool) |mdash| the RM intends to terminate the
  session.
* ``PMIX_EVENT_TERMINATE_JOB`` (bool) |mdash| the RM intends to terminate the
  entire job.
* ``PMIX_EVENT_TERMINATE_NODE`` (bool) |mdash| the RM intends to terminate all
  processes on the affected node.
* ``PMIX_EVENT_TERMINATE_PROC`` (bool) |mdash| the RM intends to terminate just the
  affected process(es).
* ``PMIX_EVENT_ACTION_TIMEOUT`` (int) |mdash| the time, in seconds, before the RM
  will execute the indicated termination action.


RETURN VALUE
------------

For the non-blocking form (a non-``NULL`` ``cbfunc``), a return of ``PMIX_SUCCESS``
indicates only that the request was accepted for processing and the final status
will be delivered to ``cbfunc``. For the blocking form (``NULL`` ``cbfunc``), the
returned status reflects the result of the local operation.

* ``PMIX_SUCCESS`` |mdash| the request was accepted; for a non-blocking call,
  ``cbfunc`` will be invoked upon completion.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| the request was processed immediately and
  successfully; ``cbfunc`` will **not** be called.
* ``PMIX_ERR_UNREACH`` |mdash| the caller is not connected to its PMIx server and
  the requested ``range`` requires reaching it.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory to process
  the request.

Note that a successful return does **not** reflect the success or failure of
delivering the event to any recipient. Any other negative value indicates an
appropriate error condition. PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

The PMIx server library is not permitted to echo an event handed to it by its host
back to that host. When a host delivers an event via this API, it is required to
also deliver that event to all PMIx servers where the targeted processes are
currently running |mdash| or might run in the future |mdash| since events are
cached by the PMIx server library for delivery to processes that have not yet
started (unless ``PMIX_EVENT_DO_NOT_CACHE`` is specified).

Event handlers are registered with
:ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>` and
removed with
:ref:`PMIx_Deregister_event_handler(3) <man3-PMIx_Deregister_event_handler>`.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
   :ref:`PMIx_Deregister_event_handler(3) <man3-PMIx_Deregister_event_handler>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_data_range_t(5) <man5-pmix_data_range_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
