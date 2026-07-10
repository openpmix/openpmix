.. _man3-PMIx_Byte_object_load:

PMIx_Byte_object_load
=====================

.. include_body

``PMIx_Byte_object_load`` |mdash| Load a data buffer into a
:ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Byte_object_load(pmix_byte_object_t *b,
                              char *d, size_t sz);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``b``: Pointer to the :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  structure into which the data is to be loaded.
* ``d``: Pointer to the data buffer to load.
* ``sz``: Size, in bytes, of the data buffer.


DESCRIPTION
-----------

Load a data buffer into a :ref:`pmix_byte_object_t <man5-pmix_byte_object_t>`
by directly assigning the object's ``bytes`` field to ``d`` and its ``size``
field to ``sz``. No copy is made |mdash| the byte object simply takes ownership
of the pointer ``d`` that the caller supplied.

Because ownership transfers, the caller must not free ``d`` after loading it;
the memory will be released when the byte object is subsequently destructed or
freed. The target structure ``b`` should be empty (freshly constructed) before
loading, since any existing payload it holds is overwritten without being
freed.


RETURN VALUE
------------

``PMIx_Byte_object_load`` returns no value (``void``).


NOTES
-----

The convenience macro ``PMIX_BYTE_OBJECT_LOAD`` wraps this function and
additionally resets the caller's source pointer to ``NULL`` and the source size
to zero, reflecting the transfer of ownership.


EXAMPLES
--------

Load an allocated buffer into a byte object:

.. code-block:: c

    pmix_byte_object_t bo;
    char *data;
    size_t len;

    PMIx_Byte_object_construct(&bo);
    data = strdup("payload");
    len = strlen(data) + 1;
    PMIx_Byte_object_load(&bo, data, len);
    /* 'bo' now owns 'data'; do not free 'data' directly */
    PMIx_Byte_object_destruct(&bo);


.. seealso::
   :ref:`PMIx_Byte_object_construct(3) <man3-PMIx_Byte_object_construct>`,
   :ref:`PMIx_Byte_object_destruct(3) <man3-PMIx_Byte_object_destruct>`,
   :ref:`PMIx_Byte_object_create(3) <man3-PMIx_Byte_object_create>`,
   :ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
