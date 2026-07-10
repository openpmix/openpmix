.. _man3-PMIx_Value_comparison_string:

PMIx_Value_comparison_string
============================

.. include_body

``PMIx_Value_comparison_string`` |mdash| Return the string representation of a
``pmix_value_cmp_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Value_comparison_string(pmix_value_cmp_t cmp);


INPUT PARAMETERS
----------------

* ``cmp``: A value-comparison result of type ``pmix_value_cmp_t``. Recognized
  values are ``PMIX_EQUAL``, ``PMIX_VALUE1_GREATER``, ``PMIX_VALUE2_GREATER``,
  ``PMIX_VALUE_TYPE_DIFFERENT``, ``PMIX_VALUE_COMPARISON_NOT_AVAIL``, and
  ``PMIX_VALUE_INCOMPATIBLE_OBJECTS``.


DESCRIPTION
-----------

Return a human-readable, statically-allocated string describing the given value
comparison result, such as that produced when comparing two
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structures. The recognized results map
to ``"EQUAL"``, ``"VALUE1 GREATER"``, ``"VALUE2 GREATER"``, ``"DIFFERENT TYPES"``,
``"COMPARISON NOT AVAILABLE"``, and ``"INCOMPATIBLE OBJECTS"``. If the value does
not correspond to a recognized comparison result, the fallback string
``"UNKNOWN VALUE"`` is returned.

The returned pointer refers to constant storage owned by the PMIx library. The
caller must **not** modify or free it.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated character string. The pointer
is always non-``NULL`` and must not be freed by the caller.


.. seealso::
   :ref:`PMIx_Value_get_number(3) <man3-PMIx_Value_get_number>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_value_cmp_t(5) <man5-pmix_value_cmp_t>`
