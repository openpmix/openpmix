.. _man5-pmix_resource_block_directive_t:

pmix_resource_block_directive_t
===============================

.. include_body

`pmix_resource_block_directive_t` |mdash| Directive specifying a resource block request

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_resource_block_directive_t;

   #define PMIX_RESOURCE_BLOCK_DEFINE  1
   #define PMIX_RESOURCE_BLOCK_EXTEND  2
   #define PMIX_RESOURCE_BLOCK_REMOVE  3
   #define PMIX_RESOURCE_BLOCK_DELETE  4

   /* define a value boundary beyond which implementers are free
    * to define their own directive values */
   #define PMIX_RESOURCE_BLOCK_EXTERNAL     128


DESCRIPTION
-----------

The `pmix_resource_block_directive_t` type indicates the nature of a resource
block request passed to
:ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`. It is an 8-bit
unsigned integer taking one of the following values:

``PMIX_RESOURCE_BLOCK_DEFINE`` (1)
   Define a new resource block.

``PMIX_RESOURCE_BLOCK_EXTEND`` (2)
   Extend an existing block definition by adding the specified resources to
   the block.

``PMIX_RESOURCE_BLOCK_REMOVE`` (3)
   Remove the specified resources from the block definition.

``PMIX_RESOURCE_BLOCK_DELETE`` (4)
   Delete the resource block definition.

Values at or above ``PMIX_RESOURCE_BLOCK_EXTERNAL`` (128) define a boundary
beyond which implementers are free to define their own directive values.

The
:ref:`PMIx_Resource_block_directive_string(3) <man3-PMIx_Resource_block_directive_string>`
function returns a string representation of a
`pmix_resource_block_directive_t` value.


.. seealso::
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`PMIx_Resource_block_directive_string(3) <man3-PMIx_Resource_block_directive_string>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
