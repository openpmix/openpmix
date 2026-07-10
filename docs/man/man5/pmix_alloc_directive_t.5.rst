.. _man5-pmix_alloc_directive_t:

pmix_alloc_directive_t
======================

.. include_body

`pmix_alloc_directive_t` |mdash| Directive specifying the type of allocation request

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_alloc_directive_t;

   #define PMIX_ALLOC_NEW          1
   #define PMIX_ALLOC_EXTEND       2
   #define PMIX_ALLOC_RELEASE      3
   #define PMIX_ALLOC_REAQUIRE     4
   #define PMIX_ALLOC_REQ_CANCEL   5

   /* define a value boundary beyond which implementers are free
    * to define their own directive values */
   #define PMIX_ALLOC_EXTERNAL     128


DESCRIPTION
-----------

The `pmix_alloc_directive_t` type is used to indicate the nature of an
allocation request passed to
:ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`. It is an
8-bit unsigned integer taking one of the following values:

``PMIX_ALLOC_NEW`` (1)
   A new allocation is being requested. The resulting allocation will be
   disjoint (i.e., not connected in a job sense) from the requesting
   allocation.

``PMIX_ALLOC_EXTEND`` (2)
   Extend the existing allocation, either in time or as additional resources.

``PMIX_ALLOC_RELEASE`` (3)
   Release part or all of the existing allocation. Attributes in the
   accompanying :ref:`pmix_info_t(5) <man5-pmix_info_t>` array may be used to
   specify "lending" of those resources for some period of time.

``PMIX_ALLOC_REAQUIRE`` (4)
   Reacquire resources that were previously "lent" back to the scheduler.

``PMIX_ALLOC_REQ_CANCEL`` (5)
   Cancel the indicated allocation request.

Values at or above ``PMIX_ALLOC_EXTERNAL`` (128) define a boundary beyond
which implementers are free to define their own directive values.

The :ref:`PMIx_Alloc_directive_string(3) <man3-PMIx_Alloc_directive_string>`
function returns a string representation of a `pmix_alloc_directive_t` value.


.. seealso::
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Alloc_directive_string(3) <man3-PMIx_Alloc_directive_string>`,
   :ref:`pmix_alloc_inheritance_t(5) <man5-pmix_alloc_inheritance_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
