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


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   dev = {'uuid': "dev-uuid", 'osname': "mlx5_0", 'type': PMIX_DEVTYPE_NETWORK}

where ``uuid`` and ``osname`` are strings (``None`` for NULL) and ``type``
is a :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>` value. All
three must be present. A device handed back by the library has the same
shape.


DESCRIPTION
-----------

The `pmix_device_t` structure describes a single device located on a node.

The ``uuid`` field is a string identifier guaranteed to be unique within
the cluster. It is assembled from the attributes the topology reports for
the device, in one of the forms listed under `DEVICE UUIDS`_ below.

The ``osname`` field is the local operating system name of the device
(e.g., ``eth0``, ``mlx5_0`` or ``cuda0``) and is only unique to the node on
which the device resides. Where one PCI function presents several operating
system devices |mdash| an InfiniBand adapter presents both ``mlx5_0`` and
``ib0`` |mdash| the device is reported once, under the name the software
that uses it expects: the OpenFabrics device in preference to the network
interface. A GPU the topology reports only as a PCI function, with no
operating system device, is named by its PCI bus id (e.g.,
``0000:c1:00.0``).

The ``type`` field is a :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`
bitmask identifying the type(s) of the device.

DEVICE UUIDS
------------

The form of a ``uuid`` depends on the type of the device and on what the
topology knows about it. ``<node>`` is the name of the node the device is on
and ``<osname>`` is the device's ``osname``.

* A fabric (OpenFabrics) device with both GUIDs |mdash|
  ``fab://<NodeGUID>::<SysImageGUID>``, e.g.,
  ``fab://0e1a:2b3c:0000:0020::0e1a:2b3c:0000:0020``.
* A fabric device whose NodeGUID is zero |mdash|
  ``fab://<NodeGUID>::<SysImageGUID>::<node>::<osname>``, e.g.,
  ``fab://0000:0000:0000:0000::0000:0000:0000:0000::node01::rdmap17s0``.
* A fabric device missing either GUID |mdash| ``fab://<node>::<osname>``,
  e.g., ``fab://node01::mlx5_0``.
* A network interface with a 6-byte (Ethernet) hardware address |mdash|
  ``ipv4://<address>``, e.g., ``ipv4://00:1b:21:63:5f:cd``.
* A network interface with a 20-byte (IP-over-InfiniBand) hardware address
  |mdash| ``ipv6://<address>``, e.g.,
  ``ipv6://00:00:10:47:fe:80:00:00:00:00:00:00:b8:e9:24:03:00:ba:a4:bc``.
* A network interface with no recognized hardware address |mdash|
  ``net://<node>::<osname>``, e.g., ``net://node01::eth2``.
* A GPU or coprocessor |mdash| ``gpu://<node>::<osname>``, e.g.,
  ``gpu://node01::cuda0``, or ``gpu://node01::0000:c1:00.0`` for a GPU named
  by its bus id.
* A block device |mdash| ``blk://<node>::<osname>``, e.g.,
  ``blk://node01::nvme0n1``.

Note that the ``ipv4://`` and ``ipv6://`` forms carry the interface's
*hardware* address, not an IP address; the scheme distinguishes the two
address lengths and says nothing about the protocol running over the
interface.

A UUID is reported by the library and is meant to be read, not constructed:
a program that needs one obtains it from the ``uuid`` field of a
``pmix_device_t`` or :ref:`pmix_device_distance_t(5)
<man5-pmix_device_distance_t>` it was given, and may pass it back as a
``PMIX_DEVICE_ID`` (see :ref:`PMIx_Compute_distances(3)
<man3-PMIx_Compute_distances>`). Most forms embed the name of the node the
device is on, which is what makes them unique across the cluster.

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
