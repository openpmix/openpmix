.. _man3-PMIx_server_deregister_session:

PMIx_server_deregister_session
==============================

.. include_body

``PMIx_server_deregister_session`` |mdash| Deregister a session and
release the information held for it.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   void PMIx_server_deregister_session(uint32_t sessionID,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata);


INPUT PARAMETERS
----------------

* ``sessionID``: The identifier of the session to deregister.
  ``UINT32_MAX`` is not a valid session identifier.
* ``cbfunc``: Callback function of type
  :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked when the
  deregistration completes. A ``NULL`` value makes the call *blocking*
  (see `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Tell the PMIx server library that a session has ended, so that it can
release the information it holds for it. This is the session-level
counterpart of
:ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`.

The host must say so: the library cannot infer that a session is over
when its last job is deregistered, since a session with no job running
is an ordinary state and another job may be about to start in it.

**Jobs in the session are not deregistered by this call.** The host
deregisters them with
:ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
before or after this call. A job still registered keeps the session's
information available to its processes until it is deregistered, so
the two may be done in either order.

Deregistering a session this server never knew is not an error. A host
may deregister a session on every node, including nodes where none of
its jobs ran.

The call thread-shifts the request onto the library's progress thread.
It supports a non-blocking and a blocking mode, selected by ``cbfunc``:

* When ``cbfunc`` is **non-**\ ``NULL``, the call is *non-blocking*:
  it returns at once, and ``cbfunc`` is invoked when the session has
  been deregistered.
* When ``cbfunc`` is ``NULL``, the call is *blocking*: it returns when
  the deregistration is complete.

Because the function returns ``void``, a caller that needs to know the
outcome must supply a ``cbfunc`` and inspect the status delivered to
it.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature
``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

``status`` is ``PMIX_SUCCESS`` on success, or a negative PMIx error
constant otherwise. In particular, the callback is invoked with
``PMIX_ERR_INIT`` if the PMIx server library is not initialized, with
``PMIX_ERR_NOT_AVAILABLE`` if its progress engine has already been
stopped, with ``PMIX_ERR_BAD_PARAM`` if ``sessionID`` is
``UINT32_MAX``, and with ``PMIX_ERR_NOMEM`` if the request could not be
allocated.


.. include:: /man/no-blocking-in-progress-thread.rst


**Exception for this call.** ``PMIx_server_deregister_session`` has
no return value, so it cannot report ``PMIX_ERR_WOULD_BLOCK``. When it
is passed a ``NULL`` ``cbfunc`` from the progress thread it completes
the deregistration *asynchronously* on that same loop. Every other
PMIx operation is serialized through that loop, so each sees the
session gone exactly as it would have.


.. seealso::
   :ref:`PMIx_server_register_session(3) <man3-PMIx_server_register_session>`,
   :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
