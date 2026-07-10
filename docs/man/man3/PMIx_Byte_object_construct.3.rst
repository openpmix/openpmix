.. _man3-PMIx_Byte_object_construct:

PMIx_Byte_object_construct
==========================

.. include_body

``PMIx_Byte_object_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Byte_object_construct(pmix_byte_object_t *b);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``b``: Pointer to the :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on
  the stack or embedded within another object).


DESCRIPTION
-----------

Initialize the fields of a :ref:`pmix_byte_object_t <man5-pmix_byte_object_t>`
that the caller has already allocated. The function sets the ``bytes`` pointer
to ``NULL`` and the ``size`` field to zero. No heap memory is allocated and no
payload is attached.


RETURN VALUE
------------

``PMIx_Byte_object_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Byte_object_construct`` operates on caller-provided storage, it
must be paired with :ref:`PMIx_Byte_object_destruct(3)
<man3-PMIx_Byte_object_destruct>` to release any payload the object
subsequently acquires. Do **not** pass a constructed (as opposed to created)
object to :ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>`, as that
would attempt to free storage the library did not allocate.

The convenience macro ``PMIX_BYTE_OBJECT_CONSTRUCT`` is provided as a direct
wrapper around this function.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_byte_object_t bo;

    PMIx_Byte_object_construct(&bo);


.. seealso::
   :ref:`PMIx_Byte_object_destruct(3) <man3-PMIx_Byte_object_destruct>`,
   :ref:`PMIx_Byte_object_create(3) <man3-PMIx_Byte_object_create>`,
   :ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>`,
   :ref:`PMIx_Byte_object_load(3) <man3-PMIx_Byte_object_load>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
