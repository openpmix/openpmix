.. _man5-pmix_device_t:

pmix_device_t
=============

.. include_body

`pmix_device_t` |mdash| Describes a single device on a node

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_device {
       char *uuid;
       char *osname;
       pmix_device_type_t type;
   } pmix_device_t;


DESCRIPTION
-----------

The `pmix_device_t` structure describes a single device located on a node.

The ``uuid`` field is a string identifier guaranteed to be unique within
the cluster, and is typically assembled from discovered device attributes
(e.g., the IP address of the device).

The ``osname`` field is the local operating system name of the device
(e.g., ``eth0`` or ``mlx5_0``) and is only unique to the node on which the
device resides.

The ``type`` field is a :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`
bitmask identifying the type(s) of the device.

STATIC INITIALIZER
------------------

A statically declared ``pmix_device_t`` may be initialized with the
``PMIX_DEVICE_STATIC_INIT`` macro, which sets ``uuid`` and ``osname`` to ``NULL`` and ``type`` to ``PMIX_DEVTYPE_UNKNOWN``:

.. code-block:: c

   pmix_device_t device = PMIX_DEVICE_STATIC_INIT;


.. seealso::
   :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`,
   :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>`,
   :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>`,
   :ref:`PMIx_Device_type_string(3) <man3-PMIx_Device_type_string>`
