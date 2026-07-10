.. _man3-PMIx_Argv_append_nosize:

PMIx_Argv_append_nosize
=======================

.. include_body

``PMIx_Argv_append_nosize`` |mdash| Append a copy of a string to the end of
a NULL-terminated ``argv``-style array.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Argv_append_nosize(char ***argv, const char *arg);


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

* ``arg``: The string to append. The routine stores a private copy; the
  caller retains ownership of ``arg``.


DESCRIPTION
-----------

Append the string ``arg`` to the end of the array referenced by ``*argv``.

If ``*argv`` is ``NULL``, a new two-element array is allocated: element zero
holds the copied string and element one is the terminating ``NULL``. If
``*argv`` already points to an array, the array is reallocated one entry
larger, a copy of ``arg`` (made with ``strdup``) is stored in the newly
opened slot, and a fresh terminating ``NULL`` is written after it. In both
cases ``*argv`` is updated to the resulting array pointer, which the caller
must use in place of any prior value because reallocation may move the array.

Only the string ``arg`` is duplicated; the routine takes ownership of the
copy it makes, and that copy is released when the array is later passed to
:ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`.


RETURN VALUE
------------

Returns one of:

* ``PMIX_SUCCESS`` The string was appended successfully.

* ``PMIX_ERR_OUT_OF_RESOURCE`` Memory could not be allocated for the array or
  the copied string.


NOTES
-----

The ``_nosize`` suffix indicates that, unlike the internal
``pmix_argv_append`` routine, this function does not accept or update a
separate element-count argument. Use :ref:`PMIx_Argv_count(3)
<man3-PMIx_Argv_count>` when the current number of entries is needed.


EXAMPLES
--------

.. code-block:: c

   char **argv = NULL;

   PMIx_Argv_append_nosize(&argv, "first");
   PMIx_Argv_append_nosize(&argv, "second");
   /* argv == { "first", "second", NULL } */

   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_prepend_nosize(3) <man3-PMIx_Argv_prepend_nosize>`,
   :ref:`PMIx_Argv_append_unique_nosize(3) <man3-PMIx_Argv_append_unique_nosize>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_copy(3) <man3-PMIx_Argv_copy>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
