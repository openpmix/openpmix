.. _man3-PMIx_IOF_pull:

PMIx_IOF_pull
=============

.. include_body

``PMIx_IOF_pull`` |mdash| Register to receive output forwarded from a set of
remote processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_IOF_pull(const pmix_proc_t procs[], size_t nprocs,
                               const pmix_info_t directives[], size_t ndirs,
                               pmix_iof_channel_t channel,
                               pmix_iof_cbfunc_t cbfunc,
                               pmix_hdlr_reg_cbfunc_t regcbfunc,
                               void *regcbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  # ... after a successful foo.init() and attachment to a server ...
  # the procs is a list of Python ``pmix_proc_t`` dictionaries
  pyprocs = [{'nspace': "target-nspace", 'rank': PMIX_RANK_WILDCARD}]
  pydirs = [{'key': PMIX_IOF_TAG_OUTPUT,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  channel = PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL
  # myhdlr is a Python IOF delivery function
  rc, refid = foo.iof_pull(pyprocs, channel, pydirs, myhdlr)


INPUT PARAMETERS
----------------

* ``procs``: Pointer to an array of :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
  identifiers naming the source processes whose output is being requested. A
  rank of ``PMIX_RANK_WILDCARD`` requests the output of all processes in the
  specified namespace.
* ``nprocs``: Number of elements in the ``procs`` array.
* ``directives``: Pointer to an array of :ref:`pmix_info_t(5)
  <man5-pmix_info_t>` structures conveying directives that control the
  behavior of the request (see `DIRECTIVES`_). A ``NULL`` value is supported
  when no directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``channel``: A :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`
  bitmask identifying the I/O channels included in the request (for example,
  ``PMIX_FWD_STDOUT_CHANNEL`` and/or ``PMIX_FWD_STDERR_CHANNEL``).
  ``PMIX_FWD_STDIN_CHANNEL`` is **not** supported on this path and results in
  an error.
* ``cbfunc``: Function of type :ref:`pmix_iof_cbfunc_t <man5-pmix_iof_cbfunc_t>` to be called when
  forwarded output is received (see `CALLBACK FUNCTION`_). A ``NULL`` value
  directs that output for the indicated channels be written to this process's
  own ``stdout``/``stderr`` file descriptors instead of being delivered to a
  callback.
* ``regcbfunc``: Registration-completion callback of type
  :ref:`pmix_hdlr_reg_cbfunc_t <man5-pmix_hdlr_reg_cbfunc_t>`. Because registration is asynchronous, this
  function is invoked once registration completes, delivering the final
  status and the reference identifier assigned to the request. A ``NULL``
  value makes the call blocking (see `DESCRIPTION`_).
* ``regcbdata``: Opaque pointer passed, unmodified, to ``regcbfunc``.


DESCRIPTION
-----------

Register to receive standard output and/or standard error that the host
environment forwards from the specified remote processes. The returned
reference identifier must be retained by the caller; it is the value passed
to :ref:`PMIx_IOF_deregister(3) <man3-PMIx_IOF_deregister>` to cancel the
request.

The forwarding request is thread-shifted onto the PMIx progress thread for
processing. Whether the call blocks depends on ``regcbfunc``:

* When ``regcbfunc`` is non-``NULL``, the call is *non-blocking*. On a
  successful submission the function returns ``PMIX_SUCCESS`` and the
  registration result |mdash| including the assigned reference identifier
  |mdash| is delivered later through ``regcbfunc``. If the function returns
  an error, the request could not be submitted and ``regcbfunc`` will
  **not** be called.
* When ``regcbfunc`` is ``NULL``, the call is *blocking*: it does not return
  until registration has been processed.

As with all non-blocking PMIx APIs, callers **must** keep the ``procs`` and
``directives`` arrays valid until the registration callback has fired.


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

* ``PMIX_IOF_BUFFERING_SIZE`` (uint32_t) |mdash| control the grouping of
  output on the specified channel(s) so that data is delivered in blocks of
  the requested size rather than as it arrives.
* ``PMIX_IOF_BUFFERING_TIME`` (uint32_t) |mdash| maximum time, in seconds, to
  buffer output before delivering it, used in conjunction with the buffering
  size.
* ``PMIX_IOF_TAG_OUTPUT`` (bool) |mdash| tag each line of output with the
  identity (namespace/rank) and channel from which it originated.
* ``PMIX_IOF_TIMESTAMP_OUTPUT`` (bool) |mdash| timestamp the output.
* ``PMIX_IOF_XML_OUTPUT`` (bool) |mdash| format the output in XML.


CALLBACK FUNCTION
-----------------

When forwarded output becomes available |mdash| or a specified buffering
size and/or time has been reached |mdash| PMIx invokes the delivery callback
``cbfunc``:

.. code-block:: c

   typedef void (*pmix_iof_cbfunc_t)(size_t iofhdlr,
                                     pmix_iof_channel_t channel,
                                     pmix_proc_t *source,
                                     pmix_byte_object_t *payload,
                                     pmix_info_t info[], size_t ninfo);

The parameters carry:

* ``iofhdlr`` |mdash| the reference identifier of the handler being invoked,
  matching the value returned by the registration.
* ``channel`` |mdash| a :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`
  bitmask identifying the channel on which the data arrived.
* ``source`` |mdash| the namespace/rank of the process that generated the
  data.
* ``payload`` |mdash| a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  containing the data. Note that multiple strings may be present, and the
  data is **not** guaranteed to be ``NULL``-terminated.
* ``info`` / ``ninfo`` |mdash| an optional array of metadata provided by the
  source describing the payload; for example, it may include
  ``PMIX_IOF_COMPLETE`` to indicate that the channel has been closed.


RETURN VALUE
------------

For the non-blocking form (``regcbfunc`` non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status and the assigned reference identifier are
delivered to ``regcbfunc``. Any negative return means the request was not
submitted and ``regcbfunc`` will not be called.

For the blocking form (``regcbfunc`` is ``NULL``), a return of
``PMIX_OPERATION_SUCCEEDED`` indicates that registration completed
successfully; a negative value is an error constant.

Error constants that may be returned include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced
  because the library's progress engine has been stopped.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| ``PMIX_FWD_STDIN_CHANNEL`` was included
  in ``channel``; standard input is not supported on this path.
* ``PMIX_ERR_UNREACH`` |mdash| the request must be sent to a server, but the
  tool is not connected to one.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for
  the request.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

Standard input is never delivered through this path; it is always delivered
to the ``stdin`` file descriptor. To forward standard input to a set of
processes, use :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`.

Use of ``PMIX_RANK_WILDCARD`` to request the output of all processes in a
namespace is supported but should be used with care due to the bandwidth and
memory footprint it can incur.


.. seealso::
   :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`,
   :ref:`PMIx_IOF_deregister(3) <man3-PMIx_IOF_deregister>`,
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
