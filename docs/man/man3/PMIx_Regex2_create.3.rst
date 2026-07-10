.. _man3-PMIx_Regex2_create:

PMIx_Regex2_create
==================

.. include_body

``PMIx_Regex2_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_regex2_t* PMIx_Regex2_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of ``pmix_regex2_t`` structures to allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` ``pmix_regex2_t`` structures on the heap
and construct each element (see :ref:`PMIx_Regex2_construct(3)
<man3-PMIx_Regex2_construct>`) so that every ``type`` and ``bytes`` pointer is
``NULL`` and every ``len`` is zero. A pointer to the first element of the array
is returned.

If ``n`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of ``pmix_regex2_t`` structures,
or ``NULL`` if ``n`` is zero or the allocation failed.


NOTES
-----

An array obtained from ``PMIx_Regex2_create`` must be released with
:ref:`PMIx_Regex2_free(3) <man3-PMIx_Regex2_free>`, which both destructs the
contents of each element and frees the array storage itself. Do not release
such an array element-by-element with :ref:`PMIx_Regex2_destruct(3)
<man3-PMIx_Regex2_destruct>`, as that would leave the array storage leaked.


EXAMPLES
--------

Create and later free an array:

.. code-block:: c

    pmix_regex2_t *rex;

    rex = PMIx_Regex2_create(2);

    /* ... use the array ... */

    PMIx_Regex2_free(rex, 2);


.. seealso::
   :ref:`PMIx_Regex2_free(3) <man3-PMIx_Regex2_free>`,
   :ref:`PMIx_Regex2_construct(3) <man3-PMIx_Regex2_construct>`,
   :ref:`PMIx_Regex2_destruct(3) <man3-PMIx_Regex2_destruct>`,
   :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>`
