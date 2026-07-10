.. _man3-PMIx_Topology_create:

PMIx_Topology_create
====================

.. include_body

``PMIx_Topology_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_topology_t(5) <man5-pmix_topology_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_topology_t* PMIx_Topology_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_topology_t(5) <man5-pmix_topology_t>` structures
  to allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_topology_t
<man5-pmix_topology_t>` structures on the heap and construct (initialize) each
element as if by :ref:`PMIx_Topology_construct(3)
<man3-PMIx_Topology_construct>`.

If ``n`` is zero, the function returns ``NULL`` without allocating. If the
underlying allocation fails, ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated, constructed array of
``pmix_topology_t`` structures, or ``NULL`` if ``n`` is zero or the allocation
could not be satisfied.


NOTES
-----

The returned array is owned by the caller and must be released with
:ref:`PMIx_Topology_free(3) <man3-PMIx_Topology_free>`, passing the same count
``n`` that was used to create it. ``PMIx_Topology_free`` both destructs the
contents of each element and frees the array storage itself.


EXAMPLES
--------

Create and later free an array:

.. code-block:: c

    pmix_topology_t *array;

    array = PMIx_Topology_create(2);
    if (NULL == array) {
        /* handle allocation failure */
    }

    /* ... use the array ... */

    PMIx_Topology_free(array, 2);


.. seealso::
   :ref:`PMIx_Topology_free(3) <man3-PMIx_Topology_free>`,
   :ref:`PMIx_Topology_construct(3) <man3-PMIx_Topology_construct>`,
   :ref:`PMIx_Topology_destruct(3) <man3-PMIx_Topology_destruct>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
