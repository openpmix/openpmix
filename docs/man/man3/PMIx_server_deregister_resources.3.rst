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
                 'value': "cluster-a", 'val_type': PMIX_STRING}]
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

The ``key`` of each ``info`` element selects the entries to remove. An element
whose value is **not** a data array selects by key alone: every registered entry
carrying that key is removed.

An element whose value **is** a ``pmix_data_array_t`` of ``pmix_info_t`` narrows
the removal. Two kinds of member are recognized within it:

* ``PMIX_NODEID`` (uint32_t) or ``PMIX_HOSTNAME`` (char*) |mdash| *identifiers*.
  They restrict the removal to the registered entries describing that node. A
  hostname must match exactly; ``PMIX_HOSTNAME_ALIASES`` is not consulted. An
  array carrying no identifier applies to every entry with that key, which is how
  a device that is unique across the system is removed without naming its node.
* any other member |mdash| a *target*. It selects the elements to remove from
  within the entries the identifiers chose. A target matches an element directly
  (same key, same value), or matches an element that *contains* it: a
  ``PMIX_FABRIC_DEVICE`` element carries its own array of ``pmix_info_t``, so a
  target naming the device by ``PMIX_FABRIC_DEVICE_NAME`` or ``PMIX_DEVICE_ID``
  removes that whole device from the node.

An array carrying identifiers and no target removes the whole entry for the node
it names. If removing the targets leaves an entry describing nothing |mdash| only
the identifiers remain |mdash| the entry itself is removed, since what would be
left is what an empty registration would have produced.

For example, to remove one fabric device from a given node, the ``info`` array
might include a ``PMIX_NODE_INFO_ARRAY`` containing the ``PMIX_NODEID`` or
``PMIX_HOSTNAME`` that identifies the node hosting the device, together with a
``PMIX_FABRIC_DEVICE_NAME`` specifying the device.


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
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, a ``NULL`` *info* array with a non-zero *ninfo*, or a qualifier
  array whose node identifiers are not of the declared type.

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

.. important:: Deregistration reaches the namespaces this server has
               **already** registered, not only those registered after the
               call. Non-namespace resource information is copied into a
               namespace's data store when that namespace is registered, so
               removing an entry from the server's cache also takes the key
               back from every namespace holding a copy of it, and the server
               tells its local clients to drop the copies they were handed. A
               subsequent :ref:`PMIx_Get(3) <man3-PMIx_Get>` for that key
               therefore returns ``PMIX_ERR_NOT_FOUND`` where it previously
               returned the value. The notification to the clients carries no
               acknowledgement, so it reaches them promptly rather than
               synchronously: a client that races it can read the old value
               once more.

               This governs the information held by **this** server and its
               clients. A host that wants the data withdrawn across the job
               makes the call on every server holding it.

One case is deliberately excluded from that retraction: a *qualified* request
that prunes elements out of an entry rather than removing the entry outright.
There the host has asked for part of a value to go, so the correct propagation
is the pruned value, not a deletion |mdash| pushing a removal would take more
away from a running namespace than was asked for. A pruning therefore still
governs only the namespaces registered **after** the call.


.. include:: /man/no-blocking-in-progress-thread.rst


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
