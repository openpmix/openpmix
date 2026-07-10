.. _man3-PMIx_Data_array_init:

PMIx_Data_array_init
====================

.. include_body

``PMIx_Data_array_init`` |mdash| Initialize the fields of a caller-provided
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_array_init(pmix_data_array_t *p, pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller.
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` that the
  array will hold.


DESCRIPTION
-----------

Initialize the fields of a :ref:`pmix_data_array_t <man5-pmix_data_array_t>`
without allocating any element storage. The ``array`` pointer is set to
``NULL``, the ``type`` field is set to the supplied ``type``, and the ``size``
field is set to zero.

Unlike :ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`,
``PMIx_Data_array_init`` does not allocate a backing element array; it only
records the intended element type and leaves the array empty. It is useful when
the element storage will be assigned separately.


RETURN VALUE
------------

``PMIx_Data_array_init`` returns no value (``void``).


NOTES
-----

To also allocate the backing element array, use :ref:`PMIx_Data_array_construct(3)
<man3-PMIx_Data_array_construct>` instead. If element storage is later attached
to the ``array`` field, release it with :ref:`PMIx_Data_array_destruct(3)
<man3-PMIx_Data_array_destruct>`.


.. seealso::
   :ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`,
   :ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>`,
   :ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`,
   :ref:`PMIx_Data_array_free(3) <man3-PMIx_Data_array_free>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
