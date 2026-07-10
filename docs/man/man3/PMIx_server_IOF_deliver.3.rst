.. _man3-PMIx_server_IOF_deliver:

PMIx_server_IOF_deliver
=======================

.. include_body

``PMIx_server_IOF_deliver`` |mdash| Pass forwarded I/O to the PMIx server
library for distribution to its clients.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_IOF_deliver(const pmix_proc_t *source,
                                         pmix_iof_channel_t channel,
                                         const pmix_byte_object_t *bo,
                                         const pmix_info_t info[], size_t ninfo,
                                         pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  source = {'nspace': "myapp", 'rank': 0}
  channel = PMIX_FWD_STDOUT_CHANNEL
  data = {'bytes': "hello world"}
  pydirs = []
  rc = foo.iof_deliver(source, channel, data, pydirs)


INPUT PARAMETERS
----------------

* ``source``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` identifying
  the process that generated the data being forwarded.
* ``channel``: The :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`
  identifying the I/O channel of the data |mdash| e.g.,
  ``PMIX_FWD_STDOUT_CHANNEL`` or ``PMIX_FWD_STDERR_CHANNEL``.
* ``bo``: Pointer to a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  containing the payload to be delivered.
* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  conveying optional metadata describing the data (see `DIRECTIVES`_). A
  ``NULL`` value (with ``ninfo`` of zero) is supported when no metadata is
  provided.
* ``ninfo``: Number of elements in the ``info`` array.
* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked once the
  library no longer requires access to the provided data. A ``NULL`` value makes
  the call blocking (see `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Pass forwarded I/O |mdash| such as the ``stdout`` or ``stderr`` output of a
remote process |mdash| into the local PMIx server library for distribution to
its clients. The library is responsible for determining which of its clients
have registered (via :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`) to receive
data from the given ``source`` on the given ``channel``, and for delivering the
payload only to those clients.

``PMIx_server_IOF_deliver`` supports both blocking and non-blocking operation.
When ``cbfunc`` is non-``NULL`` the call is **non-blocking**: it thread-shifts
the request into the library's progress thread, returns immediately, and invokes
``cbfunc`` once the library no longer needs the provided byte object. When
``cbfunc`` is ``NULL`` the call is **blocking**: it does not return until the
operation has completed.

The host RM is required to retain the ``bo`` byte object (and the ``info``
array) until the callback is executed |mdash| or, in the blocking case, until
the function returns. If the function returns a non-success status, the library
did not take ownership and the host may release the data immediately.


DIRECTIVES
----------

The following attributes may appear in the ``info`` array to describe the
forwarded data. Unrecognized attributes are ignored.

* ``PMIX_IOF_COMPLETE`` (bool) |mdash| indicates that the specified source I/O
  channel has been closed; no further data will be forwarded from it.
* ``PMIX_IOF_TAG_OUTPUT`` (bool) |mdash| the output should be tagged with the
  identity (namespace/rank) and channel of its source.
* ``PMIX_IOF_RANK_OUTPUT`` (bool) |mdash| the output should be tagged with the
  rank from which it originated.
* ``PMIX_IOF_XML_OUTPUT`` (bool) |mdash| the output should be formatted in XML.
* ``PMIX_IOF_OUTPUT_RAW`` (bool) |mdash| deliver the output without buffering it
  into complete lines.


CALLBACK FUNCTION
-----------------

For the non-blocking form, the ``cbfunc`` has the signature
``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes ``cbfunc`` with the final ``status`` and the original
``cbdata`` once it no longer requires access to the ``bo`` byte object and the
``info`` array, allowing the host to release them.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` is non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status is delivered through ``cbfunc``. For the blocking form
(``cbfunc`` is ``NULL``), a return of ``PMIX_OPERATION_SUCCEEDED`` indicates
that the request was immediately processed and completed successfully |mdash|
``cbfunc`` is not called. Possible return values include:

* ``PMIX_SUCCESS`` |mdash| (non-blocking form) the request was accepted for
  processing.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (blocking form) the operation completed
  successfully.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  request.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This function is the server-side counterpart to the client I/O-forwarding
registration API :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`: the host RM
collects output from remote processes and injects it here, and the library
routes it to whichever local clients requested it.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`,
   :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`,
   :ref:`PMIx_IOF_deregister(3) <man3-PMIx_IOF_deregister>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
