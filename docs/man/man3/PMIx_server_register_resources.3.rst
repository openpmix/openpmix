.. _man3-PMIx_server_register_resources:

PMIx_server_register_resources
==============================

.. include_body

``PMIx_server_register_resources`` |mdash| Register non-namespace-related
information with the local PMIx server library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_register_resources(pmix_info_t info[], size_t ninfo,
                                                pmix_op_cbfunc_t cbfunc,
                                                void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # directives is a list of pmix_info_t dictionaries describing the
  # resources to register (this binding is blocking)
  directives = [{'key': PMIX_CLUSTER_ID,
                 'value': {'value': "cluster-a", 'val_type': PMIX_STRING}}]
  rc = foo.register_resources(directives)


INPUT PARAMETERS
----------------

* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures carrying
  the information to be registered (see `DIRECTIVES`_).
* ``ninfo``: Number of elements in the ``info`` array.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked when the
  operation completes. A ``NULL`` value makes the call **blocking** (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Register information about resources that are **not** associated with any
particular namespace with the local PMIx server library, for distribution to
local client processes. This includes information about the physical cluster
(such as its identifier, hostname, or resource-manager name), fabric devices,
GPUs, and similar resources. All information provided through this API is made
available to every job as part of its job-level information. Duplicate
information provided through
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>` overrides
what was registered here, but only for that specific namespace.

This API can also be used to store data collected during operations that have no
natural namespace callback |mdash| for example, the group and endpoint
information returned during a
:ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>` bootstrap, where a
local server may host only "add-member" participants and thus has no registered
callback in which to store the returned endpoint, group, and job data.

Information registered as non-namespace resources is considered **static** and is
provided here as a logical alternative to including it in every
``PMIx_server_register_nspace`` call |mdash| useful for code clarity and to avoid
repeatedly registering the same static resource information where a single server
instance supports multiple jobs.

``PMIx_server_register_resources`` supports both calling conventions. When
``cbfunc`` is non-``NULL`` the function is **non-blocking**: the request is
thread-shifted onto the progress thread, the function returns ``PMIX_SUCCESS``,
and ``cbfunc`` is invoked with the completion status. When ``cbfunc`` is ``NULL``
the function is **blocking**: it does not return until the operation completes,
returning ``PMIX_OPERATION_SUCCEEDED`` on success.

As with all non-blocking PMIx APIs, when a callback is supplied the caller
**must** keep the ``info`` array valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

Any attribute conveying non-namespace resource information may be registered; the
values are cached and returned to jobs as job-level data. Commonly used
attributes include:

* ``PMIX_CLUSTER_ID`` (char*) |mdash| a string name for the cluster this
  allocation is on.
* ``PMIX_RM_NAME`` (char*) |mdash| the name of the resource manager.
* ``PMIX_RM_VERSION`` (char*) |mdash| the resource-manager version.
* ``PMIX_SERVER_HOSTNAME`` (char*) |mdash| the host where the target server is
  located.
* ``PMIX_NODE_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| an array describing a
  node and the resources it hosts (for example, its ``PMIX_HOSTNAME``,
  ``PMIX_HOSTNAME_ALIASES``, ``PMIX_NODEID``, ``PMIX_AVAIL_PHYS_MEMORY``, and
  fabric-device descriptions).

In addition, this API is used internally to convey group bootstrap data via the
following attributes, which receive special handling:

* ``PMIX_GROUP_INFO_ARRAY`` / ``PMIX_GROUP_INFO`` (pmix_data_array_t\*) |mdash|
  group information to be stored in the server's hash table. Requires a
  ``PMIX_GROUP_CONTEXT_ID`` to also be provided.
* ``PMIX_GROUP_ENDPT_DATA`` (pmix_data_array_t\*) |mdash| per-process endpoint
  information to be stored for the group.
* ``PMIX_GROUP_CONTEXT_ID`` (size_t) |mdash| the context identifier associated
  with the group information being stored.
* ``PMIX_GROUP_JOB_INFO`` (pmix_byte_object_t) |mdash| packed job-level
  information associated with the group.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

It is invoked with the completion ``status`` of the registration and the
``cbdata`` originally passed to ``PMIx_server_register_resources``.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` non-``NULL``), a return of ``PMIX_SUCCESS``
indicates only that the request was accepted for processing; the final status is
delivered to ``cbfunc``.

For the blocking form (``cbfunc`` is ``NULL``), a return of
``PMIX_OPERATION_SUCCEEDED`` indicates that the registration completed
successfully and no callback is invoked. Other returns include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied (for example,
  group information provided without an accompanying context identifier).

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This is a server-role API, available only after
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. Because the registered
information is static, there is no requirement to deregister it before finalizing
the library; the library cleans it up as part of normal finalization. Use
:ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`
only when client processes should no longer have access to the information.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
