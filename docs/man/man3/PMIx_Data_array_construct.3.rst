.. _man3-PMIx_Data_array_construct:

PMIx_Data_array_construct
=========================

.. include_body

``PMIx_Data_array_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` and allocate its element
storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_array_construct(pmix_data_array_t *p,
                                  size_t num, pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller.
* ``num``: Number of elements to allocate in the backing array.
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` of the
  elements.


DESCRIPTION
-----------

Initialize a :ref:`pmix_data_array_t <man5-pmix_data_array_t>` and, when
``num`` is greater than zero, allocate and initialize a backing array of
``num`` elements of the given ``type``. The ``type`` and ``size`` fields are
set accordingly and the ``array`` field is set to point at the newly allocated
element storage.

The allocation is type-aware: for complex PMIx types (for example ``PMIX_INFO``,
``PMIX_PROC``, ``PMIX_VALUE``, ``PMIX_APP``, ``PMIX_ENVAR``, and many others)
the elements are created and constructed using the appropriate per-type
routine; for scalar types (for example ``PMIX_INT``, ``PMIX_SIZE``,
``PMIX_BOOL``, ``PMIX_STRING``) an array of the corresponding C type is
allocated and zeroed. If ``num`` is zero, no element storage is allocated and
``array`` is set to ``NULL``. If ``type`` is not recognized, ``array`` is set
to ``NULL`` and ``size`` is reset to zero.


RETURN VALUE
------------

``PMIx_Data_array_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Data_array_construct`` operates on caller-provided structure
storage but allocates the element array, it must be paired with
:ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>`, which
releases the element storage (and any per-element payload) without freeing the
``pmix_data_array_t`` structure itself. To also allocate the structure, use
:ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`. To initialize
the fields without allocating element storage, use :ref:`PMIx_Data_array_init(3)
<man3-PMIx_Data_array_init>`.


EXAMPLES
--------

Construct an array of four :ref:`pmix_info_t(5) <man5-pmix_info_t>` elements:

.. code-block:: c

    pmix_data_array_t darray;

    PMIx_Data_array_construct(&darray, 4, PMIX_INFO);
    /* ... populate darray.array[0..3] ... */
    PMIx_Data_array_destruct(&darray);


.. seealso::
   :ref:`PMIx_Data_array_init(3) <man3-PMIx_Data_array_init>`,
   :ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>`,
   :ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`,
   :ref:`PMIx_Data_array_free(3) <man3-PMIx_Data_array_free>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
