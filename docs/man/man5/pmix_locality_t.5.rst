.. _man5-pmix_locality_t:

pmix_locality_t
===============

.. include_body

`pmix_locality_t` |mdash| Bitmask describing the relative locality of two processes

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint16_t pmix_locality_t;
   #define PMIX_LOCALITY_UNKNOWN           0x0000
   #define PMIX_LOCALITY_NONLOCAL          0x8000
   #define PMIX_LOCALITY_SHARE_HWTHREAD    0x0001
   #define PMIX_LOCALITY_SHARE_CORE        0x0002
   #define PMIX_LOCALITY_SHARE_L1CACHE     0x0004
   #define PMIX_LOCALITY_SHARE_L2CACHE     0x0008
   #define PMIX_LOCALITY_SHARE_L3CACHE     0x0010
   #define PMIX_LOCALITY_SHARE_PACKAGE     0x0020
   #define PMIX_LOCALITY_SHARE_NUMA        0x0040
   #define PMIX_LOCALITY_SHARE_NODE        0x4000


DESCRIPTION
-----------

The `pmix_locality_t` datatype is a ``uint16_t`` bitmask that defines the
relative locality of two processes on a node. The following constants represent
specific bits in the mask and can be tested using standard bit-test methods.
Multiple ``PMIX_LOCALITY_SHARE_*`` bits may be set simultaneously to describe the
full set of hardware locations shared by the two processes.

``PMIX_LOCALITY_UNKNOWN`` (``0x0000``)
   All bits are set to zero, indicating that the relative locality of the two
   processes is unknown.

``PMIX_LOCALITY_NONLOCAL`` (``0x8000``)
   The two processes do not share any common locations.

``PMIX_LOCALITY_SHARE_HWTHREAD`` (``0x0001``)
   The two processes share at least one hardware thread.

``PMIX_LOCALITY_SHARE_CORE`` (``0x0002``)
   The two processes share at least one core.

``PMIX_LOCALITY_SHARE_L1CACHE`` (``0x0004``)
   The two processes share at least an L1 cache.

``PMIX_LOCALITY_SHARE_L2CACHE`` (``0x0008``)
   The two processes share at least an L2 cache.

``PMIX_LOCALITY_SHARE_L3CACHE`` (``0x0010``)
   The two processes share at least an L3 cache.

``PMIX_LOCALITY_SHARE_PACKAGE`` (``0x0020``)
   The two processes share at least a package.

``PMIX_LOCALITY_SHARE_NUMA`` (``0x0040``)
   The two processes share at least one NUMA region.

``PMIX_LOCALITY_SHARE_NODE`` (``0x4000``)
   The two processes are executing on the same node.

Implementers and vendors may choose to extend these definitions as needed to
describe a particular system.

A `pmix_locality_t` value is computed by
:ref:`PMIx_Get_relative_locality(3) <man3-PMIx_Get_relative_locality>` from the
locality strings of two processes.


.. seealso::
   :ref:`PMIx_Get_relative_locality(3) <man3-PMIx_Get_relative_locality>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
