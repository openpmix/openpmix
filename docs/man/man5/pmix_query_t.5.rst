.. _man5-pmix_query_t:

pmix_query_t
============

.. include_body

`pmix_query_t` |mdash| Describes a single query operation

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_query {
       char **keys;
       pmix_info_t *qualifiers;
       size_t nqual;
   } pmix_query_t;


DESCRIPTION
-----------

A PMIx query structure is composed of one or more keys and a list of qualifiers
which provide additional information to describe the query. Keys which use the
same qualifiers can be placed in the same query for compactness, though it is
permissible to put each key in its own query.

The `pmix_query_t` structure is used by the
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>` API to describe a single query
operation.

The fields are:

``keys``
   A ``NULL``-terminated, argv-style array of strings identifying the information
   being requested.

``qualifiers``
   An array of :ref:`pmix_info_t <man5-pmix_info_t>` structures describing
   constraints on the query.

``nqual``
   The number of elements in the ``qualifiers`` array.

STATIC INITIALIZER
------------------

A statically declared ``pmix_query_t`` may be initialized with the
``PMIX_QUERY_STATIC_INIT`` macro, which sets ``keys`` and ``qualifiers`` to ``NULL`` and ``nqual`` to ``0``:

.. code-block:: c

   pmix_query_t query = PMIX_QUERY_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
