.. _man5-pmix_value_t:

pmix_value_t
============

.. include_body

`pmix_value_t` |mdash| Defines a value along with its type

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_value {
       pmix_data_type_t type;
       union {
           bool flag;
           uint8_t byte;
           char *string;
           size_t size;
           pid_t pid;
           int integer;
           int8_t int8;
           int16_t int16;
           int32_t int32;
           int64_t int64;
           unsigned int uint;
           uint8_t uint8;
           uint16_t uint16;
           uint32_t uint32;
           uint64_t uint64;
           float fval;
           double dval;
           struct timeval tv;
           time_t time;
           pmix_status_t status;
           pmix_rank_t rank;
           pmix_nspace_t *nspace;
           pmix_proc_t *proc;
           pmix_byte_object_t bo;
           pmix_persistence_t persist;
           pmix_scope_t scope;
           pmix_data_range_t range;
           pmix_proc_state_t state;
           pmix_proc_info_t *pinfo;
           pmix_data_array_t *darray;
           void *ptr;
           pmix_alloc_directive_t adir;
           pmix_resource_block_directive_t rbdir;
           pmix_envar_t envar;
           pmix_coord_t *coord;
           pmix_link_state_t linkstate;
           pmix_job_state_t jstate;
           pmix_topology_t *topo;
           pmix_cpuset_t *cpuset;
           pmix_locality_t locality;
           pmix_geometry_t *geometry;
           pmix_device_type_t devtype;
           pmix_device_t *device;
           pmix_device_distance_t *devdist;
           pmix_endpoint_t *endpoint;
           pmix_data_buffer_t *dbuf;
           pmix_resource_unit_t *resunit;
           pmix_node_pid_t *nodepid;
       } data;
   } pmix_value_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   foo = {'value': value, 'val_type': type}

where ``type`` is the PMIx datatype of ``value``, and
``value`` is the associated value expressed in the appropriate Python form
for the specified datatype


DESCRIPTION
-----------

The `pmix_value_t` structure is used to represent the value passed to `PMIx_Put` and retrieved by
``PMIx_Get``, as well as many of the other PMIx functions.

A collection of values may be specified under a single key by passing a `pmix_value_t` containing an array
of type `pmix_data_array_t`, with each array element containing its own object.


.. seealso::
   PMIx_Initialized(3)
