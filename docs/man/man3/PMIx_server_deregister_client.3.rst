.. _man3-PMIx_server_deregister_client:

PMIx_server_deregister_client
=============================

.. include_body

``PMIx_server_deregister_client`` |mdash| Deregister a local client and purge
all data relating to it.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   void PMIx_server_deregister_client(const pmix_proc_t *proc,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after registering the client ...
  foo.deregister_client({'nspace': "myjob", 'rank': 0})


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure
  identifying the client by namespace and rank.
* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked when the
  deregistration completes. A ``NULL`` value makes the call *blocking* (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Deregister the specified client and purge all data relating to it from the PMIx
server library.

This API is intended primarily for *exception cases*. In normal operation,
:ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`
deletes all client information for a namespace, and the PMIx server library
automatically performs that cleanup once all local clients of a namespace have
disconnected. ``PMIx_server_deregister_client`` is therefore needed only when a
single client must be removed while its namespace remains active |mdash| though
it may be called in non-exception cases if desired.

Like other server registration APIs, this call thread-shifts the request onto
the library's internal progress thread and supports both a non-blocking and a
blocking mode, selected by the ``cbfunc`` argument:

* When ``cbfunc`` is **non-**\ ``NULL``, the call is *non-blocking*: the
  request is posted to the progress thread and the function returns
  immediately, with ``cbfunc`` invoked once the client has been removed.
* When ``cbfunc`` is ``NULL``, the call is *blocking*: the function does not
  return until the deregistration is complete.

Because the function returns ``void``, callers that need to know the outcome
must supply a ``cbfunc`` and inspect the status delivered to it.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes ``cbfunc`` from its progress thread once the client has
been purged. ``status`` is ``PMIX_SUCCESS`` on success (including the case in
which the named client or namespace was not found, which is treated as nothing
to do). The callback (if provided) is instead invoked with ``PMIX_ERR_INIT`` if
the PMIx server library was never initialized, with ``PMIX_ERR_NOT_AVAILABLE``
if its progress engine has already been stopped, with ``PMIX_ERR_BAD_PARAM`` if
``proc`` is ``NULL`` or names an empty namespace, and with ``PMIX_ERR_NOMEM`` if
memory for the request could not be allocated. ``cbdata`` is the opaque
pointer passed to ``PMIx_server_deregister_client``.


NOTES
-----

If the PMIx server library's progress engine has already been stopped (for
example, during finalize), the request cannot be serviced; any provided
``cbfunc`` is invoked with ``PMIX_ERR_NOT_AVAILABLE``. This function reports no
status of its own, so the callback is the only way a caller can be told, and a
caller that was given one and never heard back would wait forever.


.. include:: /man/no-blocking-in-progress-thread.rst


**Exception for this call.** ``PMIx_server_deregister_client`` has no return
value, so it cannot report ``PMIX_ERR_WOULD_BLOCK``. When it is passed a
``NULL`` ``cbfunc`` from the progress thread it therefore completes the
deregistration *asynchronously* on that same loop rather than refusing it.
Nothing observable is lost: every other PMIx operation is serialized through
that loop, so each will see the client gone exactly as it would have. Running the
handler inline is not an option |mdash| it releases peers, and the up-call the
caller is nested inside may still be holding one.


.. seealso::
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
   :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
