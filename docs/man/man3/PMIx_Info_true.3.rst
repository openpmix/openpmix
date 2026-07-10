.. _man3-PMIx_Info_true:

PMIx_Info_true
==============

.. include_body

``PMIx_Info_true`` |mdash| Interpret the value of a :ref:`pmix_info_t(5) <man5-pmix_info_t>` as a boolean


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_boolean_t PMIx_Info_true(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure whose
  value is to be examined.


DESCRIPTION
-----------

The ``PMIx_Info_true`` function examines the value carried by an info structure
and reports its boolean interpretation. It is a convenience for the common case
where the presence of a key conveys a boolean directive.

The value is interpreted as follows:

* A value type of ``PMIX_UNDEF`` is treated as ``true`` |mdash| the mere presence
  of the key defaults to indicating "true".
* A ``PMIX_BOOL`` value reports its stored flag directly.
* A ``PMIX_STRING`` value is parsed leniently. An empty or all-whitespace string
  is ``true``; a numeric string is ``false`` if it evaluates to zero and ``true``
  otherwise; the words ``yes``, ``true``, ``y``, ``t``, and ``enable`` (case
  insensitive, matched by prefix) are ``true``, while ``no``, ``false``, ``n``,
  ``f``, and ``disable`` are ``false``.
* Any other value |mdash| including a string that matches none of the recognized
  tokens |mdash| is reported as non-boolean.


RETURN VALUE
------------

Returns a :ref:`pmix_boolean_t(5) <man5-pmix_boolean_t>` enumerated value:

* ``PMIX_BOOL_TRUE``: the value was interpreted as boolean true.
* ``PMIX_BOOL_FALSE``: the value was interpreted as boolean false.
* ``PMIX_NON_BOOL``: the value could not be interpreted as a boolean.

Callers must distinguish all three results |mdash| in particular,
``PMIX_NON_BOOL`` must not be treated as equivalent to ``PMIX_BOOL_FALSE``.


NOTES
-----

``PMIx_Info_true`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_TRUE`` macro, which is retained in ``pmix_deprecated.h`` for
backward compatibility. Note that the macro form yields a plain C boolean (true
only when the result is ``PMIX_BOOL_TRUE``), whereas this function returns the
full three-valued :ref:`pmix_boolean_t(5) <man5-pmix_boolean_t>`.


.. seealso::
   :ref:`PMIx_Info_load(3) <man3-PMIx_Info_load>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_boolean_t(5) <man5-pmix_boolean_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
