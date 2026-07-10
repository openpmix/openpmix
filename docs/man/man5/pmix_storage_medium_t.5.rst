.. _man5-pmix_storage_medium_t:

pmix_storage_medium_t
=====================

.. include_body

`pmix_storage_medium_t` |mdash| Bit-mask of storage medium types used by a storage system

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint64_t pmix_storage_medium_t;
   #define PMIX_STORAGE_MEDIUM_UNKNOWN     0x0000000000000001
   #define PMIX_STORAGE_MEDIUM_TAPE        0x0000000000000002
   #define PMIX_STORAGE_MEDIUM_HDD         0x0000000000000004
   #define PMIX_STORAGE_MEDIUM_SSD         0x0000000000000008
   #define PMIX_STORAGE_MEDIUM_NVME        0x0000000000000010
   #define PMIX_STORAGE_MEDIUM_PMEM        0x0000000000000020
   #define PMIX_STORAGE_MEDIUM_RAM         0x0000000000000040


DESCRIPTION
-----------

The `pmix_storage_medium_t` is a ``uint64_t`` type that defines a set of
bit-mask flags for specifying different types of storage mediums. These can
be bitwise OR'd together to accommodate storage systems that mix storage
medium types.

The defined values are:

* ``PMIX_STORAGE_MEDIUM_UNKNOWN`` |mdash| The storage medium type is unknown.
* ``PMIX_STORAGE_MEDIUM_TAPE`` |mdash| The storage system uses tape media.
* ``PMIX_STORAGE_MEDIUM_HDD`` |mdash| The storage system uses HDDs with
  traditional SAS, SATA interfaces.
* ``PMIX_STORAGE_MEDIUM_SSD`` |mdash| The storage system uses SSDs with
  traditional SAS, SATA interfaces.
* ``PMIX_STORAGE_MEDIUM_NVME`` |mdash| The storage system uses SSDs with NVMe
  interface.
* ``PMIX_STORAGE_MEDIUM_PMEM`` |mdash| The storage system uses persistent
  memory.
* ``PMIX_STORAGE_MEDIUM_RAM`` |mdash| The storage system is volatile (e.g.,
  tmpfs).

The constants are ordered in terms of increasing medium bandwidth; this
ordering is intended to provide semantic information that may be of use to
PMIx users.

A value of this type is conveyed using the ``PMIX_STORAGE_MEDIUM``
(``"pmix.strg.medium"``) attribute, and may be returned in response to
queries made via :ref:`PMIx_Get(3) <man3-PMIx_Get>` or
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_storage_accessibility_t(5) <man5-pmix_storage_accessibility_t>`,
   :ref:`pmix_storage_persistence_t(5) <man5-pmix_storage_persistence_t>`,
   :ref:`pmix_storage_access_type_t(5) <man5-pmix_storage_access_type_t>`
