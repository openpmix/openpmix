.. _man5-pmix_endpoint_t:

pmix_endpoint_t
===============

.. include_body

`pmix_endpoint_t` |mdash| An assigned communication endpoint for a fabric
device

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_endpoint {
       char *uuid;
       char *osname;
       pmix_byte_object_t endpt;
   } pmix_endpoint_t;


DESCRIPTION
-----------

The `pmix_endpoint_t` structure contains an assigned endpoint for a given
fabric device.

The ``uuid`` field contains the UUID of the fabric device |mdash| a string
identifier guaranteed to be unique within the cluster.

The ``osname`` field is the local operating system name of the device and
is only unique to the node on which the device resides.

The ``endpt`` field is a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
containing a fabric vendor-specific object identifying the communication
endpoint assigned to the process.

As multiple endpoints may be assigned to a given process (e.g., when
multiple devices are associated with a package to which the process is
bound), fabric endpoints for a process are typically returned as a
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` of `pmix_endpoint_t`
elements.


.. seealso::
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`,
   :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`
