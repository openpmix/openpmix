.. _man3-PMIx_Value_free:

PMIx_Value_free
===============

.. include_body

``PMIx_Value_free`` |mdash| Release an array of
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structures allocated by
:ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Value_free(pmix_value_t *v, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``v``: Pointer to the array of :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_value_t(5) <man5-pmix_value_t>` structures
that was allocated by :ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`.
For each of the ``n`` elements, the routine first destructs the element's
contents |mdash| freeing any heap memory referenced by its payload, exactly
as :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>` does |mdash| and
then frees the array storage itself.

If ``v`` is ``NULL`` the routine returns immediately. The caller must supply
the same count ``n`` that was passed to
:ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`.

Do not use ``PMIx_Value_free`` on a value whose storage the caller supplied
(a stack or embedded :ref:`pmix_value_t(5) <man5-pmix_value_t>`); for those,
use :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`, which releases
the contents without attempting to free the structure storage.


RETURN VALUE
------------

``PMIx_Value_free`` returns no value (``void``).


NOTES
-----

``PMIx_Value_free`` is an OpenPMIx convenience routine. The corresponding
macro-style interface for this operation is ``PMIX_VALUE_FREE``.


.. seealso::
   :ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`,
   :ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>`,
   :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`,
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`PMIx_Value_xfer(3) <man3-PMIx_Value_xfer>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
