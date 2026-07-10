.. _man3-PMIx_Argv_split:

PMIx_Argv_split
===============

.. include_body

``PMIx_Argv_split`` |mdash| Split a string on a delimiter into a
NULL-terminated ``argv``-style array, discarding empty tokens.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char **PMIx_Argv_split(const char *src_string, int delimiter);


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

Zero-length tokens are discarded: runs of consecutive delimiters, and any
leading or trailing delimiter, produce no empty entries in the result. To
retain empty tokens instead, use
:ref:`PMIx_Argv_split_with_empty(3) <man3-PMIx_Argv_split_with_empty>`. Both
routines are thin wrappers over
:ref:`PMIx_Argv_split_inter(3) <man3-PMIx_Argv_split_inter>`; this one passes
``include_empty = false``.

The returned array and all of its strings are owned by the caller and must be
released with :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`.


RETURN VALUE
------------

Returns a newly allocated NULL-terminated array of token strings. If
``src_string`` is ``NULL`` or contains no non-empty tokens, the returned
pointer is ``NULL`` (an :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>` of
zero). ``NULL`` is also returned if an internal allocation fails.


EXAMPLES
--------

.. code-block:: c

   char **argv = PMIx_Argv_split("a::b:", ':');
   /* argv == { "a", "b", NULL } -- empty tokens dropped */

   PMIx_Argv_free(argv);


.. seealso::
   :ref:`PMIx_Argv_split_with_empty(3) <man3-PMIx_Argv_split_with_empty>`,
   :ref:`PMIx_Argv_split_inter(3) <man3-PMIx_Argv_split_inter>`,
   :ref:`PMIx_Argv_join(3) <man3-PMIx_Argv_join>`,
   :ref:`PMIx_Argv_count(3) <man3-PMIx_Argv_count>`,
   :ref:`PMIx_Argv_free(3) <man3-PMIx_Argv_free>`
