.. _man3-PMIx_Proc_info_create:

PMIx_Proc_info_create
=====================

.. include_body

``PMIx_Proc_info_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_proc_info_t* PMIx_Proc_info_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>` structures
  to allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_proc_info_t
<man5-pmix_proc_info_t>` structures on the heap and construct each element
|mdash| zeroing the structure and setting ``state`` to ``PMIX_PROC_STATE_UNDEF``
|mdash| exactly as :ref:`PMIx_Proc_info_construct(3)
<man3-PMIx_Proc_info_construct>` would.

The returned array is owned by the caller and must be released with
:ref:`PMIx_Proc_info_free(3) <man3-PMIx_Proc_info_free>`, which destructs each
element (freeing any ``hostname`` and ``executable_name`` strings) and frees the
underlying storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of :ref:`pmix_proc_info_t
<man5-pmix_proc_info_t>` structures, or ``NULL`` if ``n`` is zero or the
allocation fails.


NOTES
-----

``PMIx_Proc_info_create`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_INFO_CREATE`` macro assigns the returned pointer to its first
argument and is implemented as a call to this function.


EXAMPLES
--------

Allocate an array of proc info structures and release it when done:

.. code-block:: c

    pmix_proc_info_t *pinfo;
    size_t ninfo = 2;

    pinfo = PMIx_Proc_info_create(ninfo);

    /* ... populate and use the array ... */

    PMIx_Proc_info_free(pinfo, ninfo);


.. seealso::
   :ref:`PMIx_Proc_info_free(3) <man3-PMIx_Proc_info_free>`,
   :ref:`PMIx_Proc_info_construct(3) <man3-PMIx_Proc_info_construct>`,
   :ref:`PMIx_Proc_info_destruct(3) <man3-PMIx_Proc_info_destruct>`,
   :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
