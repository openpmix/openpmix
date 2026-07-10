.. _man3-PMIx_Data_copy:

PMIx_Data_copy
==============

.. include_body

``PMIx_Data_copy`` |mdash| Copy a data value from one location to another.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_copy(void **dest, void *src,
                                pmix_data_type_t type);


INPUT PARAMETERS
----------------

* ``src``: A pointer to the memory location from which the data is to be
  copied.
* ``type``: The type of the data to be copied |mdash| must be one of the PMIx
  defined data types (see ``pmix_data_type_t``).


OUTPUT PARAMETERS
-----------------

* ``dest``: The address of a pointer into which the address of the resulting
  data is to be stored. The function allocates the storage for the copied
  value and returns a pointer to it in ``*dest``. The caller is responsible for
  releasing that storage when it is no longer needed (e.g., with ``free`` for
  a simple type, or the appropriate PMIx release macro for a PMIx-defined
  structure).


DESCRIPTION
-----------

Copy a data value from one location to another. Since registered data types can
be complex structures, the system needs some way to know how to copy the data
from one location to another (e.g., for storage in the registry). This function,
which can call other copy functions to build up complex data types, defines the
method for making a copy of the specified data type.

If the specified ``type`` is composed of multiple PMIx data types, the function
will ensure that each of these internal portions is also copied correctly as
part of the operation, producing a fully independent (deep) copy. The source
data referenced by ``src`` is not modified.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the provided ``src`` or ``dest`` is ``NULL``.
* ``PMIX_ERR_UNKNOWN_DATA_TYPE`` |mdash| the specified data type is not known to
  this implementation.
* ``PMIX_ERR_OUT_OF_RESOURCE`` |mdash| not enough memory to support the
  operation.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The library must be initialized (via :ref:`PMIx_Init(3) <man3-PMIx_Init>` or one
of the other initialization entry points) before ``PMIx_Data_copy`` can be used,
as the copy operation is dispatched through the buffer-operations machinery
selected at initialization.


.. seealso::
   :ref:`PMIx_Data_print(3) <man3-PMIx_Data_print>`,
   :ref:`PMIx_Data_copy_payload(3) <man3-PMIx_Data_copy_payload>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
