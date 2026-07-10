.. _man3-PMIx_Query_release:

PMIx_Query_release
==================

.. include_body

``PMIx_Query_release`` |mdash| Release a single
:ref:`pmix_query_t(5) <man5-pmix_query_t>` structure and its contents.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Query_release(pmix_query_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the single :ref:`pmix_query_t(5) <man5-pmix_query_t>`
  structure to be released.


DESCRIPTION
-----------

Release a single heap-allocated :ref:`pmix_query_t <man5-pmix_query_t>`
structure. The function destructs the structure |mdash| freeing its ``keys``
and ``qualifiers`` contents |mdash| and then frees the storage of the structure
itself. It is exactly equivalent to calling :ref:`PMIx_Query_free(3)
<man3-PMIx_Query_free>` with a count of ``1``.

If ``p`` is ``NULL``, the function does nothing.


RETURN VALUE
------------

``PMIx_Query_release`` returns no value (``void``).


NOTES
-----

The ``PMIX_QUERY_RELEASE`` convenience macro is a direct wrapper for this
function; in addition to calling it, the macro sets the caller's pointer to
``NULL``.

``PMIx_Query_release`` frees the structure storage and must therefore be used
only on storage the library allocated (for example, the single-element array
returned by :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>` with
``n == 1``). For a caller-provided (stack or embedded) structure, use
:ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>` instead.


.. seealso::
   :ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>`,
   :ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>`,
   :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`,
   :ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>`,
   :ref:`PMIx_Query_qualifiers_create(3) <man3-PMIx_Query_qualifiers_create>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`
