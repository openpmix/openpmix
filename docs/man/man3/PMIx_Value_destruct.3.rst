.. _man3-PMIx_Value_destruct:

PMIx_Value_destruct
===================

.. include_body

``PMIx_Value_destruct`` |mdash| Release the contents of a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Value_destruct(pmix_value_t *val);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``val``: Pointer to the :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release any heap memory referenced by the payload of a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` |mdash| for example, a string,
a nested ``pmix_data_array_t``, a byte object, or any other type that owns
allocated storage. The specific action taken is determined by the value's
``type`` field. Scalar payloads (integers, booleans, sizes, and the like)
own no separate storage and require no release.

``PMIx_Value_destruct`` frees only the *contents* of the structure; it does
**not** free the storage occupied by the ``pmix_value_t`` itself. It is the
counterpart to :ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>`
and is intended for values whose storage the caller supplied (stack or
embedded). To release an array of values that was heap-allocated by
:ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`, use
:ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>` instead, which destructs
the contents *and* frees the array storage.


RETURN VALUE
------------

``PMIx_Value_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Value_destruct`` is an OpenPMIx convenience routine. The
corresponding macro-style interface for this operation is
``PMIX_VALUE_DESTRUCT``.


.. seealso::
   :ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>`,
   :ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`,
   :ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`,
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`PMIx_Value_xfer(3) <man3-PMIx_Value_xfer>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
