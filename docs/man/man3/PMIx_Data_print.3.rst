.. _man3-PMIx_Data_print:

PMIx_Data_print
===============

.. include_body

``PMIx_Data_print`` |mdash| Pretty-print a data value into an allocated string.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_print(char **output, char *prefix,
                                 void *src, pmix_data_type_t type);


INPUT PARAMETERS
----------------

* ``prefix``: A NULL-terminated string to be prepended to the resulting output.
* ``src``: A pointer to the memory location of the data value to be printed.
* ``type``: The type of the data value to be printed |mdash| must be one of the
  PMIx defined data types (see ``pmix_data_type_t``).


OUTPUT PARAMETERS
-----------------

* ``output``: The address of a pointer into which the address of the resulting
  string is to be stored. On success, the function allocates a NULL-terminated
  string containing the printed representation and returns a pointer to it in
  ``*output``. The caller owns this storage and must release it with ``free``
  when it is no longer needed.


DESCRIPTION
-----------

Since registered data types can be complex structures, the system needs some way
to know how to print them (i.e., convert them to a string representation).
``PMIx_Data_print`` provides that capability, producing a human-readable string
for the specified data value with the caller-provided ``prefix`` prepended to
it. It is provided primarily for debugging purposes.

Note that the format of the resulting string is implementation-defined and may
vary from one PMIx implementation to another.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the provided data type is not recognized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The library must be initialized (via :ref:`PMIx_Init(3) <man3-PMIx_Init>` or one
of the other initialization entry points) before ``PMIx_Data_print`` can be
used, as the print operation is dispatched through the buffer-operations
machinery selected at initialization.


.. seealso::
   :ref:`PMIx_Data_copy(3) <man3-PMIx_Data_copy>`,
   :ref:`PMIx_Data_copy_payload(3) <man3-PMIx_Data_copy_payload>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
