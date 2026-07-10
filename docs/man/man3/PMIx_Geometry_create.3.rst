.. _man3-PMIx_Geometry_create:

PMIx_Geometry_create
====================

.. include_body

``PMIx_Geometry_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_geometry_t* PMIx_Geometry_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>` structures
  to allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_geometry_t
<man5-pmix_geometry_t>` structures on the heap and construct (initialize) each
element as if by :ref:`PMIx_Geometry_construct(3)
<man3-PMIx_Geometry_construct>`.

If ``n`` is zero, the function returns ``NULL`` without allocating. If the
underlying allocation fails, ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated, constructed array of
``pmix_geometry_t`` structures, or ``NULL`` if ``n`` is zero or the allocation
could not be satisfied.


NOTES
-----

The returned array is owned by the caller and must be released with
:ref:`PMIx_Geometry_free(3) <man3-PMIx_Geometry_free>`, passing the same count
``n`` that was used to create it. ``PMIx_Geometry_free`` both destructs the
contents of each element and frees the array storage itself.


EXAMPLES
--------

Create and later free an array:

.. code-block:: c

    pmix_geometry_t *array;

    array = PMIx_Geometry_create(4);
    if (NULL == array) {
        /* handle allocation failure */
    }

    /* ... use the array ... */

    PMIx_Geometry_free(array, 4);


.. seealso::
   :ref:`PMIx_Geometry_free(3) <man3-PMIx_Geometry_free>`,
   :ref:`PMIx_Geometry_construct(3) <man3-PMIx_Geometry_construct>`,
   :ref:`PMIx_Geometry_destruct(3) <man3-PMIx_Geometry_destruct>`,
   :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
