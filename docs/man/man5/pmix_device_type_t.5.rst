.. _man5-pmix_device_type_t:

pmix_device_type_t
==================

.. include_body

`pmix_device_type_t` |mdash| Bitmask identifying the type(s) of a device

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint64_t pmix_device_type_t;

   #define PMIX_DEVTYPE_UNKNOWN        0x0000000000000000
   #define PMIX_DEVTYPE_BLOCK          0x0000000000000001
   #define PMIX_DEVTYPE_GPU            0x0000000000000002
   #define PMIX_DEVTYPE_NETWORK        0x0000000000000004
   #define PMIX_DEVTYPE_OPENFABRICS    0x0000000000000008
   #define PMIX_DEVTYPE_DMA            0x0000000000000010
   #define PMIX_DEVTYPE_COPROC         0x0000000000000020
   /* devices used by resource units */
   #define PMIX_DEVTYPE_MEMORY         0x0000000000000040
   #define PMIX_DEVTYPE_CORE           0x0000000000000080
   #define PMIX_DEVTYPE_HWT            0x0000000000000100
   #define PMIX_DEVTYPE_CPU            0x0000000000000200
   #define PMIX_DEVTYPE_NODE           0x0000000000000400


DESCRIPTION
-----------

The `pmix_device_type_t` is a ``uint64_t`` bitmask used to identify the
type(s) of device being referenced |mdash| for example, the type(s) whose
relative distances are being requested, or the type recorded in a
:ref:`pmix_device_t(5) <man5-pmix_device_t>` or
:ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>` object.

Because the value is a bitmask, multiple device-type bits may be combined
with the bitwise-OR operator to reference several types at once.

The following constants may be used to set or test a variable of type
`pmix_device_type_t`:

* ``PMIX_DEVTYPE_UNKNOWN`` (``0x00``) |mdash| The device is of an unknown
  type. Such devices will not be included in returned device distances.
* ``PMIX_DEVTYPE_BLOCK`` (``0x01``) |mdash| Operating system block device,
  or non-volatile memory device (e.g., ``sda`` or ``dax2.0`` on Linux).
* ``PMIX_DEVTYPE_GPU`` (``0x02``) |mdash| Operating system GPU device
  (e.g., ``card0`` for a Linux DRM device).
* ``PMIX_DEVTYPE_NETWORK`` (``0x04``) |mdash| Operating system network
  device (e.g., the ``eth0`` interface on Linux).
* ``PMIX_DEVTYPE_OPENFABRICS`` (``0x08``) |mdash| Operating system
  OpenFabrics device (e.g., an ``mlx4_0`` InfiniBand HCA, or ``hfi1_0``
  Omni-Path interface on Linux).
* ``PMIX_DEVTYPE_DMA`` (``0x10``) |mdash| Operating system DMA engine
  device (e.g., the ``dma0chan0`` DMA channel on Linux).
* ``PMIX_DEVTYPE_COPROC`` (``0x20``) |mdash| Operating system co-processor
  device (e.g., ``mic0`` for a Xeon Phi, ``opencl0d0`` for an OpenCL
  device, or ``cuda0`` for a CUDA device on Linux).
* ``PMIX_DEVTYPE_MEMORY`` (``0x40``) |mdash| A memory device, as used by
  resource units.
* ``PMIX_DEVTYPE_CORE`` (``0x80``) |mdash| A processor core, as used by
  resource units.
* ``PMIX_DEVTYPE_HWT`` (``0x100``) |mdash| A hardware thread, as used by
  resource units.
* ``PMIX_DEVTYPE_CPU`` (``0x200``) |mdash| A CPU, as used by resource
  units.
* ``PMIX_DEVTYPE_NODE`` (``0x400``) |mdash| A node, as used by resource
  units.

The ``PMIX_DEVTYPE_MEMORY``, ``PMIX_DEVTYPE_CORE``, ``PMIX_DEVTYPE_HWT``,
``PMIX_DEVTYPE_CPU``, and ``PMIX_DEVTYPE_NODE`` values are used by the
:ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>` structure and
are extensions beyond the fabric-oriented device types defined in the
PMIx Standard.

A string representation of a `pmix_device_type_t` value can be obtained
using :ref:`PMIx_Device_type_string(3) <man3-PMIx_Device_type_string>`.


.. seealso::
   :ref:`PMIx_Device_type_string(3) <man3-PMIx_Device_type_string>`,
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`,
   :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
