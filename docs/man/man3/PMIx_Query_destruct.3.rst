.. _man3-PMIx_Query_destruct:

PMIx_Query_destruct
===================

.. include_body

``PMIx_Query_destruct`` |mdash| Release the contents of a
:ref:`pmix_query_t(5) <man5-pmix_query_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Query_destruct(pmix_query_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_query_t(5) <man5-pmix_query_t>` structure
  whose contents are to be released.


DESCRIPTION
-----------

Release the dynamically allocated contents of a
:ref:`pmix_query_t <man5-pmix_query_t>` structure. The function frees the
``keys`` string array (if present) and frees the array of ``qualifiers``
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structures (if present), then resets
those pointers to ``NULL`` and sets ``nqual`` to zero.

``PMIx_Query_destruct`` releases only the payload the structure references; it
does **not** free the storage of the ``pmix_query_t`` structure itself. It is
therefore the correct counterpart to
:ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>` for structures whose
storage was supplied by the caller (stack or embedded).


RETURN VALUE
------------

``PMIx_Query_destruct`` returns no value (``void``).


NOTES
-----

The ``PMIX_QUERY_DESTRUCT`` convenience macro is a direct wrapper for this
function.

To release an array of query structures allocated by
:ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>` |mdash| including the
array storage itself |mdash| use :ref:`PMIx_Query_free(3)
<man3-PMIx_Query_free>` or :ref:`PMIx_Query_release(3)
<man3-PMIx_Query_release>` instead.


.. seealso::
   :ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>`,
   :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`,
   :ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>`,
   :ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>`,
   :ref:`PMIx_Query_qualifiers_create(3) <man3-PMIx_Query_qualifiers_create>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`
