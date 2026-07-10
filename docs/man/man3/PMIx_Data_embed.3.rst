.. _man3-PMIx_Data_embed:

PMIx_Data_embed
===============

.. include_body

``PMIx_Data_embed`` |mdash| Embed a copy of a data payload into a buffer,
preserving the source payload.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_embed(pmix_data_buffer_t *buffer,
                                 const pmix_byte_object_t *payload);


INPUT PARAMETERS
----------------

* ``payload``: Pointer to a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  containing the data payload to be embedded into the buffer. The payload is
  treated as read-only and is not modified.


OUTPUT PARAMETERS
-----------------

* ``buffer``: Pointer to the destination ``pmix_data_buffer_t`` into which a copy
  of the payload is placed. The buffer must have been allocated (e.g., with
  ``PMIX_DATA_BUFFER_CREATE``) or constructed (e.g., with
  ``PMIX_DATA_BUFFER_CONSTRUCT``) in advance.


DESCRIPTION
-----------

Embed the contents of the ``payload`` byte object into ``buffer`` so that the
data can subsequently be extracted with :ref:`PMIx_Data_unpack(3)
<man3-PMIx_Data_unpack>`. The embed function is identical in operation to
:ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>` except that it does **not** clear
or otherwise alter the payload upon completion.

Unlike :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`, which moves ownership of
the payload's memory into the buffer, ``PMIx_Data_embed`` copies the payload: the
buffer receives an independent copy of the data, and the source ``payload`` byte
object is left intact and still owned by the caller. The caller therefore remains
responsible for releasing ``payload`` when it is no longer needed, and the buffer
and its embedded copy are released separately when the buffer is destructed.

If the buffer already holds a payload, that existing payload is first freed
(the buffer is destructed and re-constructed) before the copy is embedded. If
``payload`` is ``NULL``, any existing buffer payload is released and the function
returns leaving the buffer empty.

The caller is responsible for pre-packing the provided payload. The embed
function performs no serialization and cannot convert any data contained in the
payload to network byte order; the payload is expected to already be in packed
form.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``buffer`` pointer is ``NULL``.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Use ``PMIx_Data_embed`` when the source payload must remain valid and owned by
the caller after the operation. When the payload can be consumed and its memory
handed to the buffer, :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>` avoids the
copy.


.. seealso::
   :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`,
   :ref:`PMIx_Data_unload(3) <man3-PMIx_Data_unload>`,
   :ref:`PMIx_Data_copy_payload(3) <man3-PMIx_Data_copy_payload>`,
   :ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
