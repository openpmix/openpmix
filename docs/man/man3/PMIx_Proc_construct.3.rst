.. _man3-PMIx_Proc_construct:

PMIx_Proc_construct
===================

.. include_body

``PMIx_Proc_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Proc_construct(pmix_proc_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure to be
  initialized. The storage for the structure itself must already exist |mdash|
  it is supplied by the caller (typically declared on the stack or embedded
  within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_proc_t <man5-pmix_proc_t>` that the caller
has already allocated. The function zeroes the entire structure |mdash| clearing
the ``nspace`` field |mdash| and sets the ``rank`` field to ``PMIX_RANK_UNDEF``.
No heap memory is allocated.

A proc structure should be constructed (or created with
:ref:`PMIx_Proc_create(3) <man3-PMIx_Proc_create>`) before its fields are
referenced; using an uninitialized structure produces undefined behavior.

Because ``PMIx_Proc_construct`` operates on caller-provided storage, it must be
paired with :ref:`PMIx_Proc_destruct(3) <man3-PMIx_Proc_destruct>`. Do **not**
pass a constructed (as opposed to created) structure to
:ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>`, as that would attempt to free
storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Proc_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Proc_construct`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_CONSTRUCT`` macro is implemented as a direct call to this function.


EXAMPLES
--------

Construct a structure and load it with a process identifier:

.. code-block:: c

    pmix_proc_t proc;

    PMIx_Proc_construct(&proc);
    PMIx_Proc_load(&proc, "myapp.job", 0);


.. seealso::
   :ref:`PMIx_Proc_destruct(3) <man3-PMIx_Proc_destruct>`,
   :ref:`PMIx_Proc_create(3) <man3-PMIx_Proc_create>`,
   :ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>`,
   :ref:`PMIx_Proc_load(3) <man3-PMIx_Proc_load>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
