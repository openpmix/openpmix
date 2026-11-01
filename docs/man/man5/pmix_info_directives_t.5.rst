.. _man5-pmix_info_directives_t:

pmix_info_directives_t
======================

.. include_body

`pmix_info_directives_t` |mdash| A set of bit-mask flags for specifying behavior of
command directives via :ref:`pmix_info_t <man5-pmix_info_t>` arrays

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint32_t pmix_info_directives_t;

   #define PMIX_INFO_REQD              0x00000001
   #define PMIX_INFO_ARRAY_END         0x00000002   // mark the end of an array created by PMIX_INFO_CREATE
   #define PMIX_INFO_REQD_PROCESSED    0x00000004   // reqd attribute has been processed
   #define PMIX_INFO_QUALIFIER         0x00000008   // info is a qualifier to the primary value
   #define PMIX_INFO_PERSISTENT        0x00000010   // do not release included value
   /* the top 16-bits are reserved for internal use */
   #define PMIX_INFO_DIR_RESERVED      0xffff0000


Python Syntax
^^^^^^^^^^^^^

None


DESCRIPTION
-----------

PMIx info directives are used to mark a given :ref:`pmix_info_t <man5-pmix_info_t>` structure with a bit-mask flag
denoting a particular characteristic of that structure. Defined values include:

.. list-table:: Info Directive Values
   :align: center
   :header-rows: 1

   * - **NAME**
     - **VALUE**
     - **DESCRIPTION**
   * - `PMIX_INFO_REQD`
     - 0x00000001
     - The included attribute is "required". This informs consumers of the info that support is not optional, and that an error must be returned if support is not available
   * - `PMIX_INFO_ARRAY_END`
     - 0x00000002
     - Marks the last element of an array created by the :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>` function. This serves to provide an additional layer of protection should the user fail to set the number of elements in the array prior to passing the array to another PMIx library function.
   * - `PMIX_INFO_REQD_PROCESSED`
     - 0x00000004
     - Required attribute has been processed. In some cases, a required attribute should only be processed once - this flag tells subsequent functions that the attribute has been processed.
   * - `PMIX_INFO_QUALIFIER`
     - 0x00000008
     - This info is a qualifier to the primary value
   * - `PMIX_INFO_PERSISTENT`
     - 0x00000010
     - The included :ref:`pmix_value_t <man5-pmix_value_t>` must not be free'd
   * - `PMIX_INFO_DIR_RESERVED`
     - 0xffff0000
     - The top 16-bits are reserved for internal use by implementers - these may be changed inside the PMIx library


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. seealso::
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`,
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`
