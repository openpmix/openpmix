.. _man3-PMIx_Data_array_free:

PMIx_Data_array_free
====================

.. include_body

``PMIx_Data_array_free`` |mdash| Release a
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` allocated by
:ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_array_free(pmix_data_array_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
  structure to be released.


DESCRIPTION
-----------

Release a :ref:`pmix_data_array_t <man5-pmix_data_array_t>` that was previously
allocated by :ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`.
The backing element array (and any per-element payload) is destructed via
:ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>`, and then the
``pmix_data_array_t`` structure itself is freed.

If ``p`` is ``NULL``, the function does nothing.


RETURN VALUE
------------

``PMIx_Data_array_free`` returns no value (``void``).


NOTES
-----

``PMIx_Data_array_free`` is the counterpart of :ref:`PMIx_Data_array_create(3)
<man3-PMIx_Data_array_create>`. Use it only on a structure that the library
allocated. For a caller-provided structure that was merely constructed with
:ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`, use
:ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>` instead |mdash|
calling ``PMIx_Data_array_free`` on such a structure would attempt to free
storage the library did not allocate.


.. seealso::
   :ref:`PMIx_Data_array_init(3) <man3-PMIx_Data_array_init>`,
   :ref:`PMIx_Data_array_construct(3) <man3-PMIx_Data_array_construct>`,
   :ref:`PMIx_Data_array_destruct(3) <man3-PMIx_Data_array_destruct>`,
   :ref:`PMIx_Data_array_create(3) <man3-PMIx_Data_array_create>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
