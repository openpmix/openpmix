.. _man3-PMIx_Data_pack:

PMIx_Data_pack
==============

.. include_body

``PMIx_Data_pack`` |mdash| Pack one or more values of a specified type into a
buffer, usually for transmission to another process.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_pack(const pmix_proc_t *target,
                                pmix_data_buffer_t *buffer,
                                void *src, int32_t num_vals,
                                pmix_data_type_t type);


INPUT PARAMETERS
----------------

* ``target``: Pointer to a ``pmix_proc_t`` structure containing the
  namespace/rank of the process that will ultimately unpack the buffer. Only the
  target's namespace is relevant, as all processes in a namespace are required to
  use the same PMIx version. A ``NULL`` value indicates that the target is based
  on the same PMIx version as the caller.
* ``src``: A ``void*`` pointer to the data that is to be packed. Strings must be
  passed as ``(char **)`` |mdash| i.e., the caller passes the address of the
  pointer to the string as the ``void*``. This allows a single pack function to
  accept multiple strings in one call.
* ``num_vals``: An ``int32_t`` indicating the number of values, beginning at the
  location pointed to by ``src``, that are to be packed. A string value is counted
  as a single value regardless of length. The values must be contiguous in memory.
  Arrays of pointers (e.g., string arrays) should be contiguous, although the data
  pointed to need not be contiguous across array entries.
* ``type``: The type of the data to be packed |mdash| must be one of the PMIx
  defined ``pmix_data_type_t`` values.


INPUT/OUTPUT PARAMETERS
-----------------------

* ``buffer``: Pointer to a ``pmix_data_buffer_t`` into which the data is to be
  packed. The buffer must have already been initialized via
  ``PMIX_DATA_BUFFER_CREATE`` or ``PMIX_DATA_BUFFER_CONSTRUCT``. If the buffer
  already contains packed data, the new data is appended to it. The buffer's
  internal pointers may be updated by this call.


DESCRIPTION
-----------

Pack one or more values of the specified ``type`` into ``buffer``. Multiple values
of the same type may be packed in a single call by supplying a ``num_vals`` greater
than one and a contiguous block of values at ``src``. The buffer accumulates data
across successive calls |mdash| each ``PMIx_Data_pack`` appends to whatever was
already present |mdash| so a complete message may be assembled with a series of
calls and later disassembled with a matching series of
:ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>` calls in the same order.

The ``target`` identifies the process that will eventually unpack the buffer. It is
used solely to resolve any data-type representation differences between PMIx
versions: the library selects the wire format appropriate to the target's version
so that the packed bytes can be correctly interpreted on the receiving side. The
target must therefore be known before packing so that the library is aware of the
version in use. Because all processes in a namespace use the same PMIx version, the
caller need only know one process from the target's namespace; passing ``NULL``
selects the caller's own version.

Data packed with a type that does not explicitly specify its size may lose
precision when unpacked by a non-homogeneous recipient. PMIx will do its best to
manage heterogeneity between the packer and unpacker. Sending a value outside the
range that can be handled by the recipient returns an error, but that error is
generated upon unpacking and may not be detected during packing.

The raw bytes managed by a ``pmix_data_buffer_t`` can be extracted for transmission
with PMIx_Data_unload(3) and reconstituted with PMIx_Data_load(3).


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the provided ``buffer`` or ``src`` is ``NULL``.
* ``PMIX_ERR_UNKNOWN_DATA_TYPE`` |mdash| the specified data type is not known to
  this implementation.
* ``PMIX_ERR_OUT_OF_RESOURCE`` |mdash| insufficient memory to complete the
  operation.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the ``target`` namespace could not be resolved,
  so the appropriate wire format could not be determined.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The buffer must be initialized before use. The data referenced by ``src`` is copied
into the buffer, so the caller may modify or free it once the call returns.

Packing and unpacking are strictly ordered, complementary operations: values must be
unpacked in the same sequence, and with the same types, in which they were packed.


.. seealso::
   :ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>`,
   :ref:`PMIx_Data_copy(3) <man3-PMIx_Data_copy>`,
   :ref:`PMIx_Data_print(3) <man3-PMIx_Data_print>`,
   :ref:`PMIx_Data_copy_payload(3) <man3-PMIx_Data_copy_payload>`,
   :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`,
   :ref:`PMIx_Data_unload(3) <man3-PMIx_Data_unload>`,
   :ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
