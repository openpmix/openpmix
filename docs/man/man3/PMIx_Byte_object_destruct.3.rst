.. _man3-PMIx_Byte_object_destruct:

PMIx_Byte_object_destruct
=========================

.. include_body

``PMIx_Byte_object_destruct`` |mdash| Release the payload of a
:ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Byte_object_destruct(pmix_byte_object_t *g);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``g``: Pointer to the :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release the payload held by a :ref:`pmix_byte_object_t <man5-pmix_byte_object_t>`.
If the ``bytes`` pointer is non-``NULL``, the memory it references is freed.
The ``bytes`` pointer is then reset to ``NULL`` and the ``size`` field is set
to zero, leaving the structure in the same state as a freshly constructed
object.

Only the payload is released; the storage for the structure itself is not
freed. This routine is therefore the correct counterpart to
:ref:`PMIx_Byte_object_construct(3) <man3-PMIx_Byte_object_construct>` for
caller-provided (stack or embedded) structures.


RETURN VALUE
------------

``PMIx_Byte_object_destruct`` returns no value (``void``).


NOTES
-----

Do not use ``PMIx_Byte_object_destruct`` on an element of an array obtained
from :ref:`PMIx_Byte_object_create(3) <man3-PMIx_Byte_object_create>` if you
also intend to free the array as a whole |mdash| use
:ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>` for that case,
which destructs every element and then frees the array storage.

The convenience macro ``PMIX_BYTE_OBJECT_DESTRUCT`` is provided as a direct
wrapper around this function.


.. seealso::
   :ref:`PMIx_Byte_object_construct(3) <man3-PMIx_Byte_object_construct>`,
   :ref:`PMIx_Byte_object_create(3) <man3-PMIx_Byte_object_create>`,
   :ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>`,
   :ref:`PMIx_Byte_object_load(3) <man3-PMIx_Byte_object_load>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
