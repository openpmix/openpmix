.. _man3-PMIx_Data_unpack:

PMIx_Data_unpack
================

.. include_body

``PMIx_Data_unpack`` |mdash| Unpack one or more values of a specified type from a
buffer.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_unpack(const pmix_proc_t *source,
                                  pmix_data_buffer_t *buffer, void *dest,
                                  int32_t *max_num_values,
                                  pmix_data_type_t type);


INPUT PARAMETERS
----------------

* ``source``: Pointer to a ``pmix_proc_t`` structure containing the
  namespace/rank of the process that packed the buffer. Only the namespace is used
  to determine the packing version, as all processes in a namespace are required to
  use the same PMIx version. A ``NULL`` value indicates that the source is based on
  the same PMIx version as the caller.
* ``buffer``: Pointer to a ``pmix_data_buffer_t`` from which the value(s) will be
  extracted. The buffer must have already been initialized (via
  ``PMIX_DATA_BUFFER_CREATE`` or ``PMIX_DATA_BUFFER_CONSTRUCT``) and filled with
  data. The buffer's internal pointers may be updated by this call.
* ``type``: The type of the data to be unpacked |mdash| must be one of the PMIx
  defined ``pmix_data_type_t`` values, and must match the type of the next item in
  the buffer.


OUTPUT PARAMETERS
-----------------

* ``dest``: A ``void*`` pointer to the memory location into which the data is to be
  stored. The values are stored contiguously in memory. For strings, this pointer
  must be to ``(char **)`` to support multiple string operations; the unpack
  function allocates memory for each string in the array, but the caller must
  provide adequate memory for the array of pointers.
* ``max_num_values``: On input, an ``int32_t*`` giving the maximum number of values
  that can be unpacked into the memory at ``dest``. On completion, it is set to the
  number of values actually unpacked. This will never exceed the input value.


DESCRIPTION
-----------

Unpack the next value (or values) of the specified ``type`` from ``buffer``,
storing the result at ``dest``. The caller sets ``*max_num_values`` to the number
of values its ``dest`` storage can hold; on return, ``*max_num_values`` reports how
many values were actually unpacked. If more values of the requested type are
present in the buffer than will fit, the function unpacks what it can and returns an
error indicating that the buffer was only partially unpacked; if fewer values are
unpacked than are available, the buffer is left in an unpackable state and an error
is likewise returned to warn of this condition.

Values must be unpacked in the same order, and with the same types, in which they
were packed with :ref:`PMIx_Data_pack(3) <man3-PMIx_Data_pack>`. Specifying a
``type`` that does not match the type of the next item in the buffer, or attempting
to read past the end of the stored data, is reported as an error.

Unpacking is a nondestructive process |mdash| the values are not removed from the
buffer. It is therefore possible to re-unpack a value from the same buffer by
resetting the buffer's unpack pointer.

The ``source`` identifies the process that packed the buffer. It is used solely to
resolve any data-type representation differences between PMIx versions, allowing the
library to interpret the packed bytes using the wire format the source used. The
source must therefore be known before unpacking; because all processes in a
namespace use the same PMIx version, the caller need only know one process from the
source's namespace. Passing ``NULL`` selects the caller's own version.

Data that was packed using a type that does not explicitly specify its size may lose
precision when unpacked by a non-homogeneous recipient. PMIx will do its best to
manage heterogeneity between the packer and unpacker. A value larger than the
recipient can handle produces an error that is generated here, at unpack time, and
cannot be detected during packing.

.. note::
   The data-type check is not completely safe: a corrupted buffer may happen to
   contain a byte pattern at the current unpack position that matches the requested
   type flag even though it is not a valid value of that type. This limitation
   applies to all unpack operations.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the provided ``buffer`` or ``dest`` is ``NULL``.
* ``PMIX_ERR_UNKNOWN_DATA_TYPE`` |mdash| the specified data type is not known to
  this implementation, or does not match the next item in the buffer.
* ``PMIX_ERR_UNPACK_INADEQUATE_SPACE`` |mdash| more values are present in the
  buffer than the caller's storage (``*max_num_values``) can hold; the buffer was
  only partially unpacked.
* ``PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER`` |mdash| the unpack attempted to read
  beyond the end of the data stored in the buffer.
* ``PMIX_ERR_OUT_OF_RESOURCE`` |mdash| insufficient memory to complete the
  operation.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the ``source`` namespace could not be resolved,
  so the appropriate wire format could not be determined.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The caller is responsible for providing adequate memory storage for the requested
data and for setting ``*max_num_values`` accordingly before the call. For string
arrays, the library allocates each individual string; the caller frees them (e.g.,
with ``PMIX_ARGV_FREE`` or ``free``) when they are no longer needed.

Because unpacking is nondestructive and strictly ordered, a buffer assembled with a
sequence of :ref:`PMIx_Data_pack(3) <man3-PMIx_Data_pack>` calls is recovered with a
matching sequence of ``PMIx_Data_unpack`` calls issued in the same order.


.. seealso::
   :ref:`PMIx_Data_pack(3) <man3-PMIx_Data_pack>`,
   :ref:`PMIx_Data_copy(3) <man3-PMIx_Data_copy>`,
   :ref:`PMIx_Data_print(3) <man3-PMIx_Data_print>`,
   :ref:`PMIx_Data_copy_payload(3) <man3-PMIx_Data_copy_payload>`,
   :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`,
   :ref:`PMIx_Data_unload(3) <man3-PMIx_Data_unload>`,
   :ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
