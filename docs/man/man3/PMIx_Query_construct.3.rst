.. _man3-PMIx_Query_construct:

PMIx_Query_construct
====================

.. include_body

``PMIx_Query_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_query_t(5) <man5-pmix_query_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Query_construct(pmix_query_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_query_t(5) <man5-pmix_query_t>` structure to
  be initialized. The storage for the structure itself must already exist
  |mdash| it is supplied by the caller (typically declared on the stack or
  embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_query_t <man5-pmix_query_t>` that the
caller has already allocated. The function zeroes the entire structure,
clearing the ``keys`` and ``qualifiers`` pointers and setting ``nqual`` to
zero. No heap memory is allocated, and no keys or qualifiers are attached.

A query structure must be constructed (or created with
:ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`) before its fields are
populated and it is passed to a query operation such as
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`.

Because ``PMIx_Query_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>` to
release any ``keys`` or ``qualifiers`` the structure subsequently acquires. Do
**not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>`, as that would attempt
to free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Query_construct`` returns no value (``void``).


NOTES
-----

The ``PMIX_QUERY_CONSTRUCT`` convenience macro is a direct wrapper for this
function.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_query_t query;

    PMIx_Query_construct(&query);


.. seealso::
   :ref:`PMIx_Query_destruct(3) <man3-PMIx_Query_destruct>`,
   :ref:`PMIx_Query_create(3) <man3-PMIx_Query_create>`,
   :ref:`PMIx_Query_free(3) <man3-PMIx_Query_free>`,
   :ref:`PMIx_Query_release(3) <man3-PMIx_Query_release>`,
   :ref:`PMIx_Query_qualifiers_create(3) <man3-PMIx_Query_qualifiers_create>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_query_t(5) <man5-pmix_query_t>`
