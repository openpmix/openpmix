.. _man3-PMIx_Envar_create:

PMIx_Envar_create
=================

.. include_body

``PMIx_Envar_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_envar_t* PMIx_Envar_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_envar_t <man5-pmix_envar_t>`
structures on the heap and construct each element (see
:ref:`PMIx_Envar_construct(3) <man3-PMIx_Envar_construct>`) so that every
``envar`` and ``value`` pointer is ``NULL`` and every ``separator`` is
``'\0'``. A pointer to the first element of the array is returned.

If ``n`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of :ref:`pmix_envar_t
<man5-pmix_envar_t>` structures, or ``NULL`` if ``n`` is zero or the
allocation failed.


NOTES
-----

An array obtained from ``PMIx_Envar_create`` must be released with
:ref:`PMIx_Envar_free(3) <man3-PMIx_Envar_free>`, which both destructs the
contents of each element and frees the array storage itself. Do not release
such an array element-by-element with :ref:`PMIx_Envar_destruct(3)
<man3-PMIx_Envar_destruct>`, as that would leave the array storage leaked.


EXAMPLES
--------

Create and later free an array:

.. code-block:: c

    pmix_envar_t *envars;

    envars = PMIx_Envar_create(4);

    /* ... use the array ... */

    PMIx_Envar_free(envars, 4);


.. seealso::
   :ref:`PMIx_Envar_free(3) <man3-PMIx_Envar_free>`,
   :ref:`PMIx_Envar_construct(3) <man3-PMIx_Envar_construct>`,
   :ref:`PMIx_Envar_destruct(3) <man3-PMIx_Envar_destruct>`,
   :ref:`PMIx_Envar_load(3) <man3-PMIx_Envar_load>`,
   :ref:`pmix_envar_t(5) <man5-pmix_envar_t>`
