.. _man3-PMIx_Argv_copy:

PMIx_Argv_copy
==============

.. include_body

``PMIx_Argv_copy`` |mdash| Produce a deep copy of a NULL-terminated
``argv``-style array.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char **PMIx_Argv_copy(char **argv);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``argv``: Pointer to a NULL-terminated ``argv``-style array, or ``NULL``.


DESCRIPTION
-----------

Create and return an independent deep copy of ``argv``. A new array is
allocated and each contained string is duplicated into it, so the copy shares
no storage with the original; modifying or freeing one array has no effect on
the other. The duplicated entries preserve the order and content of the
source, and the new array is NULL-terminated.

If ``argv`` points to a valid but empty array (its first element is
``NULL``), a valid empty array is returned rather than ``NULL``.

The returned array and all of its strings are owned by the caller and must be
released with :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`.


RETURN VALUE
------------

Returns a newly allocated NULL-terminated deep copy of the array. Returns
``NULL`` if ``argv`` is ``NULL``, or if an internal allocation fails during
the copy.


EXAMPLES
--------

.. code-block:: c

   char **argv = PMIx_Argv_split("a:b:c", ':');
   char **dup  = PMIx_Argv_copy(argv);

   PMIx_Argv_free(argv);   /* dup remains valid */
   PMIx_Argv_free(dup);


.. seealso::
   :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
   :ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
