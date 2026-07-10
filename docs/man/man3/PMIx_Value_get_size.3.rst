.. _man3-PMIx_Value_get_size:

PMIx_Value_get_size
===================

.. include_body

``PMIx_Value_get_size`` |mdash| Compute the size (in bytes) of the data payload
in a :ref:`pmix_value_t(5) <man5-pmix_value_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Value_get_size(const pmix_value_t *val,
                                     size_t *size);


INPUT PARAMETERS
----------------

* ``val``: Pointer to the :ref:`pmix_value_t(5) <man5-pmix_value_t>` whose payload
  size is to be computed.


OUTPUT PARAMETERS
-----------------

* ``size``: Pointer to a ``size_t`` variable where the computed size, in bytes,
  will be returned.


DESCRIPTION
-----------

Compute and return the size, in bytes, of the data payload contained in a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structure. The reported size is
determined from the ``type`` field of the value and reflects the amount of
storage occupied by the data itself.

For scalar data types (e.g., ``PMIX_INT32``, ``PMIX_SIZE``, ``PMIX_DOUBLE``),
the returned size is simply the size of that type. For types that carry
variable-length or referenced data, the computation includes that data as well:

* For a ``PMIX_STRING``, the returned size includes the length of the string
  plus one byte for the terminating ``NULL``.

* For PMIx-defined structures and referenced objects (e.g., a
  ``pmix_data_buffer_t``), the returned size includes both the size of the
  structure and the size of the data it references.

In all cases, the size of the :ref:`pmix_value_t(5) <man5-pmix_value_t>`
structure itself is added to the returned value, so ``size`` reflects the total
storage associated with the value, not just its payload.

The source :ref:`pmix_value_t(5) <man5-pmix_value_t>` is not altered by this
function.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_UNKNOWN_DATA_TYPE`` |mdash| the ``type`` field of the value is
  ``PMIX_UNDEF`` or otherwise not a recognized PMIx data type, so the size could
  not be computed.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The returned size includes the size of the
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structure itself in addition to the
size of its data payload. When the value references a PMIx-defined structure
(such as a ``pmix_byte_object_t``), the size accounts for the referenced
structure and its contents.


.. seealso::
   :ref:`PMIx_Info_get_size(3) <man3-PMIx_Info_get_size>`,
   :ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
