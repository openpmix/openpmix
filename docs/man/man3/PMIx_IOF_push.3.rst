.. _man3-PMIx_IOF_push:

PMIx_IOF_push
=============

.. include_body

``PMIx_IOF_push`` |mdash| Push data collected locally (typically from
standard input) to the standard input of target recipients.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_IOF_push(const pmix_proc_t targets[], size_t ntargets,
                               pmix_byte_object_t *bo,
                               const pmix_info_t directives[], size_t ndirs,
                               pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  # ... after a successful foo.init() and attachment to a server ...
  pytargets = [{'nspace': "target-nspace", 'rank': PMIX_RANK_WILDCARD}]
  # data carries the bytes to be delivered to the targets' stdin
  data = {'bytes': "some input text"}
  pydirs = []
  rc = foo.iof_push(pytargets, data, pydirs)


INPUT PARAMETERS
----------------

* ``targets``: Pointer to an array of :ref:`pmix_proc_t(5)
  <man5-pmix_proc_t>` identifiers naming the processes to which the data is
  to be delivered. A rank of ``PMIX_RANK_WILDCARD`` directs that all
  processes in the given namespace receive a copy of the data.
* ``ntargets``: Number of elements in the ``targets`` array.
* ``bo``: Pointer to a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  containing the payload (typically ``stdin`` data) to be delivered. May be
  ``NULL`` when the call is being used solely to start or stop automatic
  ``stdin`` collection via directives.
* ``directives``: Pointer to an array of :ref:`pmix_info_t(5)
  <man5-pmix_info_t>` structures conveying directives that control the
  behavior of the request (see `DIRECTIVES`_). A ``NULL`` value is supported
  when no directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked when the
  operation has completed. A ``NULL`` value makes the call blocking (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

``PMIx_IOF_push`` is used in one of three ways:

* to push data the caller has collected itself (typically from ``stdin`` or
  a file) to the standard input of the target recipients;
* to request that the PMIx library automatically collect the caller's own
  ``stdin`` and forward it to the target recipients (via the
  ``PMIX_IOF_PUSH_STDIN`` directive); or
* to indicate that automatic collection and transmittal of ``stdin`` is to
  stop (via the ``PMIX_IOF_COMPLETE`` directive).

The request is thread-shifted onto the PMIx progress thread for processing.
When ``cbfunc`` is non-``NULL`` the call is *non-blocking*: it returns
``PMIX_SUCCESS`` immediately and the result is delivered later through
``cbfunc``. When ``cbfunc`` is ``NULL`` the call is *blocking* and the
result of the operation is carried in the return value.

Execution of ``cbfunc`` serves as notice that the PMIx library no longer
requires the caller to maintain the ``bo`` data object; it does **not**
indicate that the payload has been delivered to the targets. As with all
non-blocking PMIx APIs, callers **must** keep the ``targets``, ``bo``, and
``directives`` arguments valid until the callback has fired.


DIRECTIVES
----------

The following attributes are required to be supported by PMIx libraries that
provide I/O forwarding:

* ``PMIX_IOF_CACHE_SIZE`` (uint32_t) |mdash| requested size, in bytes, of the
  server cache for each specified channel.
* ``PMIX_IOF_DROP_OLDEST`` (bool) |mdash| in an overflow situation, drop the
  oldest cached bytes to make room for newly arriving data.
* ``PMIX_IOF_DROP_NEWEST`` (bool) |mdash| in an overflow situation, drop newly
  arriving bytes until room becomes available in the cache.

The following attributes are optional:

* ``PMIX_IOF_BUFFERING_SIZE`` (uint32_t) |mdash| control the grouping of data
  so that it is delivered in blocks of the requested size.
* ``PMIX_IOF_BUFFERING_TIME`` (uint32_t) |mdash| maximum time, in seconds, to
  buffer data before delivering it.
* ``PMIX_IOF_PUSH_STDIN`` (bool) |mdash| request that the PMIx library collect
  the ``stdin`` of the caller and forward it to the specified targets. All
  collected data is sent to the same targets until ``stdin`` is closed or a
  subsequent ``PMIx_IOF_push`` call carrying ``PMIX_IOF_COMPLETE`` terminates
  the forwarding.
* ``PMIX_IOF_COMPLETE`` (bool) |mdash| indicate that forwarding of ``stdin``
  to the targets is to be terminated.


CALLBACK FUNCTION
-----------------

When provided, ``cbfunc`` has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

It is invoked with the final ``status`` of the operation and the ``cbdata``
pointer that was supplied to ``PMIx_IOF_push``.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status is delivered to ``cbfunc``.

For the blocking form (``cbfunc`` is ``NULL``), the return value carries the
result of the operation |mdash| ``PMIX_SUCCESS`` (or
``PMIX_OPERATION_SUCCEEDED`` when the request was satisfied immediately) on
success, or a negative error constant on failure. Error constants that may
be returned include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

Use of ``PMIX_RANK_WILDCARD`` to target all processes in a namespace is
supported but should be used with care due to the bandwidth and memory
footprint it can incur.


.. seealso::
   :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`,
   :ref:`PMIx_IOF_deregister(3) <man3-PMIx_IOF_deregister>`,
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
