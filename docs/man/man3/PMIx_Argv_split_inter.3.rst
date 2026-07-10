.. _man3-PMIx_Argv_split_inter:

PMIx_Argv_split_inter
=====================

.. include_body

``PMIx_Argv_split_inter`` |mdash| Split a string on a delimiter into a
NULL-terminated ``argv``-style array, optionally retaining empty tokens.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char **PMIx_Argv_split_inter(const char *src_string,
                                int delimiter,
                                bool include_empty);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``src_string``: The string to tokenize. May be ``NULL`` or empty.

* ``delimiter``: The character on which to split, passed as an ``int``.

* ``include_empty``: If ``true``, zero-length tokens are retained as
  empty-string (``""``) entries; if ``false``, they are discarded.


DESCRIPTION
-----------

Break ``src_string`` into tokens separated by ``delimiter`` and return the
tokens as a freshly allocated, NULL-terminated ``argv``-style array. Each
token is an independently allocated copy of the corresponding substring.

This is the common backing routine for
:ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>` (which calls it with
``include_empty = false``) and
:ref:`PMIx_Argv_split_with_empty(3) <man3-PMIx_Argv_split_with_empty>` (which
calls it with ``include_empty = true``). Call it directly when the
empty-token policy is chosen at run time. The ``include_empty`` argument
controls whether runs of consecutive delimiters, and a leading delimiter,
generate empty-string entries.

The returned array and all of its strings are owned by the caller and must be
released with :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`.


RETURN VALUE
------------

Returns a newly allocated NULL-terminated array of token strings. If
``src_string`` is ``NULL`` or empty |mdash| or contains no tokens once the
``include_empty`` policy is applied |mdash| the returned pointer is ``NULL``.
``NULL`` is also returned if an internal allocation fails.


EXAMPLES
--------

.. code-block:: c

   bool keep = user_wants_empties();
   char **argv = PMIx_Argv_split_inter("a::b", ':', keep);
   /* keep == true  -> { "a", "", "b", NULL } */
   /* keep == false -> { "a", "b", NULL }     */

   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
   :ref:`PMIx_Argv_split_with_empty(3) <man3-PMIx_Argv_split_with_empty>`,
   :ref:`PMIx_Argv_join(3) <man3-PMIx_Argv_join>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
