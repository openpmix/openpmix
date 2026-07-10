.. _man5-pmix_coord_view_t:

pmix_coord_view_t
=================

.. include_body

`pmix_coord_view_t` |mdash| Identifies the view in which fabric coordinates are expressed

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_coord_view_t;
   #define PMIX_COORD_VIEW_UNDEF       0x00
   #define PMIX_COORD_LOGICAL_VIEW     0x01
   #define PMIX_COORD_PHYSICAL_VIEW    0x02


DESCRIPTION
-----------

The `pmix_coord_view_t` datatype is a ``uint8_t`` that identifies the *view* in
which a fabric coordinate is expressed. Fabric coordinates can be reported using
different views according to user preference at the time of request. The
following views are defined:

``PMIX_COORD_VIEW_UNDEF`` (``0x00``)
   The coordinate view has not been defined.

``PMIX_COORD_LOGICAL_VIEW`` (``0x01``)
   The coordinates are provided in a *logical* view, typically given in
   Cartesian (x,y,z) dimensions, that describes the data flow in the fabric as
   defined by the arrangement of the hierarchical addressing scheme, fabric
   segmentation, routing domains, and other similar factors employed by that
   fabric.

``PMIX_COORD_PHYSICAL_VIEW`` (``0x02``)
   The coordinates are provided in a *physical* view based on the actual wiring
   diagram of the fabric |mdash| i.e., values along each axis reflect the
   relative position of that interface on the specific fabric cabling.


.. seealso::
   :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`,
   :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
