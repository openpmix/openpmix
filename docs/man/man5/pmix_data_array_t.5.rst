.. _man5-pmix_data_array_t:

pmix_data_array_t
=================

.. include_body

`pmix_data_array_t` |mdash| Defines an array of like-typed values

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_data_array {
       pmix_data_type_t type;
       size_t size;
       void *array;
   } pmix_data_array_t;


DESCRIPTION
-----------

The `pmix_data_array_t` structure defines an array data structure |mdash| a
contiguous block of ``size`` elements, all of the same PMIx datatype ``type``.

* ``type`` |mdash| a :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` value
  identifying the datatype of every element stored in ``array``.
* ``size`` |mdash| the number of elements in ``array``.
* ``array`` |mdash| pointer to the contiguous block of ``size`` elements. Each
  element is of the C type corresponding to ``type`` (for example, an ``array``
  of :ref:`pmix_info_t(5) <man5-pmix_info_t>` when ``type`` is ``PMIX_INFO``).

``PMIX_DATA_ARRAY`` is itself a legal element type: when ``type`` is
``PMIX_DATA_ARRAY``, ``array`` points at a contiguous block of ``size``
`pmix_data_array_t` structures, each a complete array in its own right
(and each free to declare a different element type). The elements are
stored inline in the block |mdash| they are not pointers to arrays
elsewhere. Constructing such an array zeroes every element, so an element
the caller does not fill in is a valid empty array (``PMIX_UNDEF``,
``size`` 0) rather than uninitialized memory, and destructing the outer
array recursively releases every element.

Nesting is bounded. PMIx refuses to pack or unpack an array nested more
deeply inside other arrays than the ``bfrops_base_max_array_depth`` MCA
parameter permits |mdash| 100 levels by default, or unlimited if the
parameter is set to zero. The limit counts every form of nesting, both an
array whose element type is ``PMIX_DATA_ARRAY`` and the ordinary case of
an array reached through a `pmix_value_t`, because each level of either
costs the receiver a frame of recursion while costing the sender only a
few bytes on the wire.

The `pmix_data_array_t` structure is the mechanism by which a collection of
values is conveyed through a single :ref:`pmix_value_t(5) <man5-pmix_value_t>`
or :ref:`pmix_info_t(5) <man5-pmix_info_t>`: the containing structure references
a `pmix_data_array_t` (via the ``darray`` union member of a `pmix_value_t`),
which in turn points at the array of individual objects.

STATIC INITIALIZER
------------------

A statically declared ``pmix_data_array_t`` may be initialized with the
``PMIX_DATA_ARRAY_STATIC_INIT`` macro, which sets ``type`` to ``PMIX_UNDEF``, ``size`` to ``0``, and ``array`` to ``NULL``:

.. code-block:: c

   pmix_data_array_t array = PMIX_DATA_ARRAY_STATIC_INIT;


.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
