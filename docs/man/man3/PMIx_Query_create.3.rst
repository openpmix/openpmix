.. _man3-PMIx_Query_create:

PMIx_Query_create
=================

.. include_body

``PMIx_Query_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_query_t(5) <man5-pmix_query_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_query_t* PMIx_Query_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_query_t(5) <man5-pmix_query_t>` structures to
  allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_query_t <man5-pmix_query_t>`
structures on the heap and initialize each element by calling
:ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>` on it. The returned
array is owned by the caller and must eventually be released with
:ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>` (passing the same count
``n``) or, for a single-element array, with :ref:`PMIx_Query_release(3)
<man3-PMIx_Query_release>`.

If ``n`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated and initialized array of
``pmix_query_t`` structures, or ``NULL`` if ``n`` is zero or the allocation
fails.


NOTES
-----

The ``PMIX_QUERY_CREATE`` convenience macro is a direct wrapper for this
function; it assigns the returned pointer to its first argument.


EXAMPLES
--------

Create and later free an array of query structures:

.. code-block:: c

    pmix_query_t *queries;

    queries = PMIx_Query_create(2);
    /* ... populate queries[0], queries[1] ... */
    PMIx_Query_free(queries, 2);


.. seealso::
   :ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>`,
   :ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>`,
   :ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>`,
   :ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>`,
   :ref:`PMIx_Query_qualifiers_create(3) <man3-PMIx_Query_qualifiers_create>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`
