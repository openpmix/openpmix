.. _man3-PMIx_Data_copy_payload:

PMIx_Data_copy_payload
======================

.. include_body

``PMIx_Data_copy_payload`` |mdash| Copy a payload from one buffer to another.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_copy_payload(pmix_data_buffer_t *dest,
                                        pmix_data_buffer_t *src);


INPUT PARAMETERS
----------------

* ``dest``: Pointer to the destination ``pmix_data_buffer_t`` into which the
  payload is to be copied.
* ``src``: Pointer to the source ``pmix_data_buffer_t`` whose payload is to be
  copied. If ``src`` is ``NULL``, the function returns ``PMIX_SUCCESS`` without
  modifying ``dest``.


DESCRIPTION
-----------

This function appends a copy of the payload in the source buffer onto the
destination buffer. Note that this is *not* a destructive procedure |mdash| the
source buffer's payload will remain intact, as will any pre-existing payload in
the destination buffer. Only the unpacked (not yet consumed) portion of the
source payload is copied.

Both buffers must use the same wire format for the copy to succeed; the
operation fails if their types do not match.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the source and destination
  ``pmix_data_buffer_t`` types do not match.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the PMIx implementation does not support
  this function.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The library must be initialized (via :ref:`PMIx_Init(3) <man3-PMIx_Init>` or one
of the other initialization entry points) before ``PMIx_Data_copy_payload`` can
be used, as the copy operation is dispatched through the buffer-operations
machinery selected at initialization.


.. seealso::
   :ref:`PMIx_Data_copy(3) <man3-PMIx_Data_copy>`,
   :ref:`PMIx_Data_print(3) <man3-PMIx_Data_print>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
