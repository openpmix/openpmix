.. _man3-PMIx_Query_qualifiers_create:

PMIx_Query_qualifiers_create
============================

.. include_body

``PMIx_Query_qualifiers_create`` |mdash| Allocate the ``qualifiers`` array of a
:ref:`pmix_query_t(5) <man5-pmix_query_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Query_qualifiers_create(pmix_query_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_query_t(5) <man5-pmix_query_t>` structure
  whose ``qualifiers`` array is to be allocated.
* ``n``: Number of :ref:`pmix_info_t(5) <man5-pmix_info_t>` qualifier
  structures to allocate.


DESCRIPTION
-----------

Allocate and initialize an array of ``n`` :ref:`pmix_info_t(5)
<man5-pmix_info_t>` structures and attach it to the ``qualifiers`` field of the
supplied :ref:`pmix_query_t <man5-pmix_query_t>`, recording the count in the
structure's ``nqual`` field. The new qualifier array is constructed (zeroed)
and is owned by the query structure; it is released when the query structure is
destructed or freed.

Qualifiers refine a query by attaching directive key-value pairs (for example,
restricting a query to a particular namespace or session). After creating the
array, the caller loads each element with the desired key and value before
passing the query to :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`.

The target ``pmix_query_t`` must already have been initialized with
:ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>` or
:ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`.


RETURN VALUE
------------

``PMIx_Query_qualifiers_create`` returns no value (``void``).


NOTES
-----

The ``PMIX_QUERY_QUALIFIERS_CREATE`` convenience macro is a direct wrapper for
this function.

The qualifier array becomes part of the query structure's contents; do not free
it independently. Releasing the query structure with
:ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>`,
:ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>`, or
:ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>` also releases the
qualifiers.


EXAMPLES
--------

Attach a single qualifier to a query:

.. code-block:: c

    pmix_query_t query;

    PMIx_Query_construct(&query);
    PMIx_Query_qualifiers_create(&query, 1);
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_NSPACE,
                   "myjob", PMIX_STRING);


.. seealso::
   :ref:`PMIx_Query_construct(3) <man3-PMIx_Query_construct>`,
   :ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>`,
   :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`,
   :ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>`,
   :ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
