.. _man3-PMIx_Register_event_handler:

PMIx_Register_event_handler
===========================

.. include_body

``PMIx_Register_event_handler`` |mdash| Register an event handler to be
notified of specified events.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Register_event_handler(pmix_status_t codes[], size_t ncodes,
                                             pmix_info_t info[], size_t ninfo,
                                             pmix_notification_fn_t evhdlr,
                                             pmix_hdlr_reg_cbfunc_t cbfunc,
                                             void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the codes is a list of Python integers (status codes)
  codes = [PMIX_ERR_PROC_ABORTED]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_EVENT_HDLR_NAME,
             'value': {'value': "MY-HANDLER", 'val_type': PMIX_STRING}}]
  # myhdlr is a Python event-handler function
  rc, refid = foo.register_event_handler(codes, pydirs, myhdlr)


INPUT PARAMETERS
----------------

* ``codes``: Pointer to an array of ``pmix_status_t`` codes identifying the
  events for which the handler is being registered. A ``NULL`` value (with
  ``ncodes`` of zero) registers a *default* handler that is invoked for any
  event not claimed by a code-specific handler. The codes need not be PMIx
  event constants |mdash| any integer value may be registered, allowing for
  non-PMIx events defined by a resource manager or by the application itself.
* ``ncodes``: Number of elements in the ``codes`` array.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the registration (see
  `DIRECTIVES`_). A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.
* ``evhdlr``: The event-handler function, of type :ref:`pmix_notification_fn_t <man5-pmix_notification_fn_t>`,
  to be called when a matching event is delivered (see `DESCRIPTION`_).
* ``cbfunc``: Callback function of type :ref:`pmix_hdlr_reg_cbfunc_t <man5-pmix_hdlr_reg_cbfunc_t>` invoked when
  the registration completes, delivering the final status and the reference
  identifier assigned to the handler. A ``NULL`` value makes the call blocking
  (see `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Register an event handler to report events. Three general classes of event can
be reported: events that occur within the client library but are not otherwise
reportable through an API (e.g., loss of connection to the server); job-related
events such as the failure of another process in the job or in a connected job;
and system notifications made available by the local administrators.

``PMIx_Register_event_handler`` is fundamentally a *non-blocking* registration.
The reference identifier assigned to the handler is delivered through the
registration callback ``cbfunc``, not returned directly, and no event
corresponding to this registration is delivered before ``cbfunc`` has been
invoked. When ``cbfunc`` is non-``NULL`` the function returns immediately, and
the provided ``cbfunc`` is called with the final status and the assigned
reference identifier once registration is complete.

When ``cbfunc`` is ``NULL`` the call is instead treated as *blocking*: the
function does not return until registration is complete, and the return value
itself carries the result |mdash| a value greater than or equal to zero is the
reference identifier for the handler, while a negative value is an error code
indicating the reason for failure.

The caller **must** retain the returned reference identifier: it is the value
passed to :ref:`PMIx_Deregister_event_handler(3)
<man3-PMIx_Deregister_event_handler>` to remove the handler. Modifying the
notification behavior of a handler is accomplished by deregistering it and
registering again with a revised set of ``info`` directives.

Default versus code-specific handlers
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If ``codes`` is ``NULL``, the handler is registered as a *default* handler:
it is invoked for any event that is not directed at a code-specific handler and
that is not flagged ``PMIX_EVENT_NON_DEFAULT``. If ``codes`` names one or more
specific status codes, the handler is registered against those codes only and
is invoked when a matching event is delivered. Multiple handlers may be
registered for different codes, and PMIx returns a distinct reference
identifier for each.

The event-handler callback
^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``evhdlr`` function has the signature ``pmix_notification_fn_t``:

.. code-block:: c

   typedef void (*pmix_notification_fn_t)(size_t evhdlr_registration_id,
                                          pmix_status_t status,
                                          const pmix_proc_t *source,
                                          pmix_info_t info[], size_t ninfo,
                                          pmix_info_t results[], size_t nresults,
                                          pmix_event_notification_cbfunc_fn_t cbfunc,
                                          void *cbdata);

When a matching event occurs, PMIx invokes ``evhdlr`` with the reference
identifier of the handler being called, the ``status`` code of the event, the
``source`` process that generated it (an empty namespace with a rank of
``PMIX_RANK_UNDEF`` indicates the resource manager), any ``info`` describing the
event, and any ``results`` aggregated from handlers that ran earlier in the
chain for this event. Different resource managers provide differing levels of
detail, so the ``info`` array may be ``NULL``.

Each handler **must**, upon completing its work and before returning, call the
event-notification completion function ``cbfunc`` that was passed to it (along
with a status code and any information to be passed to later handlers) so that
the event chain can continue to be progressed. A handler may terminate all
further progress along the chain by passing ``PMIX_EVENT_ACTION_COMPLETE`` to
that completion function. The parameters passed to the handler (including the
``info`` and ``results`` arrays) cease to be valid once the completion function
has been called, so any data needed afterward must be copied.

As with all non-blocking PMIx APIs, callers **must** keep the ``codes`` and
``info`` arrays valid until the registration ``cbfunc`` is invoked.


DIRECTIVES
----------

The following attributes are relevant to this operation and are required to be
supported by all PMIx libraries. They govern the placement of the handler
within the ordered list of handlers |mdash| its *precedence* |mdash| and label
the registration.

* ``PMIX_EVENT_HDLR_NAME`` (char*) |mdash| a string name identifying this
  handler, used both for diagnostics and as the target referenced by the
  ``PMIX_EVENT_HDLR_BEFORE`` and ``PMIX_EVENT_HDLR_AFTER`` directives.
* ``PMIX_EVENT_HDLR_FIRST`` (bool) |mdash| invoke this handler before any other
  handler, across all categories. Only one handler may hold this position;
  registering a second returns an error.
* ``PMIX_EVENT_HDLR_LAST`` (bool) |mdash| invoke this handler after all other
  handlers have been called. Only one handler may hold this position.
* ``PMIX_EVENT_HDLR_FIRST_IN_CATEGORY`` (bool) |mdash| invoke this handler
  before any other handler in its category (single-code, multi-code, or
  default).
* ``PMIX_EVENT_HDLR_LAST_IN_CATEGORY`` (bool) |mdash| invoke this handler after
  all other handlers in its category have been called.
* ``PMIX_EVENT_HDLR_PREPEND`` (bool) |mdash| prepend this handler to the
  precedence list within its category.
* ``PMIX_EVENT_HDLR_APPEND`` (bool) |mdash| append this handler to the
  precedence list within its category.
* ``PMIX_EVENT_HDLR_BEFORE`` (char*) |mdash| place this handler immediately
  before the handler whose name is given in the value.
* ``PMIX_EVENT_HDLR_AFTER`` (char*) |mdash| place this handler immediately
  after the handler whose name is given in the value.
* ``PMIX_EVENT_CUSTOM_RANGE`` (pmix_data_array_t\*) |mdash| an array of
  ``pmix_proc_t`` (or a single ``pmix_proc_t``) identifying the processes that
  are to receive this notification.
* ``PMIX_RANGE`` (pmix_data_range_t) |mdash| restrict the range over which the
  handler applies.
* ``PMIX_EVENT_RETURN_OBJECT`` (void*) |mdash| an object to be returned to this
  process whenever the registered handler is invoked. The object is returned
  only to the process that registered it.
* ``PMIX_EVENT_ONESHOT`` (bool) |mdash| automatically deregister this handler
  after it has been invoked once.

Host environments that implement PMIx event notification are additionally
required to support the following directives, which restrict the handler to
events affecting the indicated process(es):

* ``PMIX_EVENT_AFFECTED_PROC`` (pmix_proc_t) |mdash| the single process that
  must be affected by the event for this handler to be invoked.
* ``PMIX_EVENT_AFFECTED_PROCS`` (pmix_data_array_t\*) |mdash| an array of
  ``pmix_proc_t`` identifying the affected processes for which this handler is
  to be invoked.


RETURN VALUE
------------

For the blocking form (``cbfunc`` is ``NULL``), a return value greater than or
equal to zero is the reference identifier assigned to the handler; a negative
value is an error constant. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status and the assigned reference identifier are delivered to
``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| (non-blocking form) the request was accepted for
  processing.
* ``PMIX_ERR_EVENT_REGISTRATION`` |mdash| the registration failed |mdash| for
  example, a requested "first" or "last" position was already claimed by
  another handler, or a named handler referenced by ``PMIX_EVENT_HDLR_BEFORE``
  or ``PMIX_EVENT_HDLR_AFTER`` could not be found.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied (e.g., a
  malformed ``PMIX_EVENT_CUSTOM_RANGE`` value).
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  registration.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

To avoid conflicts with PMIx-defined codes, applications and resource managers
that register their own event codes are advised to use values that lie outside
the range of the PMIx standard's error and event codes |mdash| that is,
positive values, or negative values beyond the ``PMIX_EXTERNAL_ERR_BASE``
boundary.


.. seealso::
   :ref:`PMIx_Deregister_event_handler(3) <man3-PMIx_Deregister_event_handler>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
