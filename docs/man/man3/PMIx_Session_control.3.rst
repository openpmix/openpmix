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

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_SESSION_APP,
             'value': {'value': "myapp", 'val_type': PMIX_STRING}}]
  rc = foo.session_control(1, pydirs)


INPUT PARAMETERS
----------------

* ``sessionID``: A ``uint32_t`` identifying the session to which the control
  action is to be applied. A value of ``UINT32_MAX`` indicates that the action
  applies to all sessions under the caller's control.
* ``directives``: Pointer to an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures describing the control
  action(s) to be taken. A ``NULL`` value is supported when no directives are
  desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type ``pmix_info_cbfunc_t``. If ``NULL``, the
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


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
