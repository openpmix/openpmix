.. _man3-PMIx_Endpoint_create:

PMIx_Endpoint_create
====================

.. include_body

``PMIx_Endpoint_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_endpoint_t* PMIx_Endpoint_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structures
  to allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n``
:ref:`pmix_endpoint_t <man5-pmix_endpoint_t>` structures on the heap and
construct each element, as though
:ref:`PMIx_Endpoint_construct(3) <man3-PMIx_Endpoint_construct>` had been
called on every member (``uuid`` and ``osname`` set to ``NULL`` and the
embedded ``endpt`` byte object zeroed).

The returned array is owned by the caller and must eventually be released with
:ref:`PMIx_Endpoint_free(3) <man3-PMIx_Endpoint_free>`, which destructs the
contents of each element and frees the array storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of
:ref:`pmix_endpoint_t <man5-pmix_endpoint_t>` structures, or ``NULL`` if ``n``
is zero or the allocation fails.


EXAMPLES
--------

Create and later release an array of endpoint structures:

.. code-block:: c

    pmix_endpoint_t *eps;

    eps = PMIx_Endpoint_create(2);
    if (NULL == eps) {
        /* handle allocation failure */
    }

    /* ... populate and use the array ... */

    PMIx_Endpoint_free(eps, 2);


.. seealso::
   :ref:`PMIx_Endpoint_free(3) <man3-PMIx_Endpoint_free>`,
   :ref:`PMIx_Endpoint_construct(3) <man3-PMIx_Endpoint_construct>`,
   :ref:`PMIx_Endpoint_destruct(3) <man3-PMIx_Endpoint_destruct>`,
   :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>`
