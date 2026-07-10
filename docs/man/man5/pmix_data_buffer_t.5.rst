.. _man5-pmix_data_buffer_t:

pmix_data_buffer_t
==================

.. include_body

`pmix_data_buffer_t` |mdash| Describes a data buffer used for packing and unpacking

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_data_buffer {
       /** Start of my memory */
       char *base_ptr;
       /** Where the next data will be packed to (within the allocated
           memory starting at base_ptr) */
       char *pack_ptr;
       /** Where the next data will be unpacked from (within the
           allocated memory starting as base_ptr) */
       char *unpack_ptr;
       /** Number of bytes allocated (starting at base_ptr) */
       size_t bytes_allocated;
       /** Number of bytes used by the buffer (i.e., amount of data --
           including overhead -- packed in the buffer) */
       size_t bytes_used;
   } pmix_data_buffer_t;


DESCRIPTION
-----------

The `pmix_data_buffer_t` structure describes a data buffer used for
packing and unpacking of PMIx data. It provides an opaque, self-describing
container into which typed data can be serialized (packed) for transmission
or storage, and from which that data can subsequently be deserialized
(unpacked).

The structure carries the following fields:

* ``base_ptr`` |mdash| Pointer to the start of the memory region allocated
  for the buffer.
* ``pack_ptr`` |mdash| Pointer to the location, within the memory region
  starting at ``base_ptr``, at which the next data element will be packed.
* ``unpack_ptr`` |mdash| Pointer to the location, within the memory region
  starting at ``base_ptr``, from which the next data element will be
  unpacked.
* ``bytes_allocated`` |mdash| The number of bytes allocated for the buffer,
  starting at ``base_ptr``.
* ``bytes_used`` |mdash| The number of bytes used by the buffer |mdash| that
  is, the amount of data (including overhead) currently packed in the buffer.

Callers should treat these fields as opaque and manipulate a
`pmix_data_buffer_t` only through the provided support routines rather than
by directly accessing or modifying the fields. A statically declared
`pmix_data_buffer_t` may be initialized using the
``PMIX_DATA_BUFFER_STATIC_INIT`` initializer.


SUPPORT FUNCTIONS
-----------------

PMIx provides a number of functions and macros to support constructing,
destructing, allocating, releasing, loading, and unloading
`pmix_data_buffer_t` structures, along with packing and unpacking their
contents. These include:

* ``PMIX_DATA_BUFFER_CREATE`` |mdash| Allocate and initialize a
  `pmix_data_buffer_t` object.
* ``PMIX_DATA_BUFFER_RELEASE`` |mdash| Free a `pmix_data_buffer_t` object and
  the data it contains.
* ``PMIX_DATA_BUFFER_CONSTRUCT`` |mdash| Initialize the fields of a
  statically declared `pmix_data_buffer_t`.
* ``PMIX_DATA_BUFFER_DESTRUCT`` |mdash| Release the data held by a
  `pmix_data_buffer_t` (the companion to ``PMIX_DATA_BUFFER_CONSTRUCT``).
* ``PMIX_DATA_BUFFER_LOAD`` / ``PMIX_DATA_BUFFER_UNLOAD`` |mdash| Load a
  region of memory into, or extract it from, a `pmix_data_buffer_t`.
* ``PMIx_Data_pack`` |mdash| Pack one or more values of a given type into a
  `pmix_data_buffer_t`.
* ``PMIx_Data_unpack`` |mdash| Unpack one or more values of a given type from
  a `pmix_data_buffer_t`.

`pmix_data_buffer_t` is the buffer type manipulated by the
``PMIx_Data_pack``, ``PMIx_Data_unpack``, and ``PMIx_Data_buffer_*``
interfaces.

STATIC INITIALIZER
------------------

A statically declared ``pmix_data_buffer_t`` may be initialized with the
``PMIX_DATA_BUFFER_STATIC_INIT`` macro, which sets the ``base_ptr``, ``pack_ptr``, and ``unpack_ptr`` pointers to ``NULL`` and both ``bytes_allocated`` and ``bytes_used`` to ``0``:

.. code-block:: c

   pmix_data_buffer_t buffer = PMIX_DATA_BUFFER_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Data_pack(3) <man3-PMIx_Data_pack>`,
   :ref:`PMIx_Data_unpack(3) <man3-PMIx_Data_unpack>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
