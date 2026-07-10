.. _man3-PMIx_Data_array_create:

PMIx_Data_array_create
======================

.. include_body

``PMIx_Data_array_create`` |mdash| Allocate and initialize a
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` together with its element
storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_data_array_t* PMIx_Data_array_create(size_t n, pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of elements the array is to hold.
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` of the
  elements.


DESCRIPTION
-----------

Allocate a single :ref:`pmix_data_array_t <man5-pmix_data_array_t>` structure
from the heap and construct it (via :ref:`PMIx_Data_array_construct(3)
<man3-PMIx_Data_array_construct>`) so that its backing element array holds ``n``
elements of the given ``type``. Both the structure and its element storage are
owned by the caller.


RETURN VALUE
------------

Returns a pointer to the newly allocated and initialized
``pmix_data_array_t`` on success. Returns ``NULL`` if ``n`` is zero or if the
allocation of the structure fails.


NOTES
-----

``PMIx_Data_array_create`` both allocates and constructs, so it must be paired
with :ref:`PMIx_Data_array_free(3) <man3-PMIx_Data_array_free>`, which destructs
the elements and frees the structure. Do not pass a created array to
:ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>` alone, as
that releases the element storage without freeing the ``pmix_data_array_t``
structure itself.


EXAMPLES
--------

Create a data array of eight :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
elements:

.. code-block:: c

    pmix_data_array_t *darray;

    darray = PMIx_Data_array_create(8, PMIX_PROC);
    if (NULL == darray) {
        /* handle allocation failure */
    }
    /* ... populate darray->array ... */
    PMIx_Data_array_free(darray);


.. seealso::
   :ref:`PMIx_Data_array_init(3) <man3-PMIx_Data_array_init>`,
   :ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`,
   :ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>`,
   :ref:`PMIx_Data_array_free(3) <man3-PMIx_Data_array_free>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
