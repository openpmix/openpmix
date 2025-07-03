How to Add a PMIx Datatype
==========================

Adding a new datatype to PMIx is a laborious task that touches the library
in multiple places. This section is intended to serve as a step-by-step
guide to defining a new datatype and implementing all the necessary support
for it.

Step 1: Definitions
-------------------

New datatypes are defined in the ``include/pmix_common.h.in`` file. This file
serves as input to the PMIx configure operation, which outputs the final
user-facing version ``include/pmix_common.h`` - this is necessary to ensure
that the final version of the file includes a few configure-set flags.

The datatype definition includes four pieces:

Definition of the struct itself
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Almost all datatypes are structures of some kind, though that is not a requirement - a new datatype could simple be an abstraction over a standard C datatype (e.g., declaring something to represent a ``uint32_t``). Structures are required to be formatted as follows:

.. code-block:: c

  typedef struct pmix_newtype {
    uint32_t myval;
    pmix_foo_t foo;
  } pmix_newtype_t;

Static initializer
^^^^^^^^^^^^^^^^^^
In addition to this basic definition, we require that the developer provide a static initializer for the datatype. This helps users to initialize variables prior to execution. The static initializer should look something like:

.. code-block:: c

  #define PMIX_NEWTYPE_STATIC_INIT  \
  {                                 \
    .myval = 1,                     \
    .foo = PMIX_FOO_STATIC_INIT     \
  }

Datatype index
^^^^^^^^^^^^^^
Next, you need to define a new ``pmix_datatype_t`` value for the new datatype. This serves as an index to identify the type of value being passed to functions that pack, unpack, and otherwise manipulate the datatype. You will find these all defined in a block of code that is headed by:

.. code-block:: c

  /****    PMIX DATA TYPES    ****/
  typedef uint16_t pmix_data_type_t;
  #define PMIX_UNDEF                       0

The value you pick doesn't matter - just ensure it is one that has not already been defined. Note that there are some holes in the list - these represent previously used values for datatypes that have since been deprecated. Deprecated values cannot be reused as PMIx mandates full backward compatibility - it is therefore possible (albeit unlikely) that someone is using an older version of the library where that definition still exists. Thus, the values can only increase as time goes on. We recommend that you choose the next value in line with the end of the current datatype list.

Add to value struct
^^^^^^^^^^^^^^^^^^^
Finally, you probably need to add the new datatype to the ``pmix_value_t`` structure. Although this is not **required**, it is almost always true that a new datatype will be combined with some PMIx attribute, and thus used in a ``pmix_info_t`` structure. This in turn means that the value must somehow be passed in the associated ``pmix_value_t``.

   The definition of ``pmix_value_t`` looks like this:

.. code-block:: c

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

Note that all the data values are stored in a union. This is done to minimize the size of the structure since all values stored in memory (e.g., the initial job-level information and any exchanged connection info) are stored in ``pmix_value_t`` structures. Hence, it is critical that non-trivial data (e.g., structures) be included as pointers to such structures. For example, a PMIx data type that included a fixed-length character array could expand the size of the ``pmix_value_t`` by stretching the union to encompass the entire size of the array - thereby significantly impacting the overall size of the data storage.

At this point, the size of the union is set by the ``pmix_byte_object_t`` structure, which includes a ``char*`` pointer and a ``size_t`` value - hence the size of the union is 128-bits on a typical Linux system. Care should be taken to avoid expanding that size.


Step 2: Buffer Operations
-------------------------

If this is the first new datatype definition since the last release, then we have to create a new brops module. Explain how to construct it - copy the prior one, add new datatype registration connecting the new datatype to its support functions. Note where the "base" copy can be used for simple datatypes vs writing a separate dedicated function.

List the various functions that must be provided, and where implementation must go. Need to include the tma.h changes and probably explain why that level of indirection exists. Pack, unpack, print, copy, compare value/darray, macro backers, tma, value/darray size, value load/unload. Explain how/where all the support must be defined for pmix_data_array_t and pmix_value_t inclusion.

Step 3: Random Things
---------------------

If you need a caddy (e.g., to add instances of your new datatype to a PMIx list), then add the definition to pmix_globals.h. Class instance functions and declarations are done in pmix_globals.c
