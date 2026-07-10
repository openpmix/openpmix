.. _man3-PMIx_Proc_destruct:

PMIx_Proc_destruct
==================

.. include_body

``PMIx_Proc_destruct`` |mdash| Clear the contents of a
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Proc_destruct(pmix_proc_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure to be
  cleared. The storage for the structure itself is not freed.


DESCRIPTION
-----------

Reset the contents of a :ref:`pmix_proc_t <man5-pmix_proc_t>` structure. A
:ref:`pmix_proc_t <man5-pmix_proc_t>` carries no heap-allocated payload |mdash|
its ``nspace`` field is a fixed-size character array and its ``rank`` is a
scalar |mdash| so this function performs the same reset as
:ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>`: it zeroes the ``nspace``
field and sets ``rank`` to ``PMIX_RANK_UNDEF``. No memory is freed.

``PMIx_Proc_destruct`` is the counterpart to
:ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>` and releases only the
contents of the structure, not the structure storage itself. To release an array
of proc structures that was heap-allocated by
:ref:`PMIx_Proc_create(3) <man3-PMIx_Proc_create>`, use
:ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>` instead.


RETURN VALUE
------------

``PMIx_Proc_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Proc_destruct`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_DESTRUCT`` macro is implemented as a direct call to this function.
Because a :ref:`pmix_proc_t <man5-pmix_proc_t>` owns no allocated memory, calling
this function is functionally equivalent to re-constructing the structure.


.. seealso::
   :ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>`,
   :ref:`PMIx_Proc_create(3) <man3-PMIx_Proc_create>`,
   :ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>`,
   :ref:`PMIx_Proc_load(3) <man3-PMIx_Proc_load>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
