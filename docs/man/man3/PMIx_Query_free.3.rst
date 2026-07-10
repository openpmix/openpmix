.. _man3-PMIx_Query_free:

PMIx_Query_free
===============

.. include_body

``PMIx_Query_free`` |mdash| Release an array of
:ref:`pmix_query_t(5) <man5-pmix_query_t>` structures and their contents.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Query_free(pmix_query_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the array of :ref:`pmix_query_t(5) <man5-pmix_query_t>`
  structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_query_t <man5-pmix_query_t>` structures that was
allocated by :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`. The function
first destructs each of the ``n`` elements |mdash| freeing their ``keys`` and
``qualifiers`` contents, exactly as :ref:`PMIx_Query_destruct(3)
<man3-PMIx_Query_destruct>` would |mdash| and then frees the storage of the
array itself.

If ``p`` is ``NULL`` or ``n`` is zero, the function does nothing.


RETURN VALUE
------------

``PMIx_Query_free`` returns no value (``void``).


NOTES
-----

The ``PMIX_QUERY_FREE`` convenience macro is a direct wrapper for this
function; in addition to calling it, the macro sets the caller's pointer to
``NULL``.

Use ``PMIx_Query_free`` only on storage obtained from
:ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`. For a caller-provided
(stack or embedded) structure, use :ref:`PMIx_Query_destruct(3)
<man3-PMIx_Query_destruct>` instead, which releases the contents without
freeing the structure storage.


.. seealso::
   :ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>`,
   :ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>`,
   :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`,
   :ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>`,
   :ref:`PMIx_Query_qualifiers_create(3) <man3-PMIx_Query_qualifiers_create>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`
