.. _man3-PMIx_Regattr_construct:

PMIx_Regattr_construct
======================

.. include_body

``PMIx_Regattr_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regattr_construct(pmix_regattr_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on the
  stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_regattr_t <man5-pmix_regattr_t>` that the
caller has already allocated. The function sets the ``name`` pointer to
``NULL``, clears the ``string`` key field, sets ``type`` to ``PMIX_UNDEF``, and
sets the ``description`` pointer to ``NULL``. No heap memory is allocated.

Because ``PMIx_Regattr_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>` to
release any ``name`` or ``description`` the structure subsequently acquires
(for example via :ref:`PMIx_Regattr_load(3) <man3-PMIx_Regattr_load>`).


RETURN VALUE
------------

``PMIx_Regattr_construct`` returns no value (``void``).


NOTES
-----

The ``PMIX_REGATTR_CONSTRUCT`` convenience macro is a direct wrapper for this
function.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_regattr_t attr;

    PMIx_Regattr_construct(&attr);


.. seealso::
   :ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>`,
   :ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`,
   :ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>`,
   :ref:`PMIx_Regattr_load(3) <man3-PMIx_Regattr_load>`,
   :ref:`PMIx_Regattr_xfer(3) <man3-PMIx_Regattr_xfer>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
