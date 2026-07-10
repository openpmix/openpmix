.. _man3-PMIx_Value_compare:

PMIx_Value_compare
==================

.. include_body

``PMIx_Value_compare`` |mdash| Compare the contents of two
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_value_cmp_t PMIx_Value_compare(pmix_value_t *v1,
                                       pmix_value_t *v2);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``v1``: Pointer to the first :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structure.
* ``v2``: Pointer to the second :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structure.


DESCRIPTION
-----------

Compare the type and payload of two :ref:`pmix_value_t(5)
<man5-pmix_value_t>` structures and report the relationship between them.
The comparison first considers the values' ``type`` fields; values of
different types are reported as different rather than ordered. For matching
types, the payloads are compared according to that type's semantics.

Neither input value is modified.


RETURN VALUE
------------

Returns a :ref:`pmix_value_cmp_t(5) <man5-pmix_value_cmp_t>` indicating the
result of the comparison:

* ``PMIX_EQUAL`` The two values are equal.
* ``PMIX_VALUE1_GREATER`` ``v1`` is greater than ``v2``.
* ``PMIX_VALUE2_GREATER`` ``v2`` is greater than ``v1``.
* ``PMIX_VALUE_TYPE_DIFFERENT`` The values are of different types and cannot
  be ordered.
* ``PMIX_VALUE_INCOMPATIBLE_OBJECTS`` The values reference objects that
  cannot be meaningfully compared.
* ``PMIX_VALUE_COMPARISON_NOT_AVAIL`` No comparison is available for the
  given value type.


NOTES
-----

``PMIx_Value_compare`` is an OpenPMIx convenience routine.


.. seealso::
   :ref:`PMIx_Value_true(3) <man3-PMIx_Value_true>`,
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`PMIx_Value_xfer(3) <man3-PMIx_Value_xfer>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_value_cmp_t(5) <man5-pmix_value_cmp_t>`
