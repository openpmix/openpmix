.. _man3-PMIx_Cpuset_create:

PMIx_Cpuset_create
==================

.. include_body

``PMIx_Cpuset_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_cpuset_t* PMIx_Cpuset_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_cpuset_t <man5-pmix_cpuset_t>`
structures on the heap and construct each element (zeroing its ``source`` and
``bitmap`` fields).

If ``n`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated and initialized array, or ``NULL`` if
``n`` is zero or the allocation fails.


NOTES
-----

An array obtained from ``PMIx_Cpuset_create`` must be released with
:ref:`PMIx_Cpuset_free(3) <man3-PMIx_Cpuset_free>`, which destructs every
element and frees the array storage itself. Pass the same count ``n`` used at
creation.

The convenience macro ``PMIX_CPUSET_CREATE`` is provided as a wrapper around
this function.


EXAMPLES
--------

Create and later release an array of cpusets:

.. code-block:: c

    pmix_cpuset_t *array;

    array = PMIx_Cpuset_create(2);
    /* ... populate and use the cpusets ... */
    PMIx_Cpuset_free(array, 2);


.. seealso::
   :ref:`PMIx_Cpuset_free(3) <man3-PMIx_Cpuset_free>`,
   :ref:`PMIx_Cpuset_construct(3) <man3-PMIx_Cpuset_construct>`,
   :ref:`PMIx_Cpuset_destruct(3) <man3-PMIx_Cpuset_destruct>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
