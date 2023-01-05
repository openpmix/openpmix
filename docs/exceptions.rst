Exceptions to the PMIx Standard
===============================

Exceptions to the base PMIx Standard are listed here for each OpenPMIx
release. These exceptions are not indicative of any intent to stray
from the Standard, but instead represent the difference between the
pace of development of the library versus the normal Standard's
process. Accordingly, it is expected that the exceptions listed below
will make their way into a future release of the PMIx Standard and
then be removed from the list of exceptions in some future OpenPMIx
release.

Extensions
----------

OpenPMIx version |opmix_ver| is based on the PMIx |std_ver| Standard
and includes the following extensions.

APIs
^^^^

* ``PMIx_Data_embed`` - Embed a data payload into a buffer.

Constants
^^^^^^^^^

* ``PMIX_DATA_BUFFER`` - data type for packing/unpacking of
  ``pmix_data_buffer_t`` objects
* ``PMIX_DISK_STATS`` - data type for packing/unpacking of
  ``pmix_disk_stats_t`` objects
* ``PMIX_NET_STATS`` - data type for packing/unpacking of
  ``pmix_net_stats_t`` objects
* ``PMIX_NODE_STATS`` - data type for packing/unpacking of
  ``pmix_node_stats_t`` objects
* ``PMIX_PROC_STATS`` - data type for packing/unpacking of
  ``pmix_proc_stats_t`` objects
* ``PMIX_ERR_JOB_EXE_NOT_FOUND`` - specified executable not found
* ``PMIX_ERR_JOB_INSUFFICIENT_RESOURCES`` - insufficient resources to
  spawn job
* ``PMIX_ERR_JOB_SYS_OP_FAILED`` - system library operation failed
* ``PMIX_ERR_JOB_WDIR_NOT_FOUND`` - specified working directory not
  found
* ``PMIX_READY_FOR_DEBUG`` - event indicating job/proc is ready for
  debug (accompanied by ``PMIX_BREAKPOINT`` indicating where proc is
  waiting)
* ``PMIX_STOR_ACCESS`` - data type for packing/unpacking of
  ``pmix_storage_accessibility_t`` values
* ``PMIX_STOR_ACCESS_TYPE`` - data type for packing/unpacking of
  ``pmix_storage_access_type_t`` objects
* ``PMIX_STOR_MEDIUM`` - data type for packing/unpacking of
  ``pmix_storage_medium_t`` objects
* ``PMIX_STOR_PERSIST`` - data type for packing/unpacking of
  ``pmix_storage_persistence_t`` objects

Attributes
^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_BREAKPOINT``
     - ``pmix.brkpnt``
     - ``char*``
     - string ID of the breakpoint where the process(es) is(are) waiting

   * - ``PMIX_DEBUG_STOP_IN_APP``
     - ``pmix.dbg.notify``
     - varies
     - direct specified ranks to stop at application-specific point
       and notify ready-to-debug.

       Can be any of three data types:

       * ``bool`` - true indicating all ranks, false indicating none
       * ``pmix_rank_t`` - the rank of one proc, or WILDCARD for all
       * ``pmix_data_array_t`` if an array of individual processes are specified

   * - ``PMIX_ENVARS_HARVESTED``
     - ``pmix.evar.hvstd``
     - ``bool``
     - Envars have been harvested by the spawn requestor

   * - ``PMIX_IOF_FILE_ONLY``
     - ``pmix.iof.fonly``
     - ``bool``
     - output only into designated files - do not also output a copy
       to stdout/stderr

   * - ``PMIX_IOF_FILE_PATTERN``
     - ``pmix.iof.fpt``
     - ``bool``
     - Specified output file is to be treated as a pattern and not
       automatically annotated by nspace, rank, or other parameters

   * - ``PMIX_IOF_LOCAL_OUTPUT``
     - ``pmix.iof.local``
     - ``bool``
     - Write output streams to local stdout/err

   * - ``PMIX_IOF_MERGE_STDERR_STDOUT``
     - ``pmix.iof.mrg``
     - ``bool``
     - merge stdout and stderr streams from application procs

   * - ``PMIX_IOF_OUTPUT_TO_DIRECTORY``
     - ``pmix.iof.dir``
     - ``char*``
     - direct application output into files of form
       ``<directory>/<jobid>/rank.<rank>/stdout[err]``

   * - ``PMIX_IOF_OUTPUT_TO_FILE``
     - ``pmix.iof.file``
     - ``char*``
     - direct application output into files of form
       ``<filename>.rank`` with both stdout and stderr redirected into
       it

   * - ``PMIX_IOF_RANK_OUTPUT``
     - ``pmix.iof.rank``
     - ``bool``
     - Tag output with the rank it came from

   * - ``PMIX_IOF_OUTPUT_RAW``
     - ``pmix.iof.raw``
     - ``bool``
     - Do not buffer output to be written as complete lines - output
       characters as the stream delivers them

   * - ``PMIX_NODE_OVERSUBSCRIBED``
     - ``pmix.ndosub``
     - ``bool``
     - true if number of procs from this job on this node exceeds the
       number of slots allocated to it

   * - ``PMIX_SINGLETON``
     - ``pmix.singleton``
     - ``char*``
     - String representation (nspace.rank) of proc ID for the
       singleton the server was started to support

   * - ``PMIX_JOB_TIMEOUT``
     - ``pmix.job.time``
     - ``int``
     - time in sec before job should time out (0 => infinite)

   * - ``PMIX_SPAWN_TIMEOUT``
     - ``pmix.sp.time``
     - ``int``
     - time in sec before spawn operation should time out (0 =>
       infinite)

       Logically equivalent to passing the ``PMIX_TIMEOUT`` attribute to
       the PMIx_Spawn API,

       it is provided as a separate attribute to distinguish it from
       the ``PMIX_JOB_TIMEOUT`` attribute

   * - ``PMIX_LOCAL_COLLECTIVE_STATUS``
     - ``pmix.loc.col.st``
     - ``pmix_status_t``
     - status code for local collective operation being reported to host
       by server library

   * - ``PMIX_BIND_PROGRESS_THREAD``
     - ``pmix.bind.pt``
     - ``char*``
     - Comma-delimited ranges of CPUs that the internal PMIx progress
       thread shall be bound to

   * - ``PMIX_BIND_REQUIRED``
     - ``pmix.bind.reqd``
     - ``bool``
     - Return error if the internal PMIx progress thread cannot be bound


Datatypes
^^^^^^^^^

* ``pmix_disk_stats_t`` - contains statistics on disk read/write operations
* ``pmix_net_stats_t`` - contains statistics on network activity
* ``pmix_node_stats_t`` - contains statistics on node resource usage
* ``pmix_proc_stats_t`` - contains statistics on process resource usage

Datatype static initializers
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Static initializers were added for each complex data type (i.e., a data type
defined as a struct):

* ``PMIX_COORD_STATIC_INIT``
* ``PMIX_CPUSET_STATIC_INIT``
* ``PMIX_TOPOLOGY_STATIC_INIT``
* ``PMIX_GEOMETRY_STATIC_INIT``
* ``PMIX_DEVICE_DIST_STATIC_INIT``
* ``PMIX_BYTE_OBJECT_STATIC_INIT``
* ``PMIX_ENDPOINT_STATIC_INIT``
* ``PMIX_ENVAR_STATIC_INIT``
* ``PMIX_PROC_STATIC_INIT``
* ``PMIX_PROC_INFO_STATIC_INIT``
* ``PMIX_DATA_ARRAY_STATIC_INIT``
* ``PMIX_DATA_BUFFER_STATIC_INIT``
* ``PMIX_PROC_STATS_STATIC_INIT``
* ``PMIX_DISK_STATS_STATIC_INIT``
* ``PMIX_NET_STATS_STATIC_INIT``
* ``PMIX_NODE_STATS_STATIC_INIT``
* ``PMIX_VALUE_STATIC_INIT``
* ``PMIX_INFO_STATIC_INIT``
* ``PMIX_LOOKUP_STATIC_INIT``
* ``PMIX_APP_STATIC_INIT``
* ``PMIX_QUERY_STATIC_INIT``
* ``PMIX_REGATTR_STATIC_INIT``
* ``PMIX_FABRIC_STATIC_INIT``

Macros
^^^^^^

* ``PMIX_VALUE_XFER_DIRECT`` - directly transfer a pmix_value_t to another one (non-destructive copy)
* ``PMIX_XFER_PROCID`` - transfer a pmix_proc_t to another one (non-destructive copy)

Macros supporting ``pmix_disk_stats_t`` objects:

* ``PMIX_DISK_STATS_CONSTRUCT``
* ``PMIX_DISK_STATS_CREATE``
* ``PMIX_DISK_STATS_DESTRUCT``
* ``PMIX_DISK_STATS_FREE``
* ``PMIX_DISK_STATS_RELEASE``

Macros supporting ``pmix_net_stats_t`` objects:

* ``PMIX_NET_STATS_CONSTRUCT``
* ``PMIX_NET_STATS_CREATE``
* ``PMIX_NET_STATS_DESTRUCT``
* ``PMIX_NET_STATS_FREE``
* ``PMIX_NET_STATS_RELEASE``

Macros supporting ``pmix_node_stats_t`` objects:

* ``PMIX_NODE_STATS_CONSTRUCT``
* ``PMIX_NODE_STATS_CREATE``
* ``PMIX_NODE_STATS_DESTRUCT``
* ``PMIX_NODE_STATS_RELEASE``

Macros supporting ``pmix_proc_stats_t`` objects:

* ``PMIX_PROC_STATS_CONSTRUCT``
* ``PMIX_PROC_STATS_CREATE``
* ``PMIX_PROC_STATS_DESTRUCT``
* ``PMIX_PROC_STATS_FREE``
* ``PMIX_PROC_STATS_RELEASE``


Renamed Constants
-----------------

OpenPMIx version |opmix_ver| renamed the following constants:

* ``PMIX_DEBUG_WAIT_FOR_NOTIFY`` - renamed to ``PMIX_READY_FOR_DEBUG``


OpenPMIx v4.1.1 Extensions
--------------------------

.. warning:: I suggest taking this whole section and moving it to the git
             branch where OpenPMIx v4.1.1 resides.

OpenPMIx v4.1.1 is based on the PMIx v4.1 Standard and includes the following extensions:

APIs
^^^^

* ``PMIx_Data_embed`` - Embed a data payload into a buffer.


Constants
^^^^^^^^^

* ``PMIX_DATA_BUFFER`` - data type for packing/unpacking of ``pmix_data_buffer_t`` objects
* ``PMIX_DISK_STATS`` - data type for packing/unpacking of ``pmix_disk_stats_t`` objects
* ``PMIX_NET_STATS`` - data type for packing/unpacking of ``pmix_net_stats_t`` objects
* ``PMIX_NODE_STATS`` - data type for packing/unpacking of ``pmix_node_stats_t`` objects
* ``PMIX_PROC_STATS`` - data type for packing/unpacking of ``pmix_proc_stats_t`` objects
* ``PMIX_ERR_JOB_EXE_NOT_FOUND`` - specified executable not found
* ``PMIX_ERR_JOB_INSUFFICIENT_RESOURCES`` - insufficient resources to spawn job
* ``PMIX_ERR_JOB_SYS_OP_FAILED`` - system library operation failed
* ``PMIX_ERR_JOB_WDIR_NOT_FOUND`` - specified working directory not found
* ``PMIX_READY_FOR_DEBUG`` - event indicating job/proc is ready for
  debug (accompanied by ``PMIX_BREAKPOINT`` indicating where proc is
  waiting)


Attributes
^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_BREAKPOINT``
     - ``pmix.brkpnt``
     - ``char*``
     - string ID of the breakpoint where the process(es) is(are)
       waiting

   * - ``PMIX_DEBUG_STOP_IN_APP``
     - ``pmix.dbg.notify``
     - varies
     - direct specified ranks to stop at application-specific point
       and notify ready-to-debug.

       Can be any of three data types:

       * ``bool`` - true indicating all ranks, false indicating none
       * ``pmix_rank_t`` - the rank of one proc, or WILDCARD for all
       * ``pmix_data_array_t`` if an array of individual processes are specified

   * - ``PMIX_ENVARS_HARVESTED``
     - ``pmix.evar.hvstd``
     - ``bool``
     - Envars have been harvested by the spawn requestor

   * - ``PMIX_IOF_FILE_ONLY``
     - ``pmix.iof.fonly``
     - ``bool``
     - output only into designated files - do not also output a copy
       to stdout/stderr

   * - ``PMIX_IOF_FILE_PATTERN``
     - ``pmix.iof.fpt``
     - ``bool``
     - Specified output file is to be treated as a pattern and not
       automatically annotated by nspace, rank, or other parameters

   * - ``PMIX_IOF_LOCAL_OUTPUT``
     - ``pmix.iof.local``
     - ``bool``
     - Write output streams to local stdout/err

   * - ``PMIX_IOF_OUTPUT_RAW``
     - ``pmix.iof.raw``
     - ``bool``
     - Do not buffer output to be written as complete lines - output
       characters as the stream delivers them

   * - ``PMIX_IOF_MERGE_STDERR_STDOUT``
     - ``pmix.iof.mrg``
     - ``bool``
     - merge stdout and stderr streams from application procs

   * - ``PMIX_IOF_OUTPUT_TO_DIRECTORY``
     - ``pmix.iof.dir``
     - ``char*``
     - direct application output into files of form
       ``<directory>/<jobid>/rank.<rank>/stdout[err]``

   * - ``PMIX_IOF_OUTPUT_TO_FILE``
     - ``pmix.iof.file``
     - ``char*``
     - direct application output into files of form
       ``<filename>.rank`` with both stdout and stderr redirected into
       it

   * - ``PMIX_IOF_RANK_OUTPUT``
     - ``pmix.iof.rank``
     - ``bool``
     - Tag output with the rank it came from

   * - ``PMIX_NODE_OVERSUBSCRIBED``
     - ``pmix.ndosub``
     - ``bool``
     - true if number of procs from this job on this node exceeds the number of slots allocated to it

Datatypes
^^^^^^^^^

* ``pmix_disk_stats_t`` - contains statistics on disk read/write operations
* ``pmix_net_stats_t`` - contains statistics on network activity
* ``pmix_node_stats_t`` - contains statistics on node resource usage
* ``pmix_proc_stats_t`` - contains statistics on process resource usage

Datatype static initializers
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Static initializers were added for each complex data type (i.e., a data type
defined as a struct):

* ``PMIX_COORD_STATIC_INIT``
* ``PMIX_CPUSET_STATIC_INIT``
* ``PMIX_TOPOLOGY_STATIC_INIT``
* ``PMIX_GEOMETRY_STATIC_INIT``
* ``PMIX_DEVICE_DIST_STATIC_INIT``
* ``PMIX_BYTE_OBJECT_STATIC_INIT``
* ``PMIX_ENDPOINT_STATIC_INIT``
* ``PMIX_ENVAR_STATIC_INIT``
* ``PMIX_PROC_STATIC_INIT``
* ``PMIX_PROC_INFO_STATIC_INIT``
* ``PMIX_DATA_ARRAY_STATIC_INIT``
* ``PMIX_DATA_BUFFER_STATIC_INIT``
* ``PMIX_PROC_STATS_STATIC_INIT``
* ``PMIX_DISK_STATS_STATIC_INIT``
* ``PMIX_NET_STATS_STATIC_INIT``
* ``PMIX_NODE_STATS_STATIC_INIT``
* ``PMIX_VALUE_STATIC_INIT``
* ``PMIX_INFO_STATIC_INIT``
* ``PMIX_LOOKUP_STATIC_INIT``
* ``PMIX_APP_STATIC_INIT``
* ``PMIX_QUERY_STATIC_INIT``
* ``PMIX_REGATTR_STATIC_INIT``
* ``PMIX_FABRIC_STATIC_INIT``

Macros
^^^^^^

* ``PMIX_VALUE_XFER_DIRECT`` - directly transfer a ``pmix_value_t`` to another one (non-destructive copy)
* ``PMIX_XFER_PROCID`` - transfer a ``pmix_proc_t`` to another one (non-destructive copy)

Macros supporting ``pmix_disk_stats_t`` objects:

* ``PMIX_DISK_STATS_CONSTRUCT``
* ``PMIX_DISK_STATS_CREATE``
* ``PMIX_DISK_STATS_DESTRUCT``
* ``PMIX_DISK_STATS_FREE``
* ``PMIX_DISK_STATS_RELEASE``

Macros supporting ``pmix_net_stats_t`` objects:

* ``PMIX_NET_STATS_CONSTRUCT``
* ``PMIX_NET_STATS_CREATE``
* ``PMIX_NET_STATS_DESTRUCT``
* ``PMIX_NET_STATS_FREE``
* ``PMIX_NET_STATS_RELEASE``

Macros supporting ``pmix_node_stats_t`` objects:

* ``PMIX_NODE_STATS_CONSTRUCT``
* ``PMIX_NODE_STATS_CREATE``
* ``PMIX_NODE_STATS_DESTRUCT``
* ``PMIX_NODE_STATS_RELEASE``

Macros supporting ``pmix_proc_stats_t`` objects:

* ``PMIX_PROC_STATS_CONSTRUCT``
* ``PMIX_PROC_STATS_CREATE``
* ``PMIX_PROC_STATS_DESTRUCT``
* ``PMIX_PROC_STATS_FREE``
* ``PMIX_PROC_STATS_RELEASE``

Renamed Constants
-----------------

OpenPMIx v4.1.1 renamed one constant:

* ``PMIX_DEBUG_WAIT_FOR_NOTIFY`` - renamed to ``PMIX_READY_FOR_DEBUG``


OpenPMIx v4.1.0 Extensions
--------------------------

.. warning:: I suggest taking this whole section and moving it to the git
             branch where OpenPMIx v4.1.1 resides.

OpenPMIx v4.1.0 is based on the PMIx v4.1 Standard and includes the following extensions


APIs
^^^^

* ``PMIx_Data_embed`` - Embed a data payload into a buffer.


Constants
^^^^^^^^^

* ``PMIX_DATA_BUFFER`` - data type for packing/unpacking of ``pmix_data_buffer_t`` objects
* ``PMIX_DISK_STATS`` - data type for packing/unpacking of ``pmix_disk_stats_t`` objects
* ``PMIX_NET_STATS`` - data type for packing/unpacking of ``pmix_net_stats_t`` objects
* ``PMIX_NODE_STATS`` - data type for packing/unpacking of ``pmix_node_stats_t`` objects
* ``PMIX_PROC_STATS`` - data type for packing/unpacking of ``pmix_proc_stats_t`` objects
* ``PMIX_ERR_JOB_EXE_NOT_FOUND`` - specified executable not found
* ``PMIX_ERR_JOB_INSUFFICIENT_RESOURCES`` - insufficient resources to spawn job
* ``PMIX_ERR_JOB_SYS_OP_FAILED`` - system library operation failed
* ``PMIX_ERR_JOB_WDIR_NOT_FOUND`` - specified working directory not found
* ``PMIX_READY_FOR_DEBUG`` - event indicating job/proc is ready for
  debug (accompanied by ``PMIX_BREAKPOINT`` indicating where proc is
  waiting)


Attributes
^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - Attribute
     - Name
     - Type
     - Description

   * - ``PMIX_BREAKPOINT``
     - ``pmix.brkpnt``
     - ``char*``
     - string ID of the breakpoint where the process(es) is(are)
       waiting

   * - ``PMIX_ENVARS_HARVESTED``
     - ``pmix.evar.hvstd``
     - ``bool``
     - Envars have been harvested by the spawn requestor

   * - ``PMIX_IOF_FILE_ONLY``
     - ``pmix.iof.fonly``
     - ``bool``
     - output only into designated files - do not also output a copy
       to stdout/stderr

   * - ``PMIX_IOF_FILE_PATTERN``
     - ``pmix.iof.fpt``
     - ``bool``
     - Specified output file is to be treated as a pattern and not
       automatically annotated by nspace, rank, or other parameters

   * - ``PMIX_IOF_LOCAL_OUTPUT``
     - ``pmix.iof.local``
     - ``bool``
     - Write output streams to local stdout/err

   * - ``PMIX_IOF_MERGE_STDERR_STDOUT``
     - ``pmix.iof.mrg``
     - ``bool``
     - merge stdout and stderr streams from application procs

   * - ``PMIX_IOF_OUTPUT_TO_DIRECTORY``
     - ``pmix.iof.dir``
     - ``char*``
     - direct application output into files of form
       ``<directory>/<jobid>/rank.<rank>/stdout[err]``

   * - ``PMIX_IOF_OUTPUT_TO_FILE``
     - ``pmix.iof.file``
     - ``char*``
     - direct application output into files of form
       ``<filename>.rank`` with both stdout and stderr redirected into
       it

   * - ``PMIX_IOF_RANK_OUTPUT``
     - ``pmix.iof.rank``
     - ``bool``
     - Tag output with the rank it came from

   * - ``PMIX_NODE_OVERSUBSCRIBED``
     - ``pmix.ndosub``
     - ``bool``
     - true if number of procs from this job on this node exceeds the
       number of slots allocated to it

The following attributes were also included in OpenPMIx v4.1.0 as an
early form of support for the Storage portion of the Standard, but
later dropped by the Standard:

* ``PMIX_STORAGE_AVAIL_BW``
* ``PMIX_STORAGE_BW``
* ``PMIX_STORAGE_CAPACITY_AVAIL``
* ``PMIX_STORAGE_CAPACITY_FREE``
* ``PMIX_STORAGE_OBJECTS_AVAIL``
* ``PMIX_STORAGE_OBJECTS_FREE``


Datatypes
^^^^^^^^^

* ``pmix_disk_stats_t`` - contains statistics on disk read/write operations
* ``pmix_net_stats_t`` - contains statistics on network activity
* ``pmix_node_stats_t`` - contains statistics on node resource usage
* ``pmix_proc_stats_t`` - contains statistics on process resource usage


Macros
^^^^^^

* ``PMIX_VALUE_XFER_DIRECT`` - directly transfer a ``pmix_value_t`` to
  another one (non-destructive copy)
* ``PMIX_XFER_PROCID`` - transfer a ``pmix_proc_t`` to another one
  (non-destructive copy)

Macros supporting ``pmix_disk_stats_t`` objects:

* ``PMIX_DISK_STATS_CONSTRUCT``
* ``PMIX_DISK_STATS_CREATE``
* ``PMIX_DISK_STATS_DESTRUCT``
* ``PMIX_DISK_STATS_FREE``
* ``PMIX_DISK_STATS_RELEASE``

Macros supporting ``pmix_net_stats_t`` objects:

* ``PMIX_NET_STATS_CONSTRUCT``
* ``PMIX_NET_STATS_CREATE``
* ``PMIX_NET_STATS_DESTRUCT``
* ``PMIX_NET_STATS_FREE``
* ``PMIX_NET_STATS_RELEASE``

Macros supporting ``pmix_node_stats_t`` objects:

* ``PMIX_NODE_STATS_CONSTRUCT``
* ``PMIX_NODE_STATS_CREATE``
* ``PMIX_NODE_STATS_DESTRUCT``
* ``PMIX_NODE_STATS_RELEASE``

Macros supporting ``pmix_proc_stats_t`` objects:

* ``PMIX_PROC_STATS_CONSTRUCT``
* ``PMIX_PROC_STATS_CREATE``
* ``PMIX_PROC_STATS_DESTRUCT``
* ``PMIX_PROC_STATS_FREE``
* ``PMIX_PROC_STATS_RELEASE``


Exclusions
----------

OpenPMIx v4.1.0 **DOES NOT INCLUDE** support for the storage
definitions in the PMIx v4.1 Standard, and renamed one constant. This
includes the following.

Renamed Constants
^^^^^^^^^^^^^^^^^

* ``PMIX_DEBUG_WAIT_FOR_NOTIFY`` - renamed to ``PMIX_READY_FOR_DEBUG``


Attributes
^^^^^^^^^^

* ``PMIX_STORAGE_ACCESSIBILITY``
* ``PMIX_STORAGE_ACCESSIBILITY_CLUSTER``
* ``PMIX_STORAGE_ACCESSIBILITY_JOB``
* ``PMIX_STORAGE_ACCESSIBILITY_NODE``
* ``PMIX_STORAGE_ACCESSIBILITY_RACK``
* ``PMIX_STORAGE_ACCESSIBILITY_REMOTE``
* ``PMIX_STORAGE_ACCESSIBILITY_SESSION``
* ``PMIX_STORAGE_ACCESS_RD``
* ``PMIX_STORAGE_ACCESS_RDWR``
* ``PMIX_STORAGE_ACCESS_TYPE``
* ``PMIX_STORAGE_ACCESS_WR``
* ``PMIX_STORAGE_BW_CUR``
* ``PMIX_STORAGE_BW_MAX``
* ``PMIX_STORAGE_CAPACITY_USED``
* ``PMIX_STORAGE_IOPS_CUR``
* ``PMIX_STORAGE_IOPS_MAX``
* ``PMIX_STORAGE_MEDIUM``
* ``PMIX_STORAGE_MEDIUM_HDD``
* ``PMIX_STORAGE_MEDIUM_NVME``
* ``PMIX_STORAGE_MEDIUM_PMEM``
* ``PMIX_STORAGE_MEDIUM_RAM``
* ``PMIX_STORAGE_MEDIUM_SSD``
* ``PMIX_STORAGE_MEDIUM_TAPE``
* ``PMIX_STORAGE_MEDIUM_UNKNOWN``
* ``PMIX_STORAGE_MINIMAL_XFER_SIZE``
* ``PMIX_STORAGE_OBJECTS_USED``
* ``PMIX_STORAGE_PERSISTENCE``
* ``PMIX_STORAGE_PERSISTENCE_ARCHIVE``
* ``PMIX_STORAGE_PERSISTENCE_JOB``
* ``PMIX_STORAGE_PERSISTENCE_NODE``
* ``PMIX_STORAGE_PERSISTENCE_PROJECT``
* ``PMIX_STORAGE_PERSISTENCE_SCRATCH``
* ``PMIX_STORAGE_PERSISTENCE_SESSION``
* ``PMIX_STORAGE_PERSISTENCE_TEMPORARY``
* ``PMIX_STORAGE_SUGGESTED_XFER_SIZE``
* ``PMIX_STORAGE_VERSION``

Datatypes
^^^^^^^^^

* ``pmix_storage_access_type_t``
* ``pmix_storage_accessibility_t``
* ``pmix_storage_medium_t``
* ``pmix_storage_persistence_t``
