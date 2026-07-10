.. _man5-pmix_boolean_t:

pmix_boolean_t
==============

.. include_body

`pmix_boolean_t` |mdash| Tri-state boolean enumeration

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef enum {
       PMIX_BOOL_TRUE,
       PMIX_BOOL_FALSE,
       PMIX_NON_BOOL
   } pmix_boolean_t;


DESCRIPTION
-----------

The `pmix_boolean_t` enumeration provides a tri-state boolean value. In
addition to the usual true and false states, it distinguishes a third state
that indicates the value being examined is not a boolean at all |mdash| for
example, when interpreting the value carried by a
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structure.

The enumerated values are:

* ``PMIX_BOOL_TRUE`` |mdash| The value is boolean and evaluates to true.
* ``PMIX_BOOL_FALSE`` |mdash| The value is boolean and evaluates to false.
* ``PMIX_NON_BOOL`` |mdash| The value is not a boolean value.


.. seealso::
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
