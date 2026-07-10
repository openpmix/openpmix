.. _man3-PMIx_Value_true:

PMIx_Value_true
===============

.. include_body

``PMIx_Value_true`` |mdash| Determine the boolean interpretation of a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_boolean_t PMIx_Value_true(const pmix_value_t *v);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``v``: Pointer to the :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structure to be examined.


DESCRIPTION
-----------

Examine a :ref:`pmix_value_t(5) <man5-pmix_value_t>` and report whether it
represents a boolean "true" or "false", handling the several ways a boolean
can be encoded:

* A ``type`` of ``PMIX_UNDEF`` is taken to mean "true" |mdash| the mere
  presence of the attribute implies the value is set.
* A ``type`` of ``PMIX_BOOL`` returns the state of the stored flag.
* A ``type`` of ``PMIX_STRING`` is parsed for a boolean interpretation. A
  ``NULL`` or empty (whitespace-only) string is treated as "true". A leading
  numeric string is "false" when it evaluates to zero and "true" otherwise.
  The words ``yes``, ``true``, ``y``, ``t``, and ``enable`` (case
  insensitive) are "true"; ``no``, ``false``, ``n``, ``f``, and ``disable``
  are "false".

Any value that cannot be interpreted as a boolean is reported as such rather
than being forced to true or false.


RETURN VALUE
------------

Returns a :ref:`pmix_boolean_t(5) <man5-pmix_boolean_t>`:

* ``PMIX_BOOL_TRUE`` The value represents boolean true.
* ``PMIX_BOOL_FALSE`` The value represents boolean false.
* ``PMIX_NON_BOOL`` The value cannot be interpreted as a boolean.


NOTES
-----

``PMIx_Value_true`` is an OpenPMIx convenience routine.


.. seealso::
   :ref:`PMIx_Value_compare(3) <man3-PMIx_Value_compare>`,
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_boolean_t(5) <man5-pmix_boolean_t>`
