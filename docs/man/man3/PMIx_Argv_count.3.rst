.. _man3-PMIx_Argv_count:

PMIx_Argv_count
===============

.. include_body

``PMIx_Argv_count`` |mdash| Count the number of entries in a
NULL-terminated ``argv``-style array of strings.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   int PMIx_Argv_count(char **a);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``a``: Pointer to a NULL-terminated array of ``char *`` strings, or
  ``NULL``.


DESCRIPTION
-----------

Return the number of string entries contained in the ``argv``-style array
``a``. The count is obtained by walking the array from its first element
until the terminating ``NULL`` pointer is reached; the terminating ``NULL``
itself is not included in the count.

The array is not modified, and none of the contained strings are examined
beyond the pointers themselves.


RETURN VALUE
------------

Returns the number of non-``NULL`` entries in the array. If ``a`` is
``NULL``, the function returns ``0``.


EXAMPLES
--------

.. code-block:: c

   char **argv = NULL;

   PMIx_Argv_append_nosize(&argv, "alpha");
   PMIx_Argv_append_nosize(&argv, "beta");

   int n = PMIx_Argv_count(argv);   /* n == 2 */


.. seealso::
   :ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
   :ref:`PMIx_Argv_prepend_nosize(3) <man3-PMIx_Argv_prepend_nosize>`,
   :ref:`PMIx_Argv_append_unique_nosize(3) <man3-PMIx_Argv_append_unique_nosize>`,
   :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
   :ref:`PMIx_Argv_join(3) <man3-PMIx_Argv_join>`,
   :ref:`PMIx_Argv_copy(3) <man3-PMIx_Argv_copy>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
