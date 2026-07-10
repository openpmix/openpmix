.. _man3-PMIx_Byte_object_create:

PMIx_Byte_object_create
=======================

.. include_body

``PMIx_Byte_object_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_byte_object_t* PMIx_Byte_object_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  structures to allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_byte_object_t
<man5-pmix_byte_object_t>` structures on the heap and construct each element
(setting its ``bytes`` pointer to ``NULL`` and its ``size`` to zero).

If ``n`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated and initialized array, or ``NULL`` if
``n`` is zero or the allocation fails.


NOTES
-----

An array obtained from ``PMIx_Byte_object_create`` must be released with
:ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>`, which destructs
every element's payload and frees the array storage itself. Passing the same
count ``n`` used at creation is required.

The convenience macro ``PMIX_BYTE_OBJECT_CREATE`` is provided as a wrapper
around this function.


EXAMPLES
--------

Create and later release an array of byte objects:

.. code-block:: c

    pmix_byte_object_t *array;

    array = PMIx_Byte_object_create(4);
    /* ... populate and use the objects ... */
    PMIx_Byte_object_free(array, 4);


.. seealso::
   :ref:`PMIx_Byte_object_free(3) <man3-PMIx_Byte_object_free>`,
   :ref:`PMIx_Byte_object_construct(3) <man3-PMIx_Byte_object_construct>`,
   :ref:`PMIx_Byte_object_destruct(3) <man3-PMIx_Byte_object_destruct>`,
   :ref:`PMIx_Byte_object_load(3) <man3-PMIx_Byte_object_load>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
