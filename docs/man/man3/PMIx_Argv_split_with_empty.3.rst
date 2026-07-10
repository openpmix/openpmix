.. _man3-PMIx_Argv_split_with_empty:

PMIx_Argv_split_with_empty
==========================

.. include_body

``PMIx_Argv_split_with_empty`` |mdash| Split a string on a delimiter into a
NULL-terminated ``argv``-style array, retaining empty tokens.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char **PMIx_Argv_split_with_empty(const char *src_string, int delimiter);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``src_string``: The string to tokenize. May be ``NULL`` or empty.

* ``delimiter``: The character on which to split, passed as an ``int``.


DESCRIPTION
-----------

Break ``src_string`` into tokens separated by ``delimiter`` and return the
tokens as a freshly allocated, NULL-terminated ``argv``-style array. Each
token is an independently allocated copy of the corresponding substring.

Unlike :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`, zero-length tokens
are preserved: each run of consecutive delimiters, and a leading delimiter,
yields an empty-string (``""``) entry in the result, so the number of entries
reflects the delimiter structure of the input. This routine is a thin wrapper
over :ref:`PMIx_Argv_split_inter(3) <man3-PMIx_Argv_split_inter>` with
``include_empty = true``.

The returned array and all of its strings are owned by the caller and must be
released with :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`.


RETURN VALUE
------------

Returns a newly allocated NULL-terminated array of token strings, including
empty-string entries for zero-length tokens. If ``src_string`` is ``NULL`` or
empty, the returned pointer is ``NULL``. ``NULL`` is also returned if an
internal allocation fails.


EXAMPLES
--------

.. code-block:: c

   char **argv = PMIx_Argv_split_with_empty("a::b", ':');
   /* argv == { "a", "", "b", NULL } -- empty token retained */

   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_split(3) <man3-PMIx_Argv_split>`,
   :ref:`PMIx_Argv_split_inter(3) <man3-PMIx_Argv_split_inter>`,
   :ref:`PMIx_Argv_join(3) <man3-PMIx_Argv_join>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
