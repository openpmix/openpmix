.. _man3-PMIx_Data_unload:

PMIx_Data_unload
================

.. include_body

``PMIx_Data_unload`` |mdash| Unload the contents of a buffer into a byte object
for transmission or storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Data_unload(pmix_data_buffer_t *buffer,
                                  pmix_byte_object_t *payload);


INPUT PARAMETERS
----------------

* ``buffer``: Pointer to the source ``pmix_data_buffer_t`` whose payload is to be
  unloaded.


OUTPUT PARAMETERS
-----------------

* ``payload``: Pointer to a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  into which the buffer's payload is placed. The structure is initialized by the
  function before use; on return its ``bytes`` and ``size`` fields describe the
  extracted data.


DESCRIPTION
-----------

Extract the not-yet-unpacked portion of ``buffer``'s payload into the ``payload``
byte object, giving the caller direct access to the packed data |mdash| for
example, to hand it to a transmission or storage routine. This is the inverse of
:ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`.

Only the region of the payload that has not already been consumed by a prior
:ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>` is unloaded; any portion that
was previously unpacked is not included. If nothing has yet been unpacked from
the buffer, the entire payload region is handed back directly without a
byte-copy (an ownership transfer); otherwise the remaining region is copied into
freshly allocated memory. In either case the ``payload`` byte object owns the
returned ``bytes`` on completion, and the caller is responsible for releasing
that memory (e.g., with ``PMIX_BYTE_OBJECT_DESTRUCT`` or by calling ``free`` on
``payload->bytes``).

This is a destructive operation on the buffer. While the data returned in
``payload`` is undisturbed, the function clears the buffer's pointers to that
data by destructing and re-constructing it, leaving the buffer empty. The buffer
and the payload are thereby completely separated, and the caller is free to
release the buffer without affecting the unloaded data.

If the buffer contains no payload, the function succeeds and returns an empty
byte object.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``buffer`` and/or ``payload`` pointer is
  ``NULL``.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The unloaded payload is in packed (wire) form. It may be reloaded into a buffer
for later unpacking with :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`.


.. seealso::
   :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`,
   :ref:`PMIx_Data_embed(3) <man3-PMIx_Data_embed>`,
   :ref:`PMIx_Data_pack(3) <man3-PMIx_Data_pack>`,
   :ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
