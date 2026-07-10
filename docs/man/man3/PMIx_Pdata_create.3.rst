.. _man3-PMIx_Pdata_create:

PMIx_Pdata_create
=================

.. include_body

``PMIx_Pdata_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_pdata_t* PMIx_Pdata_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_pdata_t <man5-pmix_pdata_t>`
structures on the heap and initialize each element as if by
:ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>` |mdash| each element
is zeroed and its ``value`` type set to ``PMIX_UNDEF``.

The returned array is owned by the caller and must be released with
:ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`, which destructs each element
and frees the array storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of :ref:`pmix_pdata_t
<man5-pmix_pdata_t>` structures, or ``NULL`` if ``n`` is zero or the
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

    pmix_pdata_t *array;

    array = PMIx_Pdata_create(4);
    /* ... populate and use array ... */
    PMIx_Pdata_free(array, 4);


.. seealso::
   :ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>`,
   :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>`,
   :ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`,
   :ref:`PMIx_Pdata_load(3) <man3-PMIx_Pdata_load>`,
   :ref:`PMIx_Pdata_xfer(3) <man3-PMIx_Pdata_xfer>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
