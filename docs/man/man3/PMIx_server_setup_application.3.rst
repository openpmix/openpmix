.. _man3-PMIx_server_setup_application:

PMIx_server_setup_application
=============================

.. include_body

``PMIx_server_setup_application`` |mdash| Obtain application-specific setup
data (environment, fabric configuration, security credentials) prior to
launching a job.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_setup_application(const pmix_nspace_t nspace,
                                               pmix_info_t info[], size_t ninfo,
                                               pmix_setup_application_cbfunc_t cbfunc,
                                               void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # dicts is a list of pmix_info_t dictionaries describing the job
  # (must include the process and node maps)
  dicts = [{'key': PMIX_SETUP_APP_ALL,
            'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc, dataout = foo.setup_application("myapp", dicts)
  # dataout is a list of pmix_info_t dictionaries to be distributed
  # with the application


INPUT PARAMETERS
----------------

* ``nspace``: The namespace of the job being set up. May be ``NULL`` if the
  request is not scoped to a specific namespace.
* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  describing the job and directing what setup data is desired (see `DIRECTIVES`_).
  The array **must** include a description of the job via the ``PMIX_NODE_MAP`` and
  ``PMIX_PROC_MAP`` attributes.
* ``ninfo``: Number of elements in the ``info`` array.
* ``cbfunc``: Callback function of type :ref:`pmix_setup_application_cbfunc_t <man5-pmix_setup_application_cbfunc_t>` invoked
  when the setup data is ready (see `CALLBACK FUNCTION`_).
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Provide a function by which the host environment (typically a launcher or
resource manager) can request application-specific setup data from the
supporting PMIx server-library subsystems prior to initiating launch of a job.
For example, network libraries may opt to allocate fabric resources and provide
security credentials for the application, and programming-model support may
harvest relevant environment variables.

``PMIx_server_setup_application`` is a **non-blocking** operation |mdash| it is
defined this way because a contributing subsystem may need to perform a
potentially time-consuming action (such as querying a remote fabric-manager
service) before it can respond. Internally the request is thread-shifted onto the
progress thread, where the network (``pnet``) and programming-model (``pmdl``)
subsystems are given the opportunity to contribute. Whatever data they return is
assembled into a ``pmix_info_t`` array and handed to ``cbfunc``.

The data returned through the callback must be distributed by the host
environment and subsequently delivered, via
:ref:`PMIx_server_setup_local_support(3) <man3-PMIx_server_setup_local_support>`,
to the local PMIx server on each node where the application's processes will
execute, prior to spawning those processes.

The function may be called on a per-application basis if the ``PMIX_PROC_MAP`` and
``PMIX_NODE_MAP`` describe only the corresponding application rather than the
entire job.

As with all non-blocking PMIx APIs, the caller **must** keep the ``info`` array
valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The following attributes are relevant to this operation. PMIx libraries that
support this operation are **required** to support:

* ``PMIX_SETUP_APP_ENVARS`` (bool) |mdash| harvest and include relevant
  environment variables.
* ``PMIX_SETUP_APP_NONENVARS`` (bool) |mdash| include all relevant data other than
  environment variables.
* ``PMIX_SETUP_APP_ALL`` (bool) |mdash| include all relevant data.
* ``PMIX_ALLOC_FABRIC`` (pmix_data_array_t\*) |mdash| request fabric allocations,
  described by the accompanying fabric attributes below.
* ``PMIX_ALLOC_FABRIC_ID`` (char*) |mdash| a string identifier for the requested
  fabric allocation.
* ``PMIX_ALLOC_FABRIC_SEC_KEY`` (bool) |mdash| a fabric security key is requested.
* ``PMIX_ALLOC_FABRIC_TYPE`` (char*) |mdash| type of desired fabric transport.
* ``PMIX_ALLOC_FABRIC_PLANE`` (char*) |mdash| identifier of the fabric plane to be
  used for the allocation.
* ``PMIX_ALLOC_FABRIC_ENDPTS`` (size_t) |mdash| number of endpoints to allocate
  per process.
* ``PMIX_ALLOC_FABRIC_ENDPTS_NODE`` (size_t) |mdash| number of endpoints to
  allocate per node.
* ``PMIX_PROC_MAP`` (char*) |mdash| regular expression describing the process
  placement (ranks to nodes) for the job or application.
* ``PMIX_NODE_MAP`` (char*) |mdash| regular expression describing the nodes
  involved in the job or application.

PMIx libraries **may** additionally support:

* ``PMIX_ALLOC_BANDWIDTH`` (float) |mdash| fabric bandwidth (in Mbits/sec) being
  requested.
* ``PMIX_ALLOC_FABRIC_QOS`` (char*) |mdash| fabric quality-of-service level being
  requested.
* ``PMIX_SESSION_INFO`` (bool) |mdash| in this context, indicates that the
  ``PMIX_NODE_MAP`` describes the entire session rather than just this namespace,
  allowing subsequent calls to omit node-level information.

The following optional attributes identify the programming model being executed,
which the library may use to harvest or forward model-specific environment
variables:

* ``PMIX_PROGRAMMING_MODEL`` (char*) |mdash| the programming model (e.g., "MPI").
* ``PMIX_MODEL_LIBRARY_NAME`` (char*) |mdash| the model library name.
* ``PMIX_MODEL_LIBRARY_VERSION`` (char*) |mdash| the model library version.


CALLBACK FUNCTION
-----------------

The ``cbfunc`` has the signature ``pmix_setup_application_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_setup_application_cbfunc_t)(pmix_status_t status,
                                                   pmix_info_t info[], size_t ninfo,
                                                   void *provided_cbdata,
                                                   pmix_op_cbfunc_t cbfunc, void *cbdata);

When the setup data is ready, PMIx invokes ``cbfunc`` with the completion
``status``, the ``info`` array of setup data to be distributed with the
application, and ``provided_cbdata`` |mdash| the ``cbdata`` originally passed to
``PMIx_server_setup_application``.

The returned ``info`` array is **owned by the PMIx server library** and remains
valid only until the recipient calls the ``cbfunc`` (of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>`)
that is passed along with it; the library frees the array at that point.
Accordingly, the host must copy any data it needs to retain and then invoke that
completion function so the library can release its memory. If no data is returned,
the ``info`` array is ``NULL``, its count is zero, and the trailing ``cbfunc`` may
be ``NULL``.


RETURN VALUE
------------

A return of ``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status and the setup data are delivered to ``cbfunc``. An
immediate error return means the request was not accepted and ``cbfunc`` will not
be invoked. Possible immediate errors include:

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
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. Support for harvesting
environment variables and providing local configuration information is optional
per the Standard; a conforming library may return no data.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_setup_local_support(3) <man3-PMIx_server_setup_local_support>`,
   :ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
