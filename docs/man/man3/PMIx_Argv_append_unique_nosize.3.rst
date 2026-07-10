.. _man3-PMIx_Argv_append_unique_nosize:

PMIx_Argv_append_unique_nosize
==============================

.. include_body

``PMIx_Argv_append_unique_nosize`` |mdash| Append a copy of a string to a
NULL-terminated ``argv``-style array only if it is not already present.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Argv_append_unique_nosize(char ***argv, const char *arg);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``argv``: Address of a pointer to a NULL-terminated ``argv``-style array.
  On input, ``*argv`` may be ``NULL`` (in which case a new array is created)
  or point to an existing array. On output, ``*argv`` is updated to point at
  the (possibly reallocated) array.

INPUT PARAMETERS
----------------

* ``arg``: The string to append. The routine stores a private copy only if
  the string is not already present; the caller retains ownership of ``arg``.


DESCRIPTION
-----------

Append the string ``arg`` to the array referenced by ``*argv`` only if no
existing entry compares equal to it.

The routine scans the current entries and compares each against ``arg`` using
``strcmp``. If an exact match is found, the array is left unchanged and the
call succeeds. Otherwise, or when ``*argv`` is ``NULL``, the string is
appended exactly as by :ref:`PMIx_Argv_append_nosize(3)
<man3-PMIx_Argv_append_nosize>` |mdash| the array is reallocated, a private
copy of ``arg`` is stored, and a new terminating ``NULL`` is written.
``*argv`` is updated to the resulting array pointer.

Comparison is by full string equality; matching is case-sensitive.


RETURN VALUE
------------

Returns one of:

* ``PMIX_SUCCESS`` The string was appended, or was already present so no
  change was needed.

* ``PMIX_ERR_OUT_OF_RESOURCE`` Memory could not be allocated while appending
  a new entry.


EXAMPLES
--------

.. code-block:: c

   char **argv = NULL;

   PMIx_Argv_append_unique_nosize(&argv, "x");
   PMIx_Argv_append_unique_nosize(&argv, "x");   /* no-op, still one entry */
   /* argv == { "x", NULL } */

   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
   :ref:`PMIx_Argv_prepend_nosize(3) <man3-PMIx_Argv_prepend_nosize>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_copy(3) <man3-PMIx_Argv_copy>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
