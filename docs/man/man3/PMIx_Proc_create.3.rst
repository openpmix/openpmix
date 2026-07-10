.. _man3-PMIx_Proc_create:

PMIx_Proc_create
================

.. include_body

``PMIx_Proc_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_proc_t* PMIx_Proc_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_proc_t <man5-pmix_proc_t>`
structures on the heap and construct each element |mdash| zeroing the ``nspace``
field and setting ``rank`` to ``PMIX_RANK_UNDEF`` |mdash| exactly as
:ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>` would.

The returned array is owned by the caller and must be released with
:ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>`, which destructs each element and
frees the underlying storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of :ref:`pmix_proc_t
<man5-pmix_proc_t>` structures, or ``NULL`` if ``n`` is zero or the allocation
fails.


NOTES
-----

``PMIx_Proc_create`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_CREATE`` macro assigns the returned pointer to its first argument
and is implemented as a call to this function.


EXAMPLES
--------

Allocate an array of proc structures and release it when done:

.. code-block:: c

    pmix_proc_t *procs;
    size_t nprocs = 4;

    procs = PMIx_Proc_create(nprocs);

    /* ... populate and use the array ... */

    PMIx_Proc_free(procs, nprocs);


.. seealso::
   :ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>`,
   :ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>`,
   :ref:`PMIx_Proc_destruct(3) <man3-PMIx_Proc_destruct>`,
   :ref:`PMIx_Proc_load(3) <man3-PMIx_Proc_load>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
