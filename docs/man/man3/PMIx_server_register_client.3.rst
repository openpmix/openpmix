.. _man3-PMIx_server_register_client:

PMIx_server_register_client
===========================

.. include_body

``PMIx_server_register_client`` |mdash| Register a local client process with
the PMIx server library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_register_client(const pmix_proc_t *proc,
                                             uid_t uid, gid_t gid,
                                             void *server_object,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after registering the namespace ...
  rc = foo.register_client({'nspace': "myjob", 'rank': 0}, 1000, 1000)


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure
  identifying the client by namespace and rank.
* ``uid``: The expected user ID of the child process.
* ``gid``: The expected group ID of the child process.
* ``server_object``: An optional opaque pointer the host wishes to associate
  with this client. If provided, the library returns it to the host whenever a
  server-module function is invoked in relation to this specific process,
  letting the host access its tracking object without a namespace/rank lookup.
  May be ``NULL``.
* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked when the
  registration completes. A ``NULL`` value makes the call *blocking* (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Register a local client process with the PMIx server library. The host resource
manager is required to execute this operation *prior to starting the client
process*, so that the library is prepared for the connection the client will
make when it calls :ref:`PMIx_Init(3) <man3-PMIx_Init>`.

The expected ``uid`` and ``gid`` allow the server library to authenticate a
client as it connects by requiring the detected user and group IDs of the
connecting process to match the registered values. Because this check is
performed at registration time, the detected user and group IDs are not passed
to the host again in the ``pmix_server_client_connected2_fn_t`` server module
function.

The optional ``server_object`` provides a convenience for the host: any object
passed here is handed back to the host in subsequent server-module callbacks
that pertain to this client (for example, ``client_connected2`` or
``client_finalized``), allowing the host to track per-client state without an
internal lookup.

Like other server registration APIs, this call thread-shifts the request onto
the library's internal progress thread and supports both a non-blocking and a
blocking mode, selected by the ``cbfunc`` argument:

* When ``cbfunc`` is **non-**\ ``NULL``, the call is *non-blocking*: the
  request is posted to the progress thread and the function returns
  ``PMIX_SUCCESS`` immediately, with ``cbfunc`` invoked once the client has
  been registered.
* When ``cbfunc`` is ``NULL``, the call is *blocking*: the function does not
  return until registration is complete, and on success returns
  ``PMIX_OPERATION_SUCCEEDED``.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes ``cbfunc`` from its progress thread once the client has
been registered. ``status`` is ``PMIX_SUCCESS`` on success, or a negative PMIx
error constant otherwise. ``cbdata`` is the opaque pointer passed to
``PMIx_server_register_client``.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` is non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status is delivered to ``cbfunc``.

For the blocking form (``cbfunc`` is ``NULL``), the return value carries the
result directly:

* ``PMIX_OPERATION_SUCCEEDED`` |mdash| the client was registered successfully
  inline; no callback is or will be invoked.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  registration.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

There is no requirement that the namespace be registered before its clients.
If the namespace named in ``proc`` has not yet been registered, the library
creates a placeholder namespace and appends this client to it, in anticipation
of an eventual
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>` call.
After registering the client, the host must call ``PMIx_server_setup_fork`` to
inject the environment the client needs to connect back to this server.

For security, PMIx server implementations should verify the user and group IDs
of a connecting process against those registered here before completing the
connection.


.. seealso::
   :ref:`PMIx_server_deregister_client(3) <man3-PMIx_server_deregister_client>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
