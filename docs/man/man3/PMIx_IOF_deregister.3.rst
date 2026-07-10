.. _man3-PMIx_IOF_deregister:

PMIx_IOF_deregister
===================

.. include_body

``PMIx_IOF_deregister`` |mdash| Deregister from output previously requested
via ``PMIx_IOF_pull``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_IOF_deregister(size_t iofhdlr,
                                     const pmix_info_t directives[], size_t ndirs,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  # ... after a successful foo.iof_pull() that returned refid ...
  pydirs = []
  rc = foo.iof_deregister(refid, pydirs)


INPUT PARAMETERS
----------------

* ``iofhdlr``: The reference identifier returned from the earlier call to
  :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>` (delivered through that call's
  registration callback, or as its blocking result) identifying the
  forwarding request to be canceled.
* ``directives``: Pointer to an array of :ref:`pmix_info_t(5)
  <man5-pmix_info_t>` structures conveying directives that control the
  behavior of the request |mdash| for example, directives regarding the
  disposition of any data currently in the I/O buffer for the affected
  processes. A ``NULL`` value is supported when no directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked when
  deregistration has completed. A ``NULL`` value makes the call blocking
  (see `DESCRIPTION`_).
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Cancel a forwarding request previously established with
:ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`. The request is thread-shifted
onto the PMIx progress thread for processing.

When ``cbfunc`` is non-``NULL`` the call is *non-blocking*: it returns
immediately and the result is delivered later through ``cbfunc``. When
``cbfunc`` is ``NULL`` the call is *blocking* and the result of the
operation is carried in the return value.

Note that any I/O being flushed as a result of the deregistration may
continue to be received after deregistration has completed. Implementations
are advised to flush any currently buffered I/O upon receipt of the request,
and to discard all I/O received after that point.


CALLBACK FUNCTION
-----------------

When provided, ``cbfunc`` has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

It is invoked with the final ``status`` of the deregistration and the
``cbdata`` pointer that was supplied to ``PMIx_IOF_deregister``.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status is delivered to ``cbfunc``. A return of
``PMIX_OPERATION_SUCCEEDED`` indicates that the request was processed
immediately and successfully, in which case ``cbfunc`` will **not** be
called.

For the blocking form (``cbfunc`` is ``NULL``), the return value carries the
result of the operation directly. Error constants that may be returned
include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the calling process is a server (that is
  not also a launcher) and therefore cannot deregister I/O forwarding.
* ``PMIX_ERR_UNREACH`` |mdash| the tool is not connected to a server.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for
  the request.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

The ``iofhdlr`` passed here must be the identifier returned by the matching
:ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>` request; passing an unknown
identifier results in an error.


.. seealso::
   :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`,
   :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`,
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
