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
           pmix_alloc_inheritance_t inheritance;
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
           pmix_regex2_t *regex2;
           pmix_info_directives_t infodirs;
           pmix_data_type_t dtype;
           pmix_iof_channel_t channel;
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
(see `Python forms`_ below).

Python forms
^^^^^^^^^^^^

A value handed back by the library has the form shown for its type, and can
be passed straight back in. A value that cannot be loaded is reported as a
status, never as an exception: ``PMIX_ERR_TYPE_MISMATCH`` for the wrong
Python type, ``PMIX_ERR_BAD_PARAM`` for a value out of range for its C type,
a struct dict missing a member, or a ``val_type`` that is not an integer from
0 to 65535, and ``PMIX_ERR_NOT_SUPPORTED`` for a type that has no Python
form.

.. list-table::
   :header-rows: 1

   * - **val_type**
     - **value**
   * - ``PMIX_UNDEF``
     - ``None`` (anything given is ignored)
   * - ``PMIX_BOOL``
     - ``True`` or ``False``. On input also 0 or 1, or the strings
       ``"true"``, ``"false"``, ``"t"`` or ``"f"`` in any case; any other
       string or number is ``PMIX_ERR_BAD_PARAM``.
   * - ``PMIX_BYTE``, ``PMIX_INT8`` ... ``PMIX_INT64``, ``PMIX_UINT8`` ...
       ``PMIX_UINT64``, ``PMIX_INT``, ``PMIX_UINT``, ``PMIX_SIZE``,
       ``PMIX_PID``, ``PMIX_TIME``, ``PMIX_STATUS``, ``PMIX_PROC_RANK``
     - ``int``, checked against both ends of the C type. A ``bool`` is
       not accepted.
   * - ``PMIX_PERSIST``, ``PMIX_SCOPE``, ``PMIX_DATA_RANGE``,
       ``PMIX_PROC_STATE``, ``PMIX_ALLOC_DIRECTIVE``,
       ``PMIX_RESBLOCK_DIRECTIVE``, ``PMIX_ALLOC_INHERIT``,
       ``PMIX_LINK_STATE``, ``PMIX_JOB_STATE``, ``PMIX_LOCTYPE``,
       ``PMIX_DEVTYPE``, ``PMIX_STOR_MEDIUM``, ``PMIX_STOR_ACCESS``,
       ``PMIX_STOR_PERSIST``, ``PMIX_STOR_ACCESS_TYPE``,
       ``PMIX_INFO_DIRECTIVES``, ``PMIX_DATA_TYPE``, ``PMIX_IOF_CHANNEL``
     - ``int``, one of the type's ``PMIX_*`` constants (or, for the
       directives and the channels, an OR of them), checked against the
       width of the type
   * - ``PMIX_FLOAT``, ``PMIX_DOUBLE``
     - ``float``. On input anything ``float()`` takes (a number or a numeric
       string) except a ``bool``.
   * - ``PMIX_TIMEVAL``
     - ``{'sec': int, 'usec': int}``; both required, ``usec`` from 0 to
       999999, a ``float`` truncated
   * - ``PMIX_STRING``
     - ``str`` (UTF-8) or ``bytes``, with no NUL in it; ``None`` is a NULL
       string. Handed back as ``str``, or ``bytes`` if it is not UTF-8.
   * - ``PMIX_PROC_NSPACE``
     - a namespace string; see :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
   * - ``PMIX_BYTE_OBJECT``, ``PMIX_COMPRESSED_BYTE_OBJECT``,
       ``PMIX_COMPRESSED_STRING``
     - a byte-object dict; see :ref:`pmix_byte_object_t(5)
       <man5-pmix_byte_object_t>`
   * - ``PMIX_REGEX``
     - the regex as ``bytes``. On input also a ``bytearray``,
       ``memoryview`` or ``str``, or a byte-object dict.
   * - ``PMIX_PROC``, ``PMIX_PROC_INFO``, ``PMIX_ENVAR``, ``PMIX_COORD``,
       ``PMIX_GEOMETRY``, ``PMIX_DEVICE``, ``PMIX_DEVICE_DIST``,
       ``PMIX_ENDPOINT``, ``PMIX_DATA_BUFFER``, ``PMIX_RESOURCE_UNIT``,
       ``PMIX_NODE_PID``, ``PMIX_REGEX2``, ``PMIX_REGATTR``,
       ``PMIX_DATA_ARRAY``
     - the dict on the type's own page (for example
       :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
       :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`). Except for
       ``PMIX_ENVAR``, ``None`` is the NULL the library allows there.
   * - ``PMIX_TOPO``, ``PMIX_PROC_CPUSET``
     - ``None`` only: "no topology" or "not bound". Any other value is
       ``PMIX_ERR_NOT_SUPPORTED``, and the library's value always comes back
       as ``None``. A cpuset is passed to and from the methods that take one
       as a :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>` dict instead.
   * - ``PMIX_POINTER``
     - not supported: a C pointer has no Python form
   * - ``PMIX_VALUE``, ``PMIX_INFO``, ``PMIX_APP``, ``PMIX_PDATA``,
       ``PMIX_QUERY``, ``PMIX_KVAL``, ``PMIX_COMMAND``
     - not supported as a value. Values, infos and apps are carried as the
       elements of a ``PMIX_DATA_ARRAY``.


DESCRIPTION
-----------

The `pmix_value_t` structure is used to represent the value passed to
:ref:`PMIx_Put(3) <man3-PMIx_Put>` and retrieved by
:ref:`PMIx_Get(3) <man3-PMIx_Get>`, as well as many of the other PMIx functions.

The ``type`` field is a :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` value
identifying which member of the ``data`` union is valid. A caller (or consumer)
must always set (or inspect) ``type`` before accessing the union; reading a
union member other than the one indicated by ``type`` is undefined.

The ``data`` union holds the value itself, expressed in the form appropriate to
``type``. Several of the union members are themselves PMIx datatypes, including
:ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>` (``bo``),
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` (``proc``),
:ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>` (``pinfo``),
:ref:`pmix_envar_t(5) <man5-pmix_envar_t>` (``envar``), and
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` (``darray``).

A collection of values may be specified under a single key by passing a
`pmix_value_t` that contains an array of type
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`, with each array element
containing its own object.

STATIC INITIALIZER
------------------

A statically declared ``pmix_value_t`` may be initialized with the
``PMIX_VALUE_STATIC_INIT`` macro, which sets ``type`` to ``PMIX_UNDEF`` and clears the data union:

.. code-block:: c

   pmix_value_t value = PMIX_VALUE_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Value_get_number(3) <man3-PMIx_Value_get_number>`,
   :ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
