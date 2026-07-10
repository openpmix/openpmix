.. _man3-PMIx_Argv_join:

PMIx_Argv_join
==============

.. include_body

``PMIx_Argv_join`` |mdash| Concatenate the entries of a NULL-terminated
``argv``-style array into a single newly allocated string.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char *PMIx_Argv_join(char **argv, int delimiter);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``argv``: Pointer to a NULL-terminated ``argv``-style array, or ``NULL``.

* ``delimiter``: The character inserted between successive entries, passed as
  an ``int`` and stored as a single ``char``.


DESCRIPTION
-----------

Join every entry of ``argv`` into one freshly allocated string, placing a
single ``delimiter`` character between each pair of adjacent entries. No
delimiter is added before the first entry or after the last, and the result
is NUL-terminated.

This is the inverse of the split routines: joining with a given delimiter and
splitting the result on that same delimiter round-trips an array that
contains no embedded delimiter characters and no empty entries.

The returned string is owned by the caller and must be released with
``free``.


RETURN VALUE
------------

Returns a newly allocated, NUL-terminated string. If ``argv`` is ``NULL`` or
its first element is ``NULL`` (an empty array), a newly allocated empty
string (``""``) is returned rather than ``NULL``. ``NULL`` is returned only if
the allocation of the result string fails.


NOTES
-----

Because a non-``NULL`` result is always returned for the empty-array case, the
caller must still ``free`` the returned pointer in that case. The returned
string is an ordinary heap allocation |mdash| free it with ``free``, not with
:ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`, which is for arrays.


EXAMPLES
--------

.. code-block:: c

   char **argv = PMIx_Argv_split("a:b:c", ':');
   char  *str  = PMIx_Argv_join(argv, '/');
   /* str == "a/b/c" */

   free(str);
   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
   :ref:`PMIx_Argv_split_with_empty(3) <man3-PMIx_Argv_split_with_empty>`,
   :ref:`PMIx_Argv_split_inter(3) <man3-PMIx_Argv_split_inter>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
