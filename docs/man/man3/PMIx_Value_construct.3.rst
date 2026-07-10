.. _man3-PMIx_Value_construct:

PMIx_Value_construct
====================

.. include_body

``PMIx_Value_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Value_construct(pmix_value_t *val);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``val``: Pointer to the :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on
  the stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_value_t(5) <man5-pmix_value_t>` that
the caller has already allocated. The function zeroes the entire structure
and sets the ``type`` field to ``PMIX_UNDEF``. No heap memory is allocated
and no data payload is attached.

A value must be constructed (or created with
:ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`) before it is passed
to any load, transfer, or compare operation; using an uninitialized value
produces undefined behavior.

Because ``PMIx_Value_construct`` operates on caller-provided storage, it
must be paired with :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`
to release any payload the value subsequently acquires. Do **not** pass a
constructed (as opposed to created) value to
:ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`, as that would attempt to
free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Value_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Value_construct`` is an OpenPMIx convenience routine. The
corresponding macro-style interface for this operation is
``PMIX_VALUE_CONSTRUCT``.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_value_t val;

    PMIx_Value_construct(&val);


.. seealso::
   :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`,
   :ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`,
   :ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`,
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`PMIx_Value_xfer(3) <man3-PMIx_Value_xfer>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
