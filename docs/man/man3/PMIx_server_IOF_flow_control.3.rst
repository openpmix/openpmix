.. _man3-PMIx_server_IOF_flow_control:

PMIx_server_IOF_flow_control
============================

.. include_body

``PMIx_server_IOF_flow_control`` |mdash| Suspend or resume the processes
feeding ``stdin`` to this PMIx server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_IOF_flow_control(const pmix_proc_t *source,
                                              pmix_iof_channel_t channel,
                                              bool xoff,
                                              const pmix_info_t directives[], size_t ndirs,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  pydirs = []

  # stop every process feeding us stdin
  rc = foo.iof_flow_control(None, PMIX_FWD_STDIN_CHANNEL, True, pydirs)

  # ... and later, let them resume
  rc = foo.iof_flow_control(None, PMIX_FWD_STDIN_CHANNEL, False, pydirs)

  # or name a single producer
  source = {'nspace': "mytool", 'rank': 0}
  rc = foo.iof_flow_control(source, PMIX_FWD_STDIN_CHANNEL, True, pydirs)

The Python binding is the **blocking** form |mdash| it passes no callback,
and returns the status directly. A ``source`` of ``None`` is the Python
spelling of a ``NULL`` ``source``, and means every process feeding this
server.


INPUT PARAMETERS
----------------

* ``source``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` identifying
  the process whose ``stdin`` flow is to be controlled. A ``NULL`` value, or a
  source carrying a wildcard rank and/or namespace, applies the request to
  every process feeding ``stdin`` to this server.
* ``channel``: The :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`
  identifying the channel to control. Only ``PMIX_FWD_STDIN_CHANNEL`` can be
  flow-controlled.
* ``xoff``: ``true`` to suspend the stream, ``false`` to resume it.
* ``directives``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  qualifying the request. A ``NULL`` value (with ``ndirs`` of zero) is
  supported when no qualifiers are provided.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type
  :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked once the request has
  been applied. A ``NULL`` value makes the call blocking.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

A host environment that finds itself falling behind on the ``stdin`` it is
being handed through its ``push_stdin`` upcall has, without this function, only
two options: drop bytes, or queue them without bound. ``PMIx_server_IOF_flow_control``
provides the third |mdash| reach back to whoever is producing the data and stop
them at the source.

The library applies the request to both kinds of producer it can reach:

* any ``stdin`` the library is reading on this process's own behalf |mdash| the
  read is left un-armed while the stream is suspended; and
* every tool that has pushed ``stdin`` to this server |mdash| each is sent the
  request, and a tool that is itself a server relays it onward to its own
  producers. A chain of launchers therefore carries the request all the way
  back to the process actually holding the input stream.

Nothing is buffered by PMIx on behalf of a suspended stream, and nothing is
lost. The bytes that would have been read simply stay in the producer's input
stream, where the operating system applies the back-pressure. An XOFF is
therefore never permission to drop data.

A suspension persists until the function is called again with ``xoff`` set to
``false``. **Every XOFF must eventually be paired with an XON**, or the stream
stalls for the life of the producer.

``PMIx_server_IOF_flow_control`` supports both blocking and non-blocking
operation. When ``cbfunc`` is non-``NULL`` the call is **non-blocking**: it
thread-shifts the request into the library's progress thread, returns
immediately, and invokes ``cbfunc`` when the request has been applied. When
``cbfunc`` is ``NULL`` the call is **blocking**: it does not return until the
request has been applied.

The host is required to retain the ``directives`` array until the callback is
executed |mdash| or, in the blocking case, until the function returns.


SUSPENDING WITHOUT CALLING THIS FUNCTION
----------------------------------------

A host that would rather not track its producers can suspend a stream
opportunistically instead, by completing a ``push_stdin`` upcall with
``PMIX_ERR_IOF_XOFF`` in place of ``PMIX_SUCCESS``. That status means *"I have
taken this data, now stop sending"* |mdash| the data it accompanies is
delivered, no error is reported to the producer, and the producing stream is
suspended exactly as it would be by an ``xoff`` of ``true`` here.

``PMIX_ERR_IOF_XOFF`` is deliberately **not** a failure: no data has been lost
and the stream has not been closed, so a tool whose ``stdin`` forwarding is
suspended this way is not notified and does not see a
``PMIX_ERR_IOF_FAILURE`` event.

Resumption is only ever through this function |mdash| there is no status that
means "resume".


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` is non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status is delivered through ``cbfunc``. For the blocking form
(``cbfunc`` is ``NULL``), a return of ``PMIX_OPERATION_SUCCEEDED`` indicates
that the request was applied successfully |mdash| ``cbfunc`` is not called.
Possible return values include:

* ``PMIX_SUCCESS`` |mdash| (non-blocking form) the request was accepted for
  processing.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (blocking form) the request was applied.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| ``channel`` did not include
  ``PMIX_FWD_STDIN_CHANNEL``. ``stdin`` is the only stream whose producer this
  library can reach; output flows the other way, from processes the library
  does not control.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  request.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Naming a ``source`` that is not currently producing ``stdin`` is not an error;
the request simply reaches nobody. Likewise, an XON for a stream that was never
suspended is a no-op.

A producer running a PMIx release that predates flow control is never sent a
request |mdash| it keeps producing, exactly as it did before this function
existed. A host can detect support at build time through the
``PMIX_CAP_IOF_FLOW_CONTROL`` capability flag in ``pmix_version.h``.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_IOF_deliver(3) <man3-PMIx_server_IOF_deliver>`,
   :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`,
   :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_iof_channel_t(5) <man5-pmix_iof_channel_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
