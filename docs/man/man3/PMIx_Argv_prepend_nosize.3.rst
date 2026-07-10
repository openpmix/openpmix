.. _man3-PMIx_Argv_prepend_nosize:

PMIx_Argv_prepend_nosize
========================

.. include_body

``PMIx_Argv_prepend_nosize`` |mdash| Insert a copy of a string at the front
of a NULL-terminated ``argv``-style array.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Argv_prepend_nosize(char ***argv, const char *arg);


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

* ``arg``: The string to prepend. The routine stores a private copy; the
  caller retains ownership of ``arg``.


DESCRIPTION
-----------

Insert the string ``arg`` at position zero of the array referenced by
``*argv``, shifting every existing entry up by one position.

If ``*argv`` is ``NULL``, a new two-element array is allocated holding a copy
of ``arg`` followed by the terminating ``NULL``. If ``*argv`` already points
to an array, the array is reallocated one entry larger, the existing entries
are moved down one slot to make room, and a copy of ``arg`` (made with
``strdup``) is stored in element zero. ``*argv`` is updated to the resulting
array pointer, which the caller must use in place of any prior value because
reallocation may move the array.

This routine is the front-insertion counterpart to
:ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`.


RETURN VALUE
------------

Returns one of:

* ``PMIX_SUCCESS`` The string was prepended successfully.

* ``PMIX_ERR_OUT_OF_RESOURCE`` Memory could not be allocated for the array.


EXAMPLES
--------

.. code-block:: c

   char **argv = NULL;

   PMIx_Argv_append_nosize(&argv, "b");
   PMIx_Argv_prepend_nosize(&argv, "a");
   /* argv == { "a", "b", NULL } */

   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
   :ref:`PMIx_Argv_append_unique_nosize(3) <man3-PMIx_Argv_append_unique_nosize>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_copy(3) <man3-PMIx_Argv_copy>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
