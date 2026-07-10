.. _man3-PMIx_Argv_free:

PMIx_Argv_free
==============

.. include_body

``PMIx_Argv_free`` |mdash| Release a NULL-terminated ``argv``-style array and
all of the strings it contains.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Argv_free(char **argv);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``argv``: Pointer to a NULL-terminated ``argv``-style array, or ``NULL``.


DESCRIPTION
-----------

Free every string in the array ``argv`` and then free the array itself. The
routine walks the array from its first element to the terminating ``NULL``,
releasing each contained string, and finally releases the block that holds
the pointers.

``PMIx_Argv_free`` is the correct disposal routine for any array produced or
grown by the ``PMIx_Argv_*`` family |mdash|
:ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
:ref:`PMIx_Argv_prepend_nosize(3) <man3-PMIx_Argv_prepend_nosize>`,
:ref:`PMIx_Argv_append_unique_nosize(3) <man3-PMIx_Argv_append_unique_nosize>`,
:ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
:ref:`PMIx_Argv_split_with_empty(3) <man3-PMIx_Argv_split_with_empty>`,
:ref:`PMIx_Argv_split_inter(3) <man3-PMIx_Argv_split_inter>`, and
:ref:`PMIx_Argv_copy(3) <man3-PMIx_Argv_copy>` |mdash| because those routines
allocate both the array and the strings it holds.

Passing ``NULL`` is safe and does nothing. After the call the array pointer is
invalid and must not be reused.


RETURN VALUE
------------

``PMIx_Argv_free`` returns no value (``void``).


NOTES
-----

Do not use ``PMIx_Argv_free`` on an array whose strings were not allocated by
the library (for example, a stack array of string literals); doing so passes
non-heap pointers to ``free`` and produces undefined behavior.


.. seealso::
   :ref:`PMIx_Argv_append_nosize(3) <man3-PMIx_Argv_append_nosize>`,
   :ref:`PMIx_Argv_prepend_nosize(3) <man3-PMIx_Argv_prepend_nosize>`,
   :ref:`PMIx_Argv_append_unique_nosize(3) <man3-PMIx_Argv_append_unique_nosize>`,
   :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
   :ref:`PMIx_Argv_copy(3) <man3-PMIx_Argv_copy>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`
