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
to do). If the library was never initialized, the callback (if provided) is
invoked with ``PMIX_ERR_INIT``; if memory for the request could not be
allocated, it is invoked with ``PMIX_ERR_NOMEM``. ``cbdata`` is the opaque
pointer passed to ``PMIx_server_deregister_client``.


NOTES
-----

If the PMIx server library's progress engine has already been stopped (for
example, during finalize), the request is silently dropped and any provided
``cbfunc`` is not invoked.


.. seealso::
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
   :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
