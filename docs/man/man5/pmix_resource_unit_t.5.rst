.. _man5-pmix_resource_unit_t:

pmix_resource_unit_t
====================

.. include_body

`pmix_resource_unit_t` |mdash| Describes a quantity of a given resource type

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_resource_unit {
       pmix_device_type_t type;
       size_t count;
   } pmix_resource_unit_t;


DESCRIPTION
-----------

The `pmix_resource_unit_t` structure describes a quantity of a particular
type of resource. It is used, for example, to specify the minimum allocatable
unit for a system or to describe the resources contained in an allocation.

The ``type`` field is a :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`
value identifying the kind of resource being described (e.g., a class of
compute, memory, or fabric device). The ``count`` field is the number of units
of that resource type.

Arrays of `pmix_resource_unit_t` structures are conveyed via attributes such
as ``PMIX_ALLOC_MAU`` (the minimum allocatable unit for the system) passed to
:ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`.

The :ref:`PMIx_Resource_unit_string(3) <man3-PMIx_Resource_unit_string>`
function returns a string representation of a `pmix_resource_unit_t` value.


.. seealso::
   :ref:`PMIx_Resource_unit_string(3) <man3-PMIx_Resource_unit_string>`,
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
