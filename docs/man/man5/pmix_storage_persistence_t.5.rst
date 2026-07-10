.. _man5-pmix_storage_persistence_t:

pmix_storage_persistence_t
==========================

.. include_body

`pmix_storage_persistence_t` |mdash| Bit-mask describing the persistence level of a storage system

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint64_t pmix_storage_persistence_t;
   #define PMIX_STORAGE_PERSISTENCE_TEMPORARY  0x0000000000000001
   #define PMIX_STORAGE_PERSISTENCE_NODE       0x0000000000000002
   #define PMIX_STORAGE_PERSISTENCE_SESSION    0x0000000000000004
   #define PMIX_STORAGE_PERSISTENCE_JOB        0x0000000000000008
   #define PMIX_STORAGE_PERSISTENCE_SCRATCH    0x0000000000000010
   #define PMIX_STORAGE_PERSISTENCE_PROJECT    0x0000000000000020
   #define PMIX_STORAGE_PERSISTENCE_ARCHIVE    0x0000000000000040


DESCRIPTION
-----------

The `pmix_storage_persistence_t` is a ``uint64_t`` type that defines a set of
bit-mask flags for specifying different levels of persistence for a
particular storage system.

The defined values are:

* ``PMIX_STORAGE_PERSISTENCE_TEMPORARY`` |mdash| Data on the storage system is
  persisted only temporarily (i.e., it does not survive across sessions or
  node reboots).
* ``PMIX_STORAGE_PERSISTENCE_NODE`` |mdash| Data on the storage system is
  persisted on the node.
* ``PMIX_STORAGE_PERSISTENCE_SESSION`` |mdash| Data on the storage system is
  persisted for the duration of the session.
* ``PMIX_STORAGE_PERSISTENCE_JOB`` |mdash| Data on the storage system is
  persisted for the duration of the job.
* ``PMIX_STORAGE_PERSISTENCE_SCRATCH`` |mdash| Data on the storage system is
  persisted according to scratch storage policies (short-term storage,
  typically persisted for days to weeks).
* ``PMIX_STORAGE_PERSISTENCE_PROJECT`` |mdash| Data on the storage system is
  persisted according to project storage policies (long-term storage,
  typically persisted for the duration of a project).
* ``PMIX_STORAGE_PERSISTENCE_ARCHIVE`` |mdash| Data on the storage system is
  persisted according to archive storage policies (long-term storage,
  typically persisted indefinitely).

A value of this type is conveyed using the ``PMIX_STORAGE_PERSISTENCE``
(``"pmix.strg.persist"``) attribute, and may be returned in response to
queries made via :ref:`PMIx_Get(3) <man3-PMIx_Get>` or
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_storage_medium_t(5) <man5-pmix_storage_medium_t>`,
   :ref:`pmix_storage_accessibility_t(5) <man5-pmix_storage_accessibility_t>`,
   :ref:`pmix_storage_access_type_t(5) <man5-pmix_storage_access_type_t>`
