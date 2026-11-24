.. _man5-pmix_coord_t:

pmix_coord_t
============

.. include_body

`pmix_coord_t` |mdash| Describes the fabric coordinates of a device in a given view

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_coord {
       pmix_coord_view_t view;
       uint32_t *coord;
       size_t dims;
   } pmix_coord_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   import struct
   coord = {'view': PMIX_COORD_LOGICAL_VIEW,
            'coord': struct.pack('=3I', 1, 4, 2),
            'dims': 3}

where ``view`` is a :ref:`pmix_coord_view_t(5) <man5-pmix_coord_view_t>`
value (default ``PMIX_COORD_VIEW_UNDEF``), ``coord`` is the coordinate
vector as raw bytes |mdash| one ``uint32_t`` per dimension in this host's
byte order |mdash| and ``dims`` is the number of coordinates, not of
bytes. ``dims`` defaults to the payload's length divided by four; a
``dims`` larger than the payload holds is ``PMIX_ERR_BAD_PARAM``. A missing
or ``None`` ``coord`` is a coordinate with no dimensions. A coordinate
handed back by the library has the same shape, with ``coord`` as
``bytes``.


DESCRIPTION
-----------

The `pmix_coord_t` structure describes the fabric coordinates of a specified
fabric device in a given *view*.

The ``view`` field is a :ref:`pmix_coord_view_t(5) <man5-pmix_coord_view_t>`
value identifying the view (logical or physical) in which the coordinate is
expressed. A fabric coordinate must be unique within a given view.

The ``coord`` field points to an array of ``dims`` unsigned 32-bit integers,
one per dimension of the coordinate. The ``dims`` field gives the number of
dimensions in the coordinate, and therefore the number of elements in the
``coord`` array.

All coordinate values are expressed as unsigned integers, as their units are
defined in terms of fabric devices rather than physical distances. A coordinate
is therefore an indicator of connectivity, and not of relative communication
distance. The structure does not mandate any particular internal storage format
for the coordinate data; implementers are free to store it in whatever form they
choose.

Fabric devices are associated with the operating system that hosts them, so
fabric coordinates are logically grouped within the *node* realm.

The ``PMIX_COORD_STATIC_INIT`` macro is provided to statically initialize a
`pmix_coord_t` structure.

STATIC INITIALIZER
------------------

A statically declared ``pmix_coord_t`` may be initialized with the
``PMIX_COORD_STATIC_INIT`` macro, which sets ``view`` to ``PMIX_COORD_VIEW_UNDEF``, ``coord`` to ``NULL``, and ``dims`` to ``0``:

.. code-block:: c

   pmix_coord_t coord = PMIX_COORD_STATIC_INIT;


.. seealso::
   :ref:`pmix_coord_view_t(5) <man5-pmix_coord_view_t>`,
   :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
