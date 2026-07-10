.. _man3-PMIx_Data_buffer_release:

PMIx_Data_buffer_release
========================

.. include_body

``PMIx_Data_buffer_release`` |mdash| Release the payload and free a
heap-allocated ``pmix_data_buffer_t``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_buffer_release(pmix_data_buffer_t *b);


INPUT PARAMETERS
----------------

* ``b``: Pointer to the ``pmix_data_buffer_t`` to be released. May be
  ``NULL``, in which case the function does nothing.


DESCRIPTION
-----------

Fully dispose of a ``pmix_data_buffer_t`` that was allocated with
:ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`. The function
first destructs the buffer |mdash| freeing any data payload held in its
internal ``base_ptr`` allocation, exactly as
:ref:`PMIx_Data_buffer_destruct(3) <man3-PMIx_Data_buffer_destruct>` does
|mdash| and then frees the structure memory itself.

This is the difference between destruct and release: destruct frees only the
buffer's payload and leaves the structure intact for reuse, whereas release
frees the payload **and** the structure. Consequently,
``PMIx_Data_buffer_release`` must only be used on buffers whose structure
memory was allocated by the library (that is, obtained from
``PMIx_Data_buffer_create``). Do not call it on a caller-provided buffer that
was initialized with :ref:`PMIx_Data_buffer_construct(3)
<man3-PMIx_Data_buffer_construct>`; use ``PMIx_Data_buffer_destruct`` for those.

After the call the pointer ``b`` is no longer valid and must not be
dereferenced. The caller should discard its copy of the pointer.


RETURN VALUE
------------

``PMIx_Data_buffer_release`` returns no value (``void``).


NOTES
-----

``PMIx_Data_buffer_release`` is an OpenPMIx convenience routine. The
Standard-defined interface for this operation is the ``PMIX_DATA_BUFFER_RELEASE``
macro, which calls this function and then sets the caller's pointer to
``NULL``.


.. seealso::
   :ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`,
   :ref:`PMIx_Data_buffer_destruct(3) <man3-PMIx_Data_buffer_destruct>`,
   :ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`,
   :ref:`PMIx_Data_buffer_load(3) <man3-PMIx_Data_buffer_load>`,
   :ref:`PMIx_Data_buffer_unload(3) <man3-PMIx_Data_buffer_unload>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
