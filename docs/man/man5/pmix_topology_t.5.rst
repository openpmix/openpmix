.. _man5-pmix_topology_t:

pmix_topology_t
===============

.. include_body

`pmix_topology_t` |mdash| Contains the topology for a given node

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct {
       char *source;
       void *topology;
   } pmix_topology_t;


DESCRIPTION
-----------

The `pmix_topology_t` structure contains a case-insensitive string identifying
the source of the topology and a pointer to the corresponding
implementation-specific topology description for a given node.

The ``source`` field is a string naming the implementation that generated the
topology description (e.g., ``"hwloc"``). The comparison of this field is
case-insensitive.

The ``topology`` field is an opaque pointer to the implementation-specific
topology object (e.g., an ``hwloc_topology_t``). Its interpretation depends on
the value of ``source``.

The ``PMIX_TOPOLOGY_STATIC_INIT`` macro is provided to statically initialize a
`pmix_topology_t` structure.

A `pmix_topology_t` may be populated by
:ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>` and is consumed by
operations such as :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`.


.. seealso::
   :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`,
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
