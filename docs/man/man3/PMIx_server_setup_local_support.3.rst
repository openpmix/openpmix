.. _man3-PMIx_server_setup_local_support:

PMIx_server_setup_local_support
===============================

.. include_body

``PMIx_server_setup_local_support`` |mdash| Perform application-specific
local setup operations prior to spawning local clients of a namespace.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_setup_local_support(const pmix_nspace_t nspace,
                                                 pmix_info_t info[], size_t ninfo,
                                                 pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # ilist is the list of pmix_info_t dictionaries returned by the
  # earlier setup_application call and distributed by the host
  rc = foo.setup_local_support("myapp", ilist)


INPUT PARAMETERS
----------------

* ``nspace``: The namespace whose local support is being set up. May be ``NULL``
  if the operation is not scoped to a specific namespace.
* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures. This is
  the data returned to the host by the callback of the corresponding
  :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`
  call and distributed to this node. May be ``NULL`` with ``ninfo`` of zero.
* ``ninfo``: Number of elements in the ``info`` array.
* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked when the
  operation completes. A ``NULL`` value makes the call **blocking** (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Provide a function by which the local PMIx server can perform any
application-specific operations required before spawning local clients of a given
namespace. For example, a fabric library might need to program the local driver
for "instant on" addressing. The ``info`` array delivered here is the data that
was produced on some node by
:ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>` and
then distributed by the host environment to every node that will run the
application's processes.

Data provided in the ``info`` array is stored in the job-information region for
the namespace. Operations implied by that data are **cached** until the server
first calls
:ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>` for the namespace,
thereby indicating that a local client of the namespace is actually about to be
started. Such operations are executed **only once** per namespace |mdash| for the
first local client |mdash| not once per client.

Internally the request is thread-shifted onto the progress thread, where the
network (``pnet``) subsystem is given the opportunity to set up the local network
support. The completion status of that subsystem operation is delivered as the
result.

``PMIx_server_setup_local_support`` supports both calling conventions. When
``cbfunc`` is non-``NULL`` the function is **non-blocking**: it returns
immediately, and ``cbfunc`` is invoked with the completion status. When
``cbfunc`` is ``NULL`` the function is **blocking**: it does not return until the
operation completes and the result is carried by the return value (see `RETURN
VALUE`_).

The host environment is **required** to execute this operation prior to starting
any local application process of the namespace if setup data was obtained from a
call to ``PMIx_server_setup_application``. The host must also have registered the
namespace with
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>` before
calling this API, so that all namespace-related information the library needs is
already available.

As with all non-blocking PMIx APIs, when a callback is supplied the caller
**must** keep the ``info`` array valid until ``cbfunc`` is invoked.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

It is invoked with the completion ``status`` of the local-support operation and
the ``cbdata`` originally passed to ``PMIx_server_setup_local_support``.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` non-``NULL``), a return of ``PMIX_SUCCESS``
indicates only that the request was accepted for processing; the final status is
delivered to ``cbfunc``.

For the blocking form (``cbfunc`` is ``NULL``), a return of
``PMIX_OPERATION_SUCCEEDED`` indicates that the operation completed successfully
and no callback is (or was) invoked. Other returns include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  request.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This is a server-role API, available only after
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. Because the implied
operations are executed only for the first local client of a namespace, calling
this API has no additional effect once
:ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>` has already fired
those operations for that namespace.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`,
   :ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
