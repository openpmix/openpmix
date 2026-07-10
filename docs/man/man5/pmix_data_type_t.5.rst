.. _man5-pmix_data_type_t:

pmix_data_type_t
================

.. include_body

`pmix_data_type_t` |mdash| Identifies the type of a PMIx data value

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint16_t pmix_data_type_t;
   #define PMIX_UNDEF                       0
   #define PMIX_BOOL                        1  // converted to/from native true/false to uint8 for pack/unpack
   #define PMIX_BYTE                        2  // a byte of data
   #define PMIX_STRING                      3  // NULL-terminated string
   #define PMIX_SIZE                        4  // size_t
   #define PMIX_PID                         5  // OS-pid
   #define PMIX_INT                         6
   #define PMIX_INT8                        7
   #define PMIX_INT16                       8
   #define PMIX_INT32                       9
   #define PMIX_INT64                      10
   #define PMIX_UINT                       11
   #define PMIX_UINT8                      12
   #define PMIX_UINT16                     13
   #define PMIX_UINT32                     14
   #define PMIX_UINT64                     15
   #define PMIX_FLOAT                      16
   #define PMIX_DOUBLE                     17
   #define PMIX_TIMEVAL                    18
   #define PMIX_TIME                       19
   #define PMIX_STATUS                     20
   #define PMIX_VALUE                      21
   #define PMIX_PROC                       22
   #define PMIX_APP                        23
   #define PMIX_INFO                       24
   #define PMIX_PDATA                      25
   // Hole left by deprecation/removal of PMIX_BUFFER
   #define PMIX_BYTE_OBJECT                27
   #define PMIX_KVAL                       28
   // Hole left by deprecation/removal of PMIX_MODEX
   #define PMIX_PERSIST                    30
   #define PMIX_POINTER                    31
   #define PMIX_SCOPE                      32
   #define PMIX_DATA_RANGE                 33
   #define PMIX_COMMAND                    34
   #define PMIX_INFO_DIRECTIVES            35
   #define PMIX_DATA_TYPE                  36
   #define PMIX_PROC_STATE                 37
   #define PMIX_PROC_INFO                  38
   #define PMIX_DATA_ARRAY                 39
   #define PMIX_PROC_RANK                  40
   #define PMIX_QUERY                      41
   #define PMIX_COMPRESSED_STRING          42  // string compressed with zlib
   #define PMIX_ALLOC_DIRECTIVE            43
   // Hole left by deprecation/removal of PMIX_INFO_ARRAY
   #define PMIX_IOF_CHANNEL                45
   #define PMIX_ENVAR                      46
   #define PMIX_COORD                      47
   #define PMIX_REGATTR                    48
   // Hole left by deprecation of PMIX_REGEX (use PMIX_REGEX2 instead)
   #define PMIX_JOB_STATE                  50
   #define PMIX_LINK_STATE                 51
   #define PMIX_PROC_CPUSET                52
   #define PMIX_GEOMETRY                   53
   #define PMIX_DEVICE_DIST                54
   #define PMIX_ENDPOINT                   55
   #define PMIX_TOPO                       56
   #define PMIX_DEVTYPE                    57
   #define PMIX_LOCTYPE                    58
   #define PMIX_COMPRESSED_BYTE_OBJECT     59
   #define PMIX_PROC_NSPACE                60
   #define PMIX_DATA_BUFFER                65
   #define PMIX_STOR_MEDIUM                66
   #define PMIX_STOR_ACCESS                67
   #define PMIX_STOR_PERSIST               68
   #define PMIX_STOR_ACCESS_TYPE           69
   #define PMIX_DEVICE                     70
   #define PMIX_RESBLOCK_DIRECTIVE         71
   #define PMIX_RESOURCE_UNIT              72
   #define PMIX_NODE_PID                   73
   #define PMIX_REGEX2                     74
   #define PMIX_ALLOC_INHERIT              75

   /* define a boundary for implementers so they can add their own data types */
   #define PMIX_DATA_TYPE_MAX     500

Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   type = PMIX_STRING

where ``type`` is one of the ``PMIX_*`` data type constants naming the
type of an associated value.


DESCRIPTION
-----------

The `pmix_data_type_t` is a `uint16_t` value that identifies the type of
data carried in a PMIx value (for example, the ``type`` field of a
:ref:`pmix_value_t(5) <man5-pmix_value_t>`). It also drives serialization:
the ``bfrops`` framework uses the data type to select the correct
pack/unpack routine when placing data on, or extracting data from, the wire.

The full, authoritative list of defined data types lives in
``pmix_common.h``; the constants copied above are reproduced from that
header. The values fall into the following broad categories:

* **Intrinsic scalars** |mdash| the native C scalar types, such as
  ``PMIX_BOOL``, ``PMIX_BYTE``, ``PMIX_SIZE``, ``PMIX_PID``, the signed and
  unsigned fixed-width integers (``PMIX_INT8`` through ``PMIX_UINT64``),
  ``PMIX_INT``/``PMIX_UINT``, ``PMIX_FLOAT``, ``PMIX_DOUBLE``,
  ``PMIX_TIMEVAL``, ``PMIX_TIME``, and ``PMIX_POINTER``.

* **Strings** |mdash| ``PMIX_STRING`` for a NULL-terminated character string,
  and its compressed form ``PMIX_COMPRESSED_STRING`` (a string that has been
  compressed with zlib for transmission).

* **Enumerated / directive types** |mdash| the typed PMIx enumerations and
  bit-mask directive types, such as ``PMIX_STATUS``, ``PMIX_PERSIST``,
  ``PMIX_SCOPE``, ``PMIX_DATA_RANGE``, ``PMIX_PROC_STATE``,
  ``PMIX_INFO_DIRECTIVES``, ``PMIX_ALLOC_DIRECTIVE``, ``PMIX_IOF_CHANNEL``,
  ``PMIX_JOB_STATE``, ``PMIX_LINK_STATE``, ``PMIX_DEVTYPE``,
  ``PMIX_LOCTYPE``, and ``PMIX_RESBLOCK_DIRECTIVE``. ``PMIX_DATA_TYPE`` names
  the `pmix_data_type_t` value itself.

* **PMIx structures** |mdash| the composite PMIx structs, each identified by
  its own data type, including ``PMIX_VALUE``
  (:ref:`pmix_value_t(5) <man5-pmix_value_t>`), ``PMIX_PROC``
  (:ref:`pmix_proc_t(5) <man5-pmix_proc_t>`), ``PMIX_INFO``
  (:ref:`pmix_info_t(5) <man5-pmix_info_t>`), ``PMIX_APP``, ``PMIX_PDATA``,
  ``PMIX_KVAL``, ``PMIX_BYTE_OBJECT``, ``PMIX_COMPRESSED_BYTE_OBJECT``,
  ``PMIX_PROC_INFO``, ``PMIX_QUERY``, ``PMIX_ENVAR``, ``PMIX_COORD``,
  ``PMIX_REGATTR``, ``PMIX_PROC_CPUSET``, ``PMIX_GEOMETRY``,
  ``PMIX_DEVICE_DIST``, ``PMIX_ENDPOINT``, ``PMIX_TOPO``, ``PMIX_DEVICE``,
  ``PMIX_PROC_NSPACE``, ``PMIX_DATA_BUFFER``, ``PMIX_RESOURCE_UNIT``,
  ``PMIX_NODE_PID``, and the storage descriptors ``PMIX_STOR_MEDIUM``,
  ``PMIX_STOR_ACCESS``, ``PMIX_STOR_PERSIST``, and ``PMIX_STOR_ACCESS_TYPE``.

* **Arrays** |mdash| ``PMIX_DATA_ARRAY`` identifies a
  :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`, a self-describing
  array whose elements may themselves be any of the above data types. This is
  the mechanism by which a collection of values of a single type is carried
  in one PMIx value.

``PMIX_UNDEF`` (value 0) indicates that no type has been assigned. Several
numeric values are intentionally skipped (marked as *holes* above) where a
data type was deprecated and removed; those values must never be reused, as
doing so would break wire compatibility with peers built against earlier
releases. ``PMIX_DATA_TYPE_MAX`` (500) establishes a boundary above which
implementers are free to define their own private data types.

A human-readable string representation of a `pmix_data_type_t` value can be
obtained from :ref:`PMIx_Data_type_string(3) <man3-PMIx_Data_type_string>`.


.. seealso::
   :ref:`PMIx_Data_type_string(3) <man3-PMIx_Data_type_string>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
