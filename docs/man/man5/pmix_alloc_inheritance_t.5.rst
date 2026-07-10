.. _man5-pmix_alloc_inheritance_t:

pmix_alloc_inheritance_t
========================

.. include_body

`pmix_alloc_inheritance_t` |mdash| Inheritance rules to be applied to an allocation

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_alloc_inheritance_t;

   #define PMIX_ALLOC_INHERIT_NONE          1
   #define PMIX_ALLOC_INHERIT_CHILD         2
   #define PMIX_ALLOC_INHERIT_DEFAULT       3
   #define PMIX_ALLOC_INHERIT_CHILD_DEFAULT 4


DESCRIPTION
-----------

The `pmix_alloc_inheritance_t` type specifies the inheritance rules to be
applied to an allocation |mdash| that is, what is to become of the allocated
resources upon termination of the owning namespace. It is an 8-bit unsigned
integer taking one of the following values:

``PMIX_ALLOC_INHERIT_NONE`` (1)
   No one is to inherit the allocation |mdash| it will be released upon
   termination of the owning namespace.

``PMIX_ALLOC_INHERIT_CHILD`` (2)
   The allocation is to remain alive until all child namespaces have
   terminated, including derived children.

``PMIX_ALLOC_INHERIT_DEFAULT`` (3)
   The allocation is to become unreserved upon termination of the owning
   namespace.

``PMIX_ALLOC_INHERIT_CHILD_DEFAULT`` (4)
   The allocation is to become unreserved upon termination of the last
   derived child namespace.

An inheritance value is passed to
:ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>` via the
``PMIX_ALLOC_INHERITANCE`` attribute. The
:ref:`PMIx_Alloc_inheritance_string(3) <man3-PMIx_Alloc_inheritance_string>`
function returns a string representation of a `pmix_alloc_inheritance_t` value.


.. seealso::
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Alloc_inheritance_string(3) <man3-PMIx_Alloc_inheritance_string>`,
   :ref:`pmix_alloc_directive_t(5) <man5-pmix_alloc_directive_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
