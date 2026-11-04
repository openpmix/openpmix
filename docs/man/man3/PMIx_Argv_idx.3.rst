.. _man3-PMIx_Argv_idx:

PMIx_Argv_idx
=============

.. include_body

``PMIx_Argv_idx`` |mdash| Find a string in a NULL-terminated
``argv``-style array of strings.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   int PMIx_Argv_idx(char **argv, const char *arg);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``argv``: Pointer to a NULL-terminated array of ``char *`` strings, or
  ``NULL``.

* ``arg``: The string to look for, or ``NULL``.


DESCRIPTION
-----------

Walk the ``argv``-style array ``argv`` from its first element and return
the position of the first entry whose contents are identical to ``arg``,
as compared by ``strcmp(3)``.

A ``NULL`` array is treated as an empty one, so a list that is built only
when its first entry is added can be searched before that happens. A
``NULL`` string matches no entry.

Neither the array nor any of its strings is modified. The function does
not allocate memory and does not interact with the PMIx progress thread,
so it is safe to call from any context, including before ``PMIx_Init``.

A caller can tell whether the library provides this function from the
``PMIX_CAP_ARGV_IDX`` capability flag in ``pmix_version.h``.


RETURN VALUE
------------

Returns the zero-based index of the first matching entry, or ``-1`` if
there is none - including when ``argv`` or ``arg`` is ``NULL``.


EXAMPLES
--------

.. code-block:: c

   char **argv = NULL;

   PMIx_Argv_append_nosize(&argv, "alpha");
   PMIx_Argv_append_nosize(&argv, "beta");

   int n = PMIx_Argv_idx(argv, "beta");    /* n == 1 */
   int m = PMIx_Argv_idx(argv, "gamma");   /* m == -1 */
   int k = PMIx_Argv_idx(NULL, "alpha");   /* k == -1 */

   if (0 <= PMIx_Argv_idx(argv, "alpha")) {
       /* "alpha" is present */
   }


.. seealso::
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
   :ref:`PMIx_Argv_append_unique_nosize(3) <man3-PMIx_Argv_append_unique_nosize>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
