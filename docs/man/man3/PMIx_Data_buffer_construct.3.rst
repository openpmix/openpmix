.. _man3-PMIx_Data_buffer_construct:

PMIx_Data_buffer_construct
==========================

.. include_body

``PMIx_Data_buffer_construct`` |mdash| Initialize a caller-provided
``pmix_data_buffer_t`` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_buffer_construct(pmix_data_buffer_t *b);


INPUT PARAMETERS
----------------

* ``b``: Pointer to the ``pmix_data_buffer_t`` structure to be initialized.
  The storage for the structure itself must already exist |mdash| it is
  supplied by the caller (typically declared on the stack or embedded within
  another object).


DESCRIPTION
-----------

Initialize the memory of a ``pmix_data_buffer_t`` that the caller has already
allocated. The function simply zeroes the entire structure, clearing the
internal ``base_ptr``, ``pack_ptr``, and ``unpack_ptr`` pointers and setting
``bytes_allocated`` and ``bytes_used`` to zero. No heap memory is allocated,
and no data payload is attached.

A buffer must be constructed (or created with
:ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`) before it is
passed to any pack, unpack, load, or unload operation; using an
uninitialized buffer produces undefined behavior.

Because ``PMIx_Data_buffer_construct`` operates on caller-provided storage, it
must be paired with :ref:`PMIx_Data_buffer_destruct(3)
<man3-PMIx_Data_buffer_destruct>` to release any payload the buffer
subsequently acquires. Do **not** pass a constructed (as opposed to created)
buffer to :ref:`PMIx_Data_buffer_release(3)
<man3-PMIx_Data_buffer_release>`, as that would attempt to free storage the
library did not allocate.


RETURN VALUE
------------

``PMIx_Data_buffer_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Data_buffer_construct`` is an OpenPMIx convenience routine. The
Standard-defined interface for this operation is the ``PMIX_DATA_BUFFER_CONSTRUCT``
macro, which is implemented as a direct call to this function.


.. seealso::
   :ref:`PMIx_Data_buffer_destruct(3) <man3-PMIx_Data_buffer_destruct>`,
   :ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`,
   :ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>`,
   :ref:`PMIx_Data_buffer_load(3) <man3-PMIx_Data_buffer_load>`,
   :ref:`PMIx_Data_buffer_unload(3) <man3-PMIx_Data_buffer_unload>`
