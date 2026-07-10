.. _man3-PMIx_server_deregister_nspace:

PMIx_server_deregister_nspace
=============================

.. include_body

``PMIx_server_deregister_nspace`` |mdash| Deregister a namespace and purge all
data relating to it.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   void PMIx_server_deregister_nspace(const pmix_nspace_t nspace,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after registering the namespace ...
  foo.deregister_nspace("myjob")


INPUT PARAMETERS
----------------

* ``nspace``: The namespace (a character array of maximum length
  ``PMIX_MAX_NSLEN``) to deregister.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked when the
  deregistration completes. A ``NULL`` value makes the call *blocking* (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Deregister the specified namespace and purge all objects relating to it,
including any client information for processes in that namespace. This is
intended to support persistent PMIx servers by giving the host resource manager
an opportunity to tell the PMIx server library to release all memory associated
with a completed job.

Like :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
this call thread-shifts the request onto the library's internal progress
thread, where the purge is actually performed. It supports both a non-blocking
and a blocking mode, selected by the ``cbfunc`` argument:

* When ``cbfunc`` is **non-**\ ``NULL``, the call is *non-blocking*: the
  request is posted to the progress thread and the function returns
  immediately, with ``cbfunc`` invoked once the namespace has been purged. The
  caller need not retain any data across the call.
* When ``cbfunc`` is ``NULL``, the call is *blocking*: the function does not
  return until the deregistration is complete.

Because the function returns ``void``, callers that need to know the outcome
must supply a ``cbfunc`` and inspect the status delivered to it.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes ``cbfunc`` from its progress thread once the namespace has
been purged. ``status`` is ``PMIX_SUCCESS`` on success, or a negative PMIx
error constant otherwise. In particular, if the library was never initialized
the callback (if provided) is invoked with ``PMIX_ERR_INIT``. ``cbdata`` is the
opaque pointer passed to ``PMIx_server_deregister_nspace``.


NOTES
-----

Deregistering a namespace automatically deletes all client information for that
namespace |mdash| there is no need to individually deregister its clients
first. Use
:ref:`PMIx_server_deregister_client(3) <man3-PMIx_server_deregister_client>`
only for exception cases where a single client must be removed while the
namespace remains active.

If the PMIx server library's progress engine has already been stopped (for
example, during finalize), the request is silently dropped and any provided
``cbfunc`` is not invoked.


.. seealso::
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_deregister_client(3) <man3-PMIx_server_deregister_client>`,
   :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
