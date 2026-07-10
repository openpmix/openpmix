.. _man3-PMIx_Data_buffer_create:

PMIx_Data_buffer_create
=======================

.. include_body

``PMIx_Data_buffer_create`` |mdash| Allocate and initialize a new
``pmix_data_buffer_t`` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_data_buffer_t* PMIx_Data_buffer_create(void);


DESCRIPTION
-----------

Allocate a new ``pmix_data_buffer_t`` on the heap and initialize it. The
function allocates the structure and then zeroes it |mdash| exactly as
:ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>` would
|mdash| so the returned buffer is empty and ready for use with any pack,
unpack, load, or unload operation. No data payload is attached.

Ownership of the returned structure passes to the caller. A buffer obtained
from ``PMIx_Data_buffer_create`` must eventually be freed with
:ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>`, which
releases any payload the buffer holds and frees the structure itself. Do
**not** free the returned pointer with a plain ``free()``, and do not release
it with :ref:`PMIx_Data_buffer_destruct(3) <man3-PMIx_Data_buffer_destruct>`
alone (which would leak the structure).


RETURN VALUE
------------

Returns a pointer to a newly allocated, initialized ``pmix_data_buffer_t`` on
success. Returns ``NULL`` if the allocation failed.


NOTES
-----

``PMIx_Data_buffer_create`` is an OpenPMIx convenience routine. The
Standard-defined interface for this operation is the ``PMIX_DATA_BUFFER_CREATE``
macro, which is implemented as an assignment from a call to this function.


.. seealso::
   :ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>`,
   :ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`,
   :ref:`PMIx_Data_buffer_destruct(3) <man3-PMIx_Data_buffer_destruct>`,
   :ref:`PMIx_Data_buffer_load(3) <man3-PMIx_Data_buffer_load>`,
   :ref:`PMIx_Data_buffer_unload(3) <man3-PMIx_Data_buffer_unload>`
