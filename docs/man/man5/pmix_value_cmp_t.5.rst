.. _man5-pmix_value_cmp_t:

pmix_value_cmp_t
================

.. include_body

`pmix_value_cmp_t` |mdash| Result of comparing two PMIx values

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef enum {
       PMIX_EQUAL,
       PMIX_VALUE1_GREATER,
       PMIX_VALUE2_GREATER,
       PMIX_VALUE_TYPE_DIFFERENT,
       PMIX_VALUE_INCOMPATIBLE_OBJECTS,
       PMIX_VALUE_COMPARISON_NOT_AVAIL
   } pmix_value_cmp_t;

Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   result = PMIX_EQUAL

where ``result`` is one of the ``pmix_value_cmp_t`` enumerated values.


DESCRIPTION
-----------

The `pmix_value_cmp_t` enumeration expresses the outcome of comparing two
PMIx values (typically two :ref:`pmix_value_t(5) <man5-pmix_value_t>`
structures). The defined values are:

* ``PMIX_EQUAL`` |mdash| the two values are of the same type and compare as
  equal.

* ``PMIX_VALUE1_GREATER`` |mdash| the two values are of comparable type and
  the first value is greater than the second.

* ``PMIX_VALUE2_GREATER`` |mdash| the two values are of comparable type and
  the second value is greater than the first.

* ``PMIX_VALUE_TYPE_DIFFERENT`` |mdash| the two values carry different data
  types and therefore cannot be ordered against one another.

* ``PMIX_VALUE_INCOMPATIBLE_OBJECTS`` |mdash| the two values are of the same
  type, but that type describes composite objects whose contents are not
  compatible for comparison.

* ``PMIX_VALUE_COMPARISON_NOT_AVAIL`` |mdash| a comparison operation is not
  available for the given data type.

A human-readable string representation of a `pmix_value_cmp_t` value can be
obtained from
:ref:`PMIx_Value_comparison_string(3) <man3-PMIx_Value_comparison_string>`.


.. seealso::
   :ref:`PMIx_Value_comparison_string(3) <man3-PMIx_Value_comparison_string>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
