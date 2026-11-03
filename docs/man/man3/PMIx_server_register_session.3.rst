.. _man3-PMIx_server_register_session:

PMIx_server_register_session
============================

.. include_body

``PMIx_server_register_session`` |mdash| Register a session and its
session-level information with the PMIx server library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_register_session(uint32_t sessionID,
                                              pmix_info_t info[], size_t ninfo,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata);


INPUT PARAMETERS
----------------

* ``sessionID``: The session's identifier, as the workload manager
  assigned it. ``UINT32_MAX`` is not a valid session identifier.
* ``info``: Array of :ref:`pmix_info_t <man5-pmix_info_t>` structures
  describing the session - the session-realm information its jobs'
  processes can retrieve with
  :ref:`PMIx_Get(3) <man3-PMIx_Get>`, such as ``PMIX_UNIV_SIZE`` and
  ``PMIX_MAX_PROCS`` (see the session-realm list in
  :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`).
  May be ``NULL``.
* ``ninfo``: Number of elements in ``info``.
* ``cbfunc``: Callback function of type
  :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked when the
  registration completes. A ``NULL`` value makes the call *blocking*
  (see `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Establish a session with the PMIx server library, and record its
description.

A *session* is a set of resources the workload manager has assigned
to one or more users. Jobs run within it and come and go; the session
persists. A host can also establish a session by including a
``PMIX_SESSION_INFO_ARRAY`` in a job's registration
(:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`):
a job that names a session the library does not know establishes it.
That still works. This call says so directly, and is the way to:

* describe a session before its first job is registered;
* keep a session after its last job is deregistered. A session
  established only by its jobs has no existence apart from them.

Registering a session with no ``info`` still establishes it: a job
registered later in that session finds this one rather than creating
another.

**Registering a session that is already known updates it.** The keys
the new registration gives replace the ones the library holds; keys it
does not mention are kept. A session's resources change while it runs
(it is extended, or a node is lost), and this is how the host says so.
Clients that are already running are told of the change, and read the
new values on their next ``PMIx_Get``. A registration that restates
what the library already holds changes nothing.

The call thread-shifts the request onto the library's progress thread.
It supports a non-blocking and a blocking mode, selected by ``cbfunc``:

* When ``cbfunc`` is **non-**\ ``NULL``, the call is *non-blocking*:
  it returns ``PMIX_SUCCESS`` at once, and ``cbfunc`` is invoked when
  the session has been registered. The caller must keep ``info`` valid
  until then.
* When ``cbfunc`` is ``NULL``, the call is *blocking*: it returns when
  the registration is complete, with its result.


RETURN VALUE
------------

* ``PMIX_SUCCESS`` |mdash| for a non-blocking call, the request was
  accepted and the result will be delivered to ``cbfunc``; for a
  blocking call, the session was registered.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library is not initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the library's progress engine has
  already been stopped.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``sessionID`` is ``UINT32_MAX``.
* ``PMIX_ERR_NOMEM`` |mdash| the request could not be allocated.

Other negative values report a failure to record the session's
information.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature
``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes it from its progress thread once the session has
been registered. ``status`` is ``PMIX_SUCCESS`` on success, or a
negative PMIx error constant otherwise.


NOTES
-----

A session registered here is removed only by
:ref:`PMIx_server_deregister_session(3) <man3-PMIx_server_deregister_session>`.
The library cannot tell when a session has ended: a session with no
job running is an ordinary state.


.. include:: /man/no-blocking-in-progress-thread.rst


**Exception for this call.** When ``PMIx_server_register_session`` is
passed a ``NULL`` ``cbfunc`` from the progress thread - from inside one
of the host's up-calls - it does not return ``PMIX_ERR_WOULD_BLOCK``.
It completes the registration *asynchronously* on that same loop and
returns ``PMIX_SUCCESS``. Every other PMIx operation is serialized
through that loop, so each sees the session registered exactly as it
would have.


.. seealso::
   :ref:`PMIx_server_deregister_session(3) <man3-PMIx_server_deregister_session>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
