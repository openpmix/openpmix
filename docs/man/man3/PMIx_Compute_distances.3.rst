.. _man3-PMIx_Compute_distances:

PMIx_Compute_distances
======================

.. include_body

``PMIx_Compute_distances``, ``PMIx_Compute_distances_nb`` |mdash| Compute the
distances from a specified process location, or from a specified device, to
the local devices.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Compute_distances(pmix_topology_t *topo,
                                        pmix_cpuset_t *cpuset,
                                        pmix_info_t info[], size_t ninfo,
                                        pmix_device_distance_t *distances[],
                                        size_t *ndist);

   pmix_status_t PMIx_Compute_distances_nb(pmix_topology_t *topo,
                                           pmix_cpuset_t *cpuset,
                                           pmix_info_t info[], size_t ninfo,
                                           pmix_device_dist_cbfunc_t cbfunc,
                                           void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # load the local topology first
  rc = foo.load_topology()
  # the cpuset is a Python dictionary describing the process location
  pycpus = {'source': "hwloc", 'cpus': [0, 1]}
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_DEVICE_TYPE,
             'value': PMIX_DEVTYPE_NETWORK, 'val_type': PMIX_UINT64}]
  rc, distances = foo.compute_distances(pycpus, pydirs)

  # the non-blocking form returns as soon as the request has been accepted
  # and reports the result by executing a callback on the PMIx progress
  # thread. The callback is run if and only if the call returned
  # PMIX_SUCCESS, and must not itself make a blocking PMIx call.
  def distcb(status, distances, cbdata):
      # distances is a list of dictionaries, each carrying uuid, osname,
      # type, mindist and maxdist
      print("distances:", foo.error_string(status), distances)
  rc = foo.compute_distances_nb(pycpus, pydirs, distcb, "mycbdata")


INPUT PARAMETERS
----------------

* ``topo``: Pointer to a ``pmix_topology_t`` describing the topology of the node
  where the process is located. A ``NULL`` value indicates that the topology of
  the local node is to be used. If the caller's own topology has not yet been
  loaded, the library will attempt to load it (see
  :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`) or, failing that,
  relay the request to the local PMIx server. A topology given here is used
  only by the calling process's own library: a request relayed to the server
  is answered from the server's own topology of the node.
* ``cpuset``: Pointer to a ``pmix_cpuset_t`` identifying the location (the set of
  processing units to which the process is bound) from which distances are to be
  computed. A ``NULL`` value indicates that the caller's own location is to be
  used; if it has not yet been determined, the library will attempt to obtain it
  or relay the request to the local PMIx server. Ignored, and may be ``NULL``,
  when the ``PMIX_DEVICE_DIST_ORIGIN`` directive is given.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures describing the device type(s) or specific device(s) whose distance
  is to be computed (see `DIRECTIVES`_). A ``NULL`` value (with ``ninfo`` of
  zero) requests distances to all device types supported by the underlying
  topology description.
* ``ninfo``: Number of elements in the ``info`` array.

For :ref:`PMIx_Compute_distances_nb(3) <man3-PMIx_Compute_distances>`:

* ``cbfunc``: Callback function of type :ref:`pmix_device_dist_cbfunc_t <man5-pmix_device_dist_cbfunc_t>` to be
  invoked when the operation is complete.
* ``cbdata``: Opaque data pointer to be passed to ``cbfunc`` when invoked.


OUTPUT PARAMETERS
-----------------

For the blocking form :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`:

* ``distances``: On successful return, ``*distances`` points to a newly allocated
  array of ``pmix_device_distance_t`` structures containing the minimum and
  maximum distances from the specified process location to each of the requested
  devices. Each element reports the ``uuid``, ``osname``, ``type``, ``mindist``,
  and ``maxdist`` of a device. The array is allocated by the library and
  ownership passes to the caller, who is responsible for releasing it (for
  example, with ``PMIx_Device_distance_free()``).
* ``ndist``: On successful return, ``*ndist`` holds the number of elements in the
  returned ``distances`` array.

On entry, the blocking form initializes ``*distances`` to ``NULL`` and ``*ndist``
to zero.

For the non-blocking form, the array of ``pmix_device_distance_t`` and its count
are delivered as the ``dist`` and ``ndist`` arguments of the callback function.
The data provided to the callback is owned by the library; the callback is passed
a ``release_fn`` (and ``release_cbdata``) that must be invoked when the caller is
finished with the ``dist`` array.


DESCRIPTION
-----------

Compute the distances between a process at a given location and the devices
available on a node. Both the minimum and maximum distance fields in each element
of the returned array are filled with the respective distances between the
process location and the device types (or specific devices) identified by the
``info`` directives. In the absence of directives, distances to the network,
OpenFabrics, GPU and coprocessor devices on the node are returned.

Alternatively, distances can be computed from a device rather than from a
process location by naming that device with the ``PMIX_DEVICE_DIST_ORIGIN``
directive |mdash| for example, to find the network devices nearest the GPU a
process is using. The ``cpuset`` plays no part in that case, so a process need
not be bound to ask. The origin device itself is included in the result, at a
distance of zero, if it is of a requested type.

Distances are relative, and smaller means closer. A distance from a process
location is governed first by where the device attaches to the processor
hierarchy (the package or NUMA domain it is local to); among devices that
attach at the same place, a device fewer PCIe levels (bridges and switches)
below that point is closer. A distance from a device follows the path between
the two devices: devices under the same PCIe switch are closer than devices
under the same host bridge, which are closer than devices on different host
bridges of the same package, which are closer than devices on different
packages. Because every device under a package is equally close to that
package's processors, only a distance measured from a device can distinguish
the host bridges within a package.

:ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>` is the blocking
form: it does not return until the computation completes, at which point the
result array is returned through the ``distances`` and ``ndist`` parameters.

:ref:`PMIx_Compute_distances_nb(3) <man3-PMIx_Compute_distances>` is the
non-blocking form: it returns immediately (``PMIX_SUCCESS`` indicating that the
request was successfully initiated) and delivers the result later by invoking
``cbfunc``. As with all non-blocking PMIx operations, the caller must ensure that
the ``topo``, ``cpuset``, and ``info`` arguments remain valid until ``cbfunc``
has been invoked.

If the local PMIx library can satisfy the request itself (i.e., it has access to
a suitable topology and cpuset), it does so directly. Otherwise, a connected
client or tool relays the request to its local PMIx server, which computes the
distances from its own topology of the node whatever ``topo`` was given. A
process acting solely as a server, or a process that is not connected to a
server and cannot compute the distances locally, cannot service the request.


DIRECTIVES
----------

The following attributes are optional for PMIx implementations and may be passed
in the ``info`` array to select the devices whose distances are to be computed:

* ``PMIX_DEVICE_DISTANCES`` (``"pmix.dev.dist"``) |mdash| request the return of an
  array of ``pmix_device_distance_t`` describing the distances to devices on the
  local node.
* ``PMIX_DEVICE_TYPE`` (``"pmix.dev.type"``) |mdash| a ``pmix_device_type_t``
  bitmask specifying the type(s) of device whose distances are to be computed.
  Values include ``PMIX_DEVTYPE_BLOCK``, ``PMIX_DEVTYPE_GPU``,
  ``PMIX_DEVTYPE_NETWORK``, ``PMIX_DEVTYPE_OPENFABRICS``, ``PMIX_DEVTYPE_DMA``,
  ``PMIX_DEVTYPE_COPROC``, and others defined in ``pmix_common.h``.
* ``PMIX_DEVICE_ID`` (``"pmix.dev.id"``) |mdash| the device(s) whose
  distances are to be computed: a ``char*`` naming one device, a
  :ref:`pmix_device_t(5) <man5-pmix_device_t>`, or a ``pmix_data_array_t`` of
  ``pmix_device_t`` naming each of its elements. The array is the form
  :ref:`PMIx_Get(3) <man3-PMIx_Get>` returns for a process's own
  ``PMIX_DEVICE_ID``, so a process can pass its assignment straight back. The
  attribute may be given more than once to request distances to several
  devices. See `NAMING A DEVICE`_ below for what a name may contain. Any other
  value type, a device carrying neither an ``osname`` nor a ``uuid``, and an
  empty array return ``PMIX_ERR_BAD_PARAM``.
* ``PMIX_DEVICE_DIST_ORIGIN`` (``"pmix.dev.dist.origin"``) |mdash| the device
  from which distances are to be computed, in place of a process location, in
  any of the forms accepted by ``PMIX_DEVICE_ID``; an array must hold exactly
  one device. The ``cpuset`` argument is ignored when this directive is given.
  Returns ``PMIX_ERR_NOT_FOUND`` if no device answers to the name, and
  ``PMIX_ERR_BAD_PARAM`` if more than one does.


NAMING A DEVICE
---------------

``PMIX_DEVICE_ID`` and ``PMIX_DEVICE_DIST_ORIGIN`` name a device on the node
whose topology is being searched. A device answers to any of the following
spellings, compared without regard to case:

* **The device's PMIx UUID**, exactly as reported in the ``uuid`` field of a
  :ref:`pmix_device_t(5) <man5-pmix_device_t>` or
  :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>` |mdash| e.g.,
  ``gpu://node01::cuda0`` or
  ``fab://0e1a:2b3c:0000:0020::0e1a:2b3c:0000:0020``. Every form is listed
  in :ref:`pmix_device_t(5) <man5-pmix_device_t>`.
* **The name of any of its operating system devices** |mdash| e.g.,
  ``mlx5_0``, ``ib0``, ``eth0``, ``cuda0`` or ``nvme0n1``. These are the
  names in the ``osname`` field and the device names ``lstopo`` shows; for
  network adapters and disks they are the operating system's own, as
  ``ip link``, ``ibv_devices`` and ``lsblk`` print them.
* **Its vendor identity** |mdash| e.g.,
  ``GPU-5b1e3c2a-8f4d-4a6b-9c0e-2d7f1a3b4c5d``, the UUID ``nvidia-smi -L``
  prints for an NVIDIA GPU. This is the identity hwloc records for a GPU
  (its ``NVIDIAUUID``, ``AMDUUID`` or ``LevelZeroUUID`` attribute), so it is
  available only where the topology carries it: from hwloc's vendor
  backend, or for an NVIDIA GPU from its driver.
* **The PCI bus id of its function** |mdash| e.g., ``0000:06:00.0``, as
  ``lspci -D`` prints it, or ``00000000:06:00.0``, as ``nvidia-smi`` does.
  The fields are compared by value, so the domain may be given at any width
  up to eight hex digits, but it must be present.

A ``pmix_device_t`` is resolved by its ``osname`` when it has one, and by its
``uuid`` otherwise; its ``type`` is not consulted. The ``osname`` comes first
because it is how this node's topology names the device, while a UUID embeds
the node name its producer used, which need not match the name this node
gives itself (e.g., a fully qualified name against a short one).

Every operating system device on the device's PCI function counts, not only
the one reported in ``osname``: an InfiniBand adapter reported as ``mlx5_0``
also answers to ``ib0``, and naming both returns the adapter once. When
``PMIX_DEVICE_TYPE`` is given too, a named device is reported only if it is
of one of those types. A ``PMIX_DEVICE_ID`` that names no device on the node
contributes nothing to the result; ``PMIX_ERR_NOT_FOUND`` is returned only
if no requested device was found at all. ``PMIX_DEVICE_DIST_ORIGIN``, by
contrast, must name exactly one device.

The simplest way to obtain a valid name is to ask for distances without
naming a device and read the names back from the result. For example, to
find the network devices nearest the first GPU the process can see:

.. code-block:: c

   pmix_device_distance_t *dist = NULL, *nics = NULL;
   size_t ndist = 0, nnics = 0, n;
   pmix_device_type_t gpu = PMIX_DEVTYPE_GPU, net = PMIX_DEVTYPE_NETWORK |
                                                   PMIX_DEVTYPE_OPENFABRICS;
   pmix_info_t info[2];
   pmix_status_t rc;

   /* topo was filled in by PMIx_Load_topology() and cpuset by
    * PMIx_Get_cpuset() */

   /* what GPUs are there, and what are they called? */
   PMIX_INFO_LOAD(&info[0], PMIX_DEVICE_TYPE, &gpu, PMIX_DEVTYPE);
   rc = PMIx_Compute_distances(&topo, &cpuset, info, 1, &dist, &ndist);
   PMIX_INFO_DESTRUCT(&info[0]);
   if (PMIX_SUCCESS != rc || 0 == ndist) {
       return rc;
   }
   /* dist[0].uuid is, e.g., "gpu://node01::cuda0" and dist[0].osname
    * "cuda0"; either names that GPU */

   /* measure from that GPU to the NICs */
   PMIX_INFO_LOAD(&info[0], PMIX_DEVICE_DIST_ORIGIN, dist[0].uuid, PMIX_STRING);
   PMIX_INFO_LOAD(&info[1], PMIX_DEVICE_TYPE, &net, PMIX_DEVTYPE);
   rc = PMIx_Compute_distances(&topo, NULL, info, 2, &nics, &nnics);
   PMIX_INFO_DESTRUCT(&info[0]);
   PMIX_INFO_DESTRUCT(&info[1]);
   for (n = 0; PMIX_SUCCESS == rc && n < nnics; n++) {
       printf("%s (%s): %u\n", nics[n].osname, nics[n].uuid, nics[n].mindist);
   }
   PMIx_Device_distance_free(dist, ndist);
   PMIx_Device_distance_free(nics, nnics);

A device can equally be named by what the caller already knows about it.
To ask how far the process is from two specific adapters:

.. code-block:: c

   pmix_info_t info[2];

   PMIX_INFO_LOAD(&info[0], PMIX_DEVICE_ID, "mlx5_0", PMIX_STRING);
   PMIX_INFO_LOAD(&info[1], PMIX_DEVICE_ID, "0000:c1:00.0", PMIX_STRING);
   rc = PMIx_Compute_distances(&topo, &cpuset, info, 2, &dist, &ndist);

A process that was assigned devices by its host |mdash| see ``PMIX_DEVICE_ID``
in :ref:`PMIx_Get(3) <man3-PMIx_Get>` |mdash| can pass the value it retrieved
straight back, to ask how far it is from each of them:

.. code-block:: c

   pmix_value_t *val = NULL;
   pmix_info_t info;

   if (PMIX_SUCCESS == PMIx_Get(&myproc, PMIX_DEVICE_ID, NULL, 0, &val)) {
       if (PMIX_DATA_ARRAY == val->type) {
           PMIX_INFO_LOAD(&info, PMIX_DEVICE_ID, val->data.darray, PMIX_DATA_ARRAY);
           rc = PMIx_Compute_distances(&topo, &cpuset, &info, 1, &dist, &ndist);
           PMIX_INFO_DESTRUCT(&info);
       }
       PMIX_VALUE_RELEASE(val);
   }


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_NOT_FOUND`` |mdash| no device of the requested type(s), or none of
  the devices named by ``PMIX_DEVICE_ID``, exists on the node; or the device
  named by ``PMIX_DEVICE_DIST_ORIGIN`` does not.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a ``PMIX_DEVICE_ID`` or
  ``PMIX_DEVICE_DIST_ORIGIN`` value is not one of the accepted forms, names a
  device by neither ``osname`` nor ``uuid``, or is an empty array; or the
  ``PMIX_DEVICE_DIST_ORIGIN`` names, or matches, more than one device.
* ``PMIX_ERR_UNREACH`` |mdash| the request could not be satisfied locally and the
  local PMIx server could not be reached (or the caller is a server, or is not
  connected to a server).
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| ``PMIX_DEVICE_DIST_ORIGIN`` was given, the
  request could not be satisfied locally, and the local PMIx server predates
  support for that directive.

For the blocking form, the value returned is the status of the completed
operation. For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates only
that the request was successfully initiated; the final status of the operation is
reported as the ``status`` argument to ``cbfunc``. Any other negative value
indicates an appropriate error condition. PMIx error constants are defined in
``pmix_common.h``.


NOTES
-----

The ``pmix_device_t`` forms of ``PMIX_DEVICE_ID`` and
``PMIX_DEVICE_DIST_ORIGIN`` are accepted where ``PMIX_CAP_DEVICE_ID_STRUCT``
is defined in ``pmix_version.h``; an earlier library refuses them with
``PMIX_ERR_BAD_PARAM``, or finds no device. When the request is
relayed to the local server, each device is sent as the name it is resolved
by, so a server that predates these forms answers it correctly.

The ``PMIX_DEVICE_DIST_CREATE`` and ``PMIX_DEVICE_DIST_FREE`` convenience macros
are deprecated. New code should manage ``pmix_device_distance_t`` arrays using
the ``PMIx_Device_distance_create()`` and ``PMIx_Device_distance_free()``
functions.

A process whose threads are not all bound to the same location may return
inconsistent results from calls to this API by different threads if the
``PMIX_CPUBIND_THREAD`` binding envelope was used when generating the ``cpuset``.


.. include:: /man/no-blocking-in-progress-thread.rst


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`,
   :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>`,
   :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
