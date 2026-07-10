.. _man3-PMIx_Proc_load:

PMIx_Proc_load
==============

.. include_body

``PMIx_Proc_load`` |mdash| Initialize a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
with a namespace and rank.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Proc_load(pmix_proc_t *p,
                       const char *nspace, pmix_rank_t rank);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure to be
  loaded. The storage for the structure itself must already exist (caller
  supplied).
* ``nspace``: Namespace string to copy into the structure's ``nspace`` field
  (see :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`). May be ``NULL``, in which
  case the ``nspace`` field is left cleared.
* ``rank``: Process rank to store in the structure's ``rank`` field
  (see :ref:`pmix_rank_t(5) <man5-pmix_rank_t>`).


DESCRIPTION
-----------

Initialize a :ref:`pmix_proc_t <man5-pmix_proc_t>` and populate it with a process
identifier. The function first constructs the structure |mdash| exactly as
:ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>` would, zeroing the
``nspace`` field |mdash| then copies the supplied ``nspace`` string into the
``nspace`` field and stores ``rank`` in the ``rank`` field.

The namespace string is copied with truncation: at most ``PMIX_MAX_NSLEN``
characters are stored, and the result is always null-terminated. If ``nspace``
is ``NULL``, only the ``rank`` field is set (the ``nspace`` field remains
cleared).


RETURN VALUE
------------

``PMIx_Proc_load`` returns no value (``void``).


NOTES
-----

``PMIx_Proc_load`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_LOAD`` macro is implemented as a direct call to this function.

Because ``PMIx_Proc_load`` re-constructs the structure before loading it, any
previous contents are discarded. As a :ref:`pmix_proc_t <man5-pmix_proc_t>`
owns no heap-allocated payload, this is safe to call on a previously loaded
structure without leaking memory.


EXAMPLES
--------

Load a proc structure with a namespace and rank:

.. code-block:: c

    pmix_proc_t proc;

    PMIx_Proc_load(&proc, "myapp.job", 3);


.. seealso::
   :ref:`PMIx_Proc_construct(3) <man3-PMIx_Proc_construct>`,
   :ref:`PMIx_Proc_destruct(3) <man3-PMIx_Proc_destruct>`,
   :ref:`PMIx_Proc_create(3) <man3-PMIx_Proc_create>`,
   :ref:`PMIx_Proc_free(3) <man3-PMIx_Proc_free>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`,
   :ref:`pmix_rank_t(5) <man5-pmix_rank_t>`
