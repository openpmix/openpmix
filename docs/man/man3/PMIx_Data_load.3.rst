.. _man3-PMIx_Data_load:

PMIx_Data_load
==============

.. include_body

``PMIx_Data_load`` |mdash| Load a data payload into a buffer for subsequent
unpacking.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_load(pmix_data_buffer_t *buffer,
                                pmix_byte_object_t *payload);


INPUT PARAMETERS
----------------

* ``buffer``: Pointer to the destination ``pmix_data_buffer_t`` into which the
  payload is to be loaded. The buffer must have been allocated (e.g., with
  ``PMIX_DATA_BUFFER_CREATE``) or constructed (e.g., with
  ``PMIX_DATA_BUFFER_CONSTRUCT``) in advance.
* ``payload``: Pointer to a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  containing the data payload to be transferred into the buffer.


DESCRIPTION
-----------

Transfer the contents of the ``payload`` byte object into ``buffer`` so that the
data can subsequently be extracted with :ref:`PMIx_Data_unpack(3)
<man3-PMIx_Data_unpack>`. This is the inverse of :ref:`PMIx_Data_unload(3)
<man3-PMIx_Data_unload>`.

The transfer is a move, not a copy: the buffer takes direct ownership of the
memory referenced by ``payload->bytes`` |mdash| no new allocation is made and no
byte-copy is performed. Upon successful completion the ``payload`` object is
emptied |mdash| its ``bytes`` field is reset to ``NULL`` and its ``size`` field to
zero |mdash| so that the caller and the buffer no longer share the same memory.
Because ownership has moved into the buffer, the caller must **not** free
``payload->bytes`` afterward; the memory will be released when the buffer itself
is destructed (e.g., with ``PMIX_DATA_BUFFER_DESTRUCT`` or
``PMIX_DATA_BUFFER_RELEASE``).

If the buffer already holds a payload, that existing payload is first freed
(the buffer is destructed and re-constructed) before the new payload is loaded.
If ``payload`` is ``NULL``, any existing buffer payload is released and the
function returns leaving the buffer empty.

The caller is responsible for pre-packing the provided payload. The load
function performs no serialization and cannot convert any data contained in the
payload to network byte order; the payload is expected to already be in packed
form, such as that produced by :ref:`PMIx_Data_unload(3)
<man3-PMIx_Data_unload>`.


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

Use :ref:`PMIx_Data_embed(3) <man3-PMIx_Data_embed>` instead when the source
payload must be preserved for the caller; unlike ``PMIx_Data_load``, embed copies
the payload into the buffer and leaves the source byte object unaltered.


.. seealso::
   :ref:`PMIx_Data_unload(3) <man3-PMIx_Data_unload>`,
   :ref:`PMIx_Data_embed(3) <man3-PMIx_Data_embed>`,
   :ref:`PMIx_Data_pack(3) <man3-PMIx_Data_pack>`,
   :ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
