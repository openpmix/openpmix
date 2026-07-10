.. _man3-PMIx_Pdata_free:

PMIx_Pdata_free
===============

.. include_body

``PMIx_Pdata_free`` |mdash| Release an array of
:ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structures allocated by
:ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Pdata_free(pmix_pdata_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the array of :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
  structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_pdata_t <man5-pmix_pdata_t>` structures that
was previously allocated by :ref:`PMIx_Pdata_create(3)
<man3-PMIx_Pdata_create>`. Each of the ``n`` elements is first destructed |mdash|
releasing any memory held by its ``value`` field, as
:ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>` would |mdash| and then
the array storage itself is freed.

If ``p`` is ``NULL`` or ``n`` is zero, the function does nothing.


RETURN VALUE
------------

``PMIx_Pdata_free`` returns no value (``void``).


NOTES
-----

Use ``PMIx_Pdata_free`` only on storage obtained from
:ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`. For a single structure
provided by the caller (stack or embedded), use
:ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>` instead, as
``PMIx_Pdata_free`` frees the array storage the library allocated.

The function does not modify the caller's pointer; after the call the caller
should discard or reset ``p`` to avoid using freed memory.


EXAMPLES
--------

Allocate and free an array:

.. code-block:: c

    pmix_pdata_t *array;

    array = PMIx_Pdata_create(4);
    /* ... populate and use array ... */
    PMIx_Pdata_free(array, 4);
    array = NULL;


.. seealso::
   :ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>`,
   :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>`,
   :ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`,
   :ref:`PMIx_Pdata_load(3) <man3-PMIx_Pdata_load>`,
   :ref:`PMIx_Pdata_xfer(3) <man3-PMIx_Pdata_xfer>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
