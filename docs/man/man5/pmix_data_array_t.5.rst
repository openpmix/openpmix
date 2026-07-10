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

The `pmix_data_array_t` structure is the mechanism by which a collection of
values is conveyed through a single :ref:`pmix_value_t(5) <man5-pmix_value_t>`
or :ref:`pmix_info_t(5) <man5-pmix_info_t>`: the containing structure references
a `pmix_data_array_t` (via the ``darray`` union member of a `pmix_value_t`),
which in turn points at the array of individual objects.

.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
