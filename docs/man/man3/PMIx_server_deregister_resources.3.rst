.. _man3-PMIx_server_deregister_resources:

PMIx_server_deregister_resources
================================

.. include_body

``PMIx_server_deregister_resources`` |mdash| Remove non-namespace-related
information from the local PMIx server library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_deregister_resources(pmix_info_t info[], size_t ninfo,
                                                  pmix_op_cbfunc_t cbfunc,
                                                  void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # only the keys of the directives are significant here
  directives = [{'key': PMIX_CLUSTER_ID,
                 'value': {'value': "cluster-a", 'val_type': PMIX_STRING}}]
  rc = foo.deregister_resources(directives)


INPUT PARAMETERS
----------------

* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  identifying the information to be removed. Only the ``key`` field of each element
  is used to select what to remove; the associated values are ignored except where
  they serve as qualifiers (see `DIRECTIVES`_).
* ``ninfo``: Number of elements in the ``info`` array.
* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` invoked when the
  operation completes. A ``NULL`` value makes the call **blocking** (see
  `DESCRIPTION`_).
* ``cbdata``: Opaque pointer passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Remove information about resources not associated with a given namespace |mdash|
previously registered with
:ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`
|mdash| from the local PMIx server library. Only the ``key`` fields of the
provided ``info`` array are used to identify the entries to remove; the
associated values are ignored except where they serve as qualifiers to the
request. Each matching entry is located in the server's global data cache and
deleted.

For example, to remove a specific fabric device from a given node, the ``info``
array might include a ``PMIX_NODE_INFO_ARRAY`` containing the ``PMIX_NODEID`` or
``PMIX_HOSTNAME`` that identifies the node hosting the device, together with a
``PMIX_FABRIC_DEVICE_NAME`` specifying the device. Alternatively, the device may
be removed using only its ``PMIX_DEVICE_ID``, which is unique across the entire
system.

``PMIx_server_deregister_resources`` supports both calling conventions. When
``cbfunc`` is non-``NULL`` the function is **non-blocking**: the request is
thread-shifted onto the progress thread, the function returns ``PMIX_SUCCESS``,
and ``cbfunc`` is invoked with the completion status. When ``cbfunc`` is ``NULL``
the function is **blocking**: it does not return until the operation completes,
returning ``PMIX_OPERATION_SUCCEEDED`` on success.

As with all non-blocking PMIx APIs, when a callback is supplied the caller
**must** keep the ``info`` array valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The ``key`` of each ``info`` element selects the entry to remove. Qualifiers may
be included to scope the removal, for example:

* ``PMIX_NODE_INFO_ARRAY`` (pmix_data_array_t\*) |mdash| scope the removal to a
  specific node, identified within the array by ``PMIX_NODEID`` or
  ``PMIX_HOSTNAME``.
* ``PMIX_FABRIC_DEVICE_NAME`` (char*) |mdash| the name of a fabric device to
  remove.
* ``PMIX_DEVICE_ID`` (char*) |mdash| a system-unique device identifier; sufficient
  on its own to remove the corresponding device.


CALLBACK FUNCTION
-----------------

When ``cbfunc`` is provided, it has the signature ``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

It is invoked with the completion ``status`` of the deregistration and the
``cbdata`` originally passed to ``PMIx_server_deregister_resources``.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` non-``NULL``), a return of ``PMIX_SUCCESS``
indicates only that the request was accepted for processing; the final status is
delivered to ``cbfunc``.

For the blocking form (``cbfunc`` is ``NULL``), a return of
``PMIX_OPERATION_SUCCEEDED`` indicates that the operation completed successfully
and no callback is invoked. Other returns include:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This is a server-role API, available only after
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. Because non-namespace
resource information is static, deregistration is **not** required before
finalizing the library |mdash| the library cleans up such information as part of
its normal finalize operations. Deregistration is needed only when the host
environment determines that client processes should no longer have access to the
information.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
