.. _man3-PMIx_Resource_unit_free:

PMIx_Resource_unit_free
=======================

.. include_body

``PMIx_Resource_unit_free`` |mdash| Release an array of
:ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>` structures allocated
by :ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Resource_unit_free(pmix_resource_unit_t *d, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the array of :ref:`pmix_resource_unit_t(5)
  <man5-pmix_resource_unit_t>` structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_resource_unit_t <man5-pmix_resource_unit_t>`
structures that was previously allocated by
:ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`. Each of
the ``n`` elements is first destructed as
:ref:`PMIx_Resource_unit_destruct(3) <man3-PMIx_Resource_unit_destruct>` would,
and then the array storage itself is freed.

If ``d`` is ``NULL``, the function does nothing.


RETURN VALUE
------------

``PMIx_Resource_unit_free`` returns no value (``void``).


NOTES
-----

Use ``PMIx_Resource_unit_free`` only on storage obtained from
:ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`. For a
single structure provided by the caller (stack or embedded), use
:ref:`PMIx_Resource_unit_destruct(3) <man3-PMIx_Resource_unit_destruct>`
instead, as ``PMIx_Resource_unit_free`` frees the array storage the library
allocated.

The function does not modify the caller's pointer; after the call the caller
should discard or reset ``d`` to avoid using freed memory.


EXAMPLES
--------

Allocate and free an array:

.. code-block:: c

    pmix_resource_unit_t *array;

    array = PMIx_Resource_unit_create(2);
    /* ... populate and use array ... */
    PMIx_Resource_unit_free(array, 2);
    array = NULL;


.. seealso::
   :ref:`PMIx_Resource_unit_construct(3) <man3-PMIx_Resource_unit_construct>`,
   :ref:`PMIx_Resource_unit_destruct(3) <man3-PMIx_Resource_unit_destruct>`,
   :ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
