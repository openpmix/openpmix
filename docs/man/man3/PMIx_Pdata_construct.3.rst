.. _man3-PMIx_Pdata_construct:

PMIx_Pdata_construct
====================

.. include_body

``PMIx_Pdata_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Pdata_construct(pmix_pdata_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structure to
  be initialized. The storage for the structure itself must already exist
  |mdash| it is supplied by the caller (typically declared on the stack or
  embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_pdata_t <man5-pmix_pdata_t>` that the
caller has already allocated. The function zeroes the entire structure |mdash|
clearing the ``proc`` and ``key`` fields |mdash| and sets the contained
``value`` type to ``PMIX_UNDEF``. No heap memory is allocated and no payload is
attached.

Because ``PMIx_Pdata_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>` to
release any payload the structure's ``value`` subsequently acquires. Do
**not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`, as that would attempt to
free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Pdata_construct`` returns no value (``void``).


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_pdata_t pdata;

    PMIx_Pdata_construct(&pdata);


.. seealso::
   :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>`,
   :ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`,
   :ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`,
   :ref:`PMIx_Pdata_load(3) <man3-PMIx_Pdata_load>`,
   :ref:`PMIx_Pdata_xfer(3) <man3-PMIx_Pdata_xfer>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
