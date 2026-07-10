.. _man3-PMIx_Data_array_destruct:

PMIx_Data_array_destruct
========================

.. include_body

``PMIx_Data_array_destruct`` |mdash| Release the element storage of a
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_array_destruct(pmix_data_array_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
  whose element storage is to be released.


DESCRIPTION
-----------

Release the backing element array referenced by the ``array`` field of a
:ref:`pmix_data_array_t <man5-pmix_data_array_t>`. The release is type-aware:
based on the ``type`` field, each of the ``size`` elements has its own payload
released using the appropriate per-type routine (for example, string elements
are individually freed, and ``PMIX_INFO``, ``PMIX_VALUE``, ``PMIX_APP``, and
similar complex element types are freed via their type-specific free routines)
before the element array itself is freed. On return, ``array`` is set to
``NULL``, ``type`` is set to ``PMIX_UNDEF``, and ``size`` is set to zero.

``PMIx_Data_array_destruct`` does **not** free the storage of the
``pmix_data_array_t`` structure itself; it releases only the element storage it
references.


RETURN VALUE
------------

``PMIx_Data_array_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Data_array_destruct`` is the counterpart of
:ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`. To release
a ``pmix_data_array_t`` structure that was itself heap-allocated by
:ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`, use
:ref:`PMIx_Data_array_free(3) <man3-PMIx_Data_array_free>`, which destructs the
elements and then frees the structure.


.. seealso::
   :ref:`PMIx_Data_array_init(3) <man3-PMIx_Data_array_init>`,
   :ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`,
   :ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`,
   :ref:`PMIx_Data_array_free(3) <man3-PMIx_Data_array_free>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
