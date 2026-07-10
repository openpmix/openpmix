.. _man3-PMIx_Value_create:

PMIx_Value_create
=================

.. include_body

``PMIx_Value_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_value_t* PMIx_Value_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_value_t(5) <man5-pmix_value_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_value_t(5)
<man5-pmix_value_t>` structures on the heap and construct each element
(zeroing it and setting its ``type`` to ``PMIX_UNDEF``), exactly as
:ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>` does for a single
value.

The returned array is owned by the caller and must be released with
:ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`, passing the same count
``n``, which destructs the contents of each element and frees the array
storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structures on success.

Returns ``NULL`` if ``n`` is zero or if the allocation fails.


NOTES
-----

``PMIx_Value_create`` is an OpenPMIx convenience routine. The corresponding
macro-style interface for this operation is ``PMIX_VALUE_CREATE``.


EXAMPLES
--------

Create and later release an array of values:

.. code-block:: c

    pmix_value_t *array;

    array = PMIx_Value_create(4);
    if (NULL == array) {
        /* handle allocation failure */
    }

    /* ... use the array ... */

    PMIx_Value_free(array, 4);


.. seealso::
   :ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`,
   :ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>`,
   :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`,
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`PMIx_Value_xfer(3) <man3-PMIx_Value_xfer>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
