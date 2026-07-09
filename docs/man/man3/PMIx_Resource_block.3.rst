.. _man3-PMIx_Resource_block:

PMIx_Resource_block
===================

.. include_body

``PMIx_Resource_block``, ``PMIx_Resource_block_nb`` |mdash| Define or modify a
named resource block for use in allocation operations.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Resource_block(pmix_resource_block_directive_t directive,
                                     char *block,
                                     const pmix_resource_unit_t *res, size_t nres,
                                     const pmix_info_t *info, size_t ninfo);

   pmix_status_t PMIx_Resource_block_nb(pmix_resource_block_directive_t directive,
                                        char *block,
                                        const pmix_resource_unit_t *res, size_t nres,
                                        const pmix_info_t *info, size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata);


INPUT PARAMETERS
----------------

* ``directive``: A directive of type ``pmix_resource_block_directive_t``
  identifying the requested operation on the block (see `DIRECTIVES`_).
* ``block``: NULL-terminated string naming the resource block. The name must be
  unique within the requestor's current session.
* ``res``: Pointer to an array of ``pmix_resource_unit_t`` structures, each of
  which pairs a device type (``type``, a ``pmix_device_type_t``) with a count
  (``count``), describing the resources to which the operation applies. A
  ``NULL`` value is supported when no resource units are required |mdash| for
  example, when deleting a block.
* ``nres``: Number of elements in the ``res`` array.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying attributes that qualify the operation. A ``NULL`` value
  is supported when no attributes are desired.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form adds a callback:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked with the
  final status once the operation has been processed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Define a named resource "block" that can subsequently be referenced in
allocation operations, including the ability to define, extend, reduce, and
delete block definitions. A resource block associates a caller-chosen name with
a set of resource units (device type and count pairs); the name must be unique
within the requestor's current session.

``PMIx_Resource_block`` is the blocking form: it does not return until the
operation is complete, and the return status reflects the outcome.
``PMIx_Resource_block_nb`` is the non-blocking form: it returns immediately, and
the provided ``cbfunc`` is invoked with the final status once the operation has
been processed.

As with all non-blocking PMIx APIs, callers of ``PMIx_Resource_block_nb``
**must** keep the ``block``, ``res``, and ``info`` arguments valid until
``cbfunc`` is invoked.


DIRECTIVES
----------

The ``directive`` argument is a value of type
``pmix_resource_block_directive_t`` that identifies the requested operation:

* ``PMIX_RESOURCE_BLOCK_DEFINE`` |mdash| define a new resource block.
* ``PMIX_RESOURCE_BLOCK_EXTEND`` |mdash| extend an existing block definition by
  adding the specified resources to the block.
* ``PMIX_RESOURCE_BLOCK_REMOVE`` |mdash| remove the specified resources from the
  block definition.
* ``PMIX_RESOURCE_BLOCK_DELETE`` |mdash| delete the resource block definition.

Values at or above ``PMIX_RESOURCE_BLOCK_EXTERNAL`` are reserved for
implementer-defined directives.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the operation completed
successfully. For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates
only that the request was accepted for processing; the final status is
delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the operation was successfully processed.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the operation is not supported in the
  caller's role or environment |mdash| for example, the caller is itself the
  scheduler, or is a system controller with no scheduler attached.
* ``PMIX_ERR_UNREACH`` |mdash| the caller is not connected to a server capable
  of servicing the request.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

This operation is only meaningful when the caller can reach a scheduler, either
because the caller's server is the scheduler or because the caller is a server
whose host environment provides a resource-block entry point that can forward
the request. A process hosted directly by the scheduler cannot issue a
resource-block request against itself and will receive
``PMIX_ERR_NOT_SUPPORTED``.

``PMIx_Resource_block`` is a PMIx library extension and is not part of the
published PMIx Standard.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Session_control(3) <man3-PMIx_Session_control>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
