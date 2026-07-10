.. _man5-pmix_device_distance_t:

pmix_device_distance_t
======================

.. include_body

`pmix_device_distance_t` |mdash| Minimum and maximum relative distance from
a process to a device

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_device_distance {
       char *uuid;
       char *osname;
       pmix_device_type_t type;
       uint16_t mindist;
       uint16_t maxdist;
   } pmix_device_distance_t;


DESCRIPTION
-----------

The `pmix_device_distance_t` structure contains the minimum and maximum
relative distance from the caller to a given device.

The ``uuid`` field is a string identifier guaranteed to be unique within
the cluster, and is typically assembled from discovered device attributes
(e.g., the IP address of the device).

The ``osname`` field is the local operating system name of the device and
is only unique to the node on which the device resides.

The ``type`` field is a :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`
bitmask identifying the type(s) of the device.

The ``mindist`` and ``maxdist`` fields provide the minimum and maximum
relative distance to the device from the specified location of the process,
each expressed as a 16-bit integer value in which a smaller number indicates
that the device is closer to the process than a device with a larger
distance value. Note that relative distance values are not necessarily
correlated to a physical property |mdash| e.g., a device at twice the
distance from another device does not necessarily have twice the latency
for communication with it.

Relative distances only apply to similar devices and cannot be used to
compare devices of different types. Both minimum and maximum distances are
provided to support cases where the process may be bound to more than one
location, and those locations are at different distances from the device.

A relative distance value of ``UINT16_MAX`` indicates that the distance
from the process to the device could not be provided. This may be due to a
lack of available information (e.g., the PMIx library not having access to
device locations) or other factors.

An array of `pmix_device_distance_t` structures is returned by
:ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`.


.. seealso::
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`
