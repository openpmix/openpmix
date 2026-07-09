.. _man3-PMIx_Deregister_event_handler:

PMIx_Deregister_event_handler
=============================

.. include_body

``PMIx_Deregister_event_handler`` |mdash| Deregister a previously registered
event handler.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Deregister_event_handler(size_t evhdlr_ref,
                                               pmix_op_cbfunc_t cbfunc,
                                               void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() and register_event_handler() ...
  # refid is the reference identifier returned by registration
  rc = foo.deregister_event_handler(refid)


INPUT PARAMETERS
----------------

* ``evhdlr_ref``: The reference identifier (a ``size_t``) that was returned by
  the corresponding call to :ref:`PMIx_Register_event_handler(3)
  <man3-PMIx_Register_event_handler>` when the handler was registered.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be invoked when
  the deregistration completes. A ``NULL`` value makes the call blocking (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Remove a previously registered event handler, identified by the reference
identifier assigned to it at registration time.

If ``cbfunc`` is ``NULL`` the call is *blocking*: it does not return until the
deregistration is complete, and the result of the operation is returned in the
status code. If ``cbfunc`` is non-``NULL`` the call is *non-blocking*: it
returns immediately, and the provided ``cbfunc`` is invoked with the final
status once the operation completes.

No event corresponding to the referenced registration is delivered once the
deregistration operation has completed |mdash| that is, following return from
the API with ``PMIX_OPERATION_SUCCEEDED`` or execution of ``cbfunc``.


RETURN VALUE
------------

For the blocking form, the return value carries the result of the operation.
For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the
request was accepted for processing and the final status will be delivered to
``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the handler was successfully deregistered.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the request was
  satisfied immediately and ``cbfunc`` will **not** be called.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the provided ``evhdlr_ref`` was not recognized.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the PMIx implementation does not support
  event notification.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


.. seealso::
   :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
