.. _man3-PMIx_Compute_distances:

PMIx_Compute_distances
======================

.. include_body

``PMIx_Compute_distances``, ``PMIx_Compute_distances_nb`` |mdash| Compute the
distances from a specified process location to the local devices.


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
  pycpus = {'source': "hwloc", 'cpus': ["0", "1"]}
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_DEVICE_TYPE,
             'value': {'value': PMIX_DEVTYPE_NETWORK, 'val_type': PMIX_UINT64}}]
  rc, distances = foo.compute_distances(pycpus, pydirs)


INPUT PARAMETERS
----------------

* ``topo``: Pointer to a ``pmix_topology_t`` describing the topology of the node
  where the process is located. A ``NULL`` value indicates that the topology of
  the local node is to be used. If the caller's own topology has not yet been
  loaded, the library will attempt to load it (see
  :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`) or, failing that,
  relay the request to the local PMIx server.
* ``cpuset``: Pointer to a ``pmix_cpuset_t`` identifying the location (the set of
  processing units to which the process is bound) from which distances are to be
  computed. A ``NULL`` value indicates that the caller's own location is to be
  used; if it has not yet been determined, the library will attempt to obtain it
  or relay the request to the local PMIx server.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures describing the device type(s) or specific device(s) whose distance
  is to be computed (see `DIRECTIVES`_). A ``NULL`` value (with ``ninfo`` of
  zero) requests distances to all device types supported by the underlying
  topology description.
* ``ninfo``: Number of elements in the ``info`` array.

For :ref:`PMIx_Compute_distances_nb(3) <man3-PMIx_Compute_distances>`:

* ``cbfunc``: Callback function of type ``pmix_device_dist_cbfunc_t`` to be
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
process location and the device types (or specific device) identified by the
``info`` directives. In the absence of directives, distances to all device types
supported by the underlying topology description are returned.

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
client or tool relays the request to its local PMIx server. A process acting
solely as a server, or a process that is not connected to a server and cannot
compute the distances locally, cannot service the request.


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
* ``PMIX_DEVICE_ID`` (``"pmix.dev.id"``) |mdash| a system-wide UUID or node-local
  OS name (``char*``) identifying a particular device.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_UNREACH`` |mdash| the request could not be satisfied locally and the
  local PMIx server could not be reached (or the caller is a server, or is not
  connected to a server).

For the blocking form, the value returned is the status of the completed
operation. For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates only
that the request was successfully initiated; the final status of the operation is
reported as the ``status`` argument to ``cbfunc``. Any other negative value
indicates an appropriate error condition. PMIx error constants are defined in
``pmix_common.h``.


NOTES
-----

The ``PMIX_DEVICE_DIST_CREATE`` and ``PMIX_DEVICE_DIST_FREE`` convenience macros
are deprecated. New code should manage ``pmix_device_distance_t`` arrays using
the ``PMIx_Device_distance_create()`` and ``PMIx_Device_distance_free()``
functions.

A process whose threads are not all bound to the same location may return
inconsistent results from calls to this API by different threads if the
``PMIX_CPUBIND_THREAD`` binding envelope was used when generating the ``cpuset``.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
