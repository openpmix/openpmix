.. _man3-PMIx_Resource_unit_create:

PMIx_Resource_unit_create
=========================

.. include_body

``PMIx_Resource_unit_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_resource_unit_t* PMIx_Resource_unit_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
  structures to allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_resource_unit_t
<man5-pmix_resource_unit_t>` structures on the heap and initialize each element
as if by :ref:`PMIx_Resource_unit_construct(3)
<man3-PMIx_Resource_unit_construct>` |mdash| each element is zeroed and its
``type`` field set to ``PMIX_DEVTYPE_UNKNOWN``.

The returned array is owned by the caller and must be released with
:ref:`PMIx_Resource_unit_free(3) <man3-PMIx_Resource_unit_free>`, which frees
the array storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of :ref:`pmix_resource_unit_t
<man5-pmix_resource_unit_t>` structures, or ``NULL`` if ``n`` is zero or the
allocation fails.


NOTES
-----

Because a request for zero elements returns ``NULL``, callers should treat a
``NULL`` return as either an out-of-memory condition or a zero-length request,
depending on the value passed for ``n``.


EXAMPLES
--------

Allocate and free an array:

.. code-block:: c

    pmix_resource_unit_t *array;

    array = PMIx_Resource_unit_create(2);
    /* ... populate and use array ... */
    PMIx_Resource_unit_free(array, 2);


.. seealso::
   :ref:`PMIx_Resource_unit_construct(3) <man3-PMIx_Resource_unit_construct>`,
   :ref:`PMIx_Resource_unit_destruct(3) <man3-PMIx_Resource_unit_destruct>`,
   :ref:`PMIx_Resource_unit_free(3) <man3-PMIx_Resource_unit_free>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
