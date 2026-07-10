.. _man5-pmix_storage_accessibility_t:

pmix_storage_accessibility_t
============================

.. include_body

`pmix_storage_accessibility_t` |mdash| Bit-mask describing from where a storage system may be accessed

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint64_t pmix_storage_accessibility_t;
   #define PMIX_STORAGE_ACCESSIBILITY_NODE     0x0000000000000001
   #define PMIX_STORAGE_ACCESSIBILITY_SESSION  0x0000000000000002
   #define PMIX_STORAGE_ACCESSIBILITY_JOB      0x0000000000000004
   #define PMIX_STORAGE_ACCESSIBILITY_RACK     0x0000000000000008
   #define PMIX_STORAGE_ACCESSIBILITY_CLUSTER  0x0000000000000010
   #define PMIX_STORAGE_ACCESSIBILITY_REMOTE   0x0000000000000020


DESCRIPTION
-----------

The `pmix_storage_accessibility_t` is a ``uint64_t`` type that defines a set
of bit-mask flags for specifying different levels of storage accessibility
(i.e., from where a storage system may be accessed). These can be bitwise
OR'd together to accommodate storage systems that are accessible in multiple
ways.

The defined values are:

* ``PMIX_STORAGE_ACCESSIBILITY_NODE`` |mdash| The storage system resources
  are accessible within the same node.
* ``PMIX_STORAGE_ACCESSIBILITY_SESSION`` |mdash| The storage system resources
  are accessible within the same session.
* ``PMIX_STORAGE_ACCESSIBILITY_JOB`` |mdash| The storage system resources are
  accessible within the same job.
* ``PMIX_STORAGE_ACCESSIBILITY_RACK`` |mdash| The storage system resources
  are accessible within the same rack.
* ``PMIX_STORAGE_ACCESSIBILITY_CLUSTER`` |mdash| The storage system resources
  are accessible within the same cluster.
* ``PMIX_STORAGE_ACCESSIBILITY_REMOTE`` |mdash| The storage system resources
  are remote.

A value of this type is conveyed using the ``PMIX_STORAGE_ACCESSIBILITY``
(``"pmix.strg.access"``) attribute, and may be returned in response to
queries made via :ref:`PMIx_Get(3) <man3-PMIx_Get>` or
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_storage_medium_t(5) <man5-pmix_storage_medium_t>`,
   :ref:`pmix_storage_persistence_t(5) <man5-pmix_storage_persistence_t>`,
   :ref:`pmix_storage_access_type_t(5) <man5-pmix_storage_access_type_t>`
