.. _man3-PMIx_Data_buffer_destruct:

PMIx_Data_buffer_destruct
=========================

.. include_body

``PMIx_Data_buffer_destruct`` |mdash| Release the payload held inside a
``pmix_data_buffer_t`` without freeing the structure itself.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_buffer_destruct(pmix_data_buffer_t *b);


INPUT PARAMETERS
----------------

* ``b``: Pointer to the ``pmix_data_buffer_t`` whose internal storage is to be
  released.


DESCRIPTION
-----------

Free the data payload stored inside a ``pmix_data_buffer_t`` and reset the
structure so that it is empty but still usable. If the buffer holds a payload,
its internal ``base_ptr`` allocation is freed and the pointer is set to
``NULL``. The ``pack_ptr`` and ``unpack_ptr`` pointers are cleared, and
``bytes_allocated`` and ``bytes_used`` are set to zero.

The structure memory of ``b`` itself is **not** freed. After destruction the
buffer is left in the same empty state produced by
:ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`, so it may
be reused for further pack or load operations without being reconstructed.

``PMIx_Data_buffer_destruct`` is the correct counterpart to
:ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>` for
buffers whose structure storage was supplied by the caller (for example, a
stack-declared or embedded buffer). For a buffer allocated with
:ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`, use
:ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>` instead,
which both destructs the payload and frees the structure.


RETURN VALUE
------------

``PMIx_Data_buffer_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Data_buffer_destruct`` is an OpenPMIx convenience routine. The
Standard-defined interface for this operation is the ``PMIX_DATA_BUFFER_DESTRUCT``
macro, which is implemented as a direct call to this function.


.. seealso::
   :ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`,
   :ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`,
   :ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>`,
   :ref:`PMIx_Data_buffer_load(3) <man3-PMIx_Data_buffer_load>`,
   :ref:`PMIx_Data_buffer_unload(3) <man3-PMIx_Data_buffer_unload>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
