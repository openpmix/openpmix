.. _man5-pmix_geometry_t:

pmix_geometry_t
===============

.. include_body

`pmix_geometry_t` |mdash| Describes the fabric coordinates of a specified device

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_geometry {
       size_t fabric;
       char *uuid;
       char *osname;
       pmix_coord_t *coordinates;
       size_t ncoords;
   } pmix_geometry_t;


DESCRIPTION
-----------

The `pmix_geometry_t` structure describes the fabric coordinates of a specified
fabric device, associating a device with its coordinates in one or more views.

The ``fabric`` field is the index of the fabric (fabric plane) with which the
device is associated.

The ``uuid`` field is a string containing the universally unique identifier of
the fabric device.

The ``osname`` field is a string containing the operating-system name of the
fabric device (e.g., the local device name).

The ``coordinates`` field points to an array of ``ncoords``
:ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structures giving the device's
coordinates. A device may have more than one coordinate, since a coordinate is
expressed in a particular view (see
:ref:`pmix_coord_view_t(5) <man5-pmix_coord_view_t>`). The ``ncoords`` field
gives the number of elements in the ``coordinates`` array.

All coordinate values are expressed as unsigned integers, as their units are
defined in terms of fabric devices rather than physical distances; a coordinate
is therefore an indicator of connectivity, and not of relative communication
distance. Fabric coordinates must be unique within a given view and are logically
grouped within the *node* realm.

The ``PMIX_GEOMETRY_STATIC_INIT`` macro is provided to statically initialize a
`pmix_geometry_t` structure.


.. seealso::
   :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`,
   :ref:`pmix_coord_view_t(5) <man5-pmix_coord_view_t>`,
   :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`,
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`
