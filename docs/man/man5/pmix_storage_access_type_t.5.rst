.. _man5-pmix_storage_access_type_t:

pmix_storage_access_type_t
==========================

.. include_body

`pmix_storage_access_type_t` |mdash| Bit-mask describing storage system access types

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint16_t pmix_storage_access_type_t;
   #define PMIX_STORAGE_ACCESS_RD      0x0001
   #define PMIX_STORAGE_ACCESS_WR      0x0002
   #define PMIX_STORAGE_ACCESS_RDWR    0x0003


DESCRIPTION
-----------

The `pmix_storage_access_type_t` is a ``uint16_t`` type that defines a set of
bit-mask flags for specifying different storage system access types.

The defined values are:

* ``PMIX_STORAGE_ACCESS_RD`` |mdash| Provide information on storage system
  read operations.
* ``PMIX_STORAGE_ACCESS_WR`` |mdash| Provide information on storage system
  write operations.
* ``PMIX_STORAGE_ACCESS_RDWR`` |mdash| Provide information on storage system
  read and write operations.

A value of this type is conveyed using the ``PMIX_STORAGE_ACCESS_TYPE``
(``"pmix.strg.atype"``) attribute, typically as a qualifier describing the
type of storage access information to return in response to queries made via
:ref:`PMIx_Get(3) <man3-PMIx_Get>` or
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_storage_medium_t(5) <man5-pmix_storage_medium_t>`,
   :ref:`pmix_storage_accessibility_t(5) <man5-pmix_storage_accessibility_t>`,
   :ref:`pmix_storage_persistence_t(5) <man5-pmix_storage_persistence_t>`
