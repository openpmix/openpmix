.. _man3-PMIx_Regattr_load:

PMIx_Regattr_load
=================

.. include_body

``PMIx_Regattr_load`` |mdash| Populate the fields of a
:ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regattr_load(pmix_regattr_t *info,
                          const char *name,
                          const char *key,
                          pmix_data_type_t type,
                          const char *description);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``info``: Pointer to the :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
  structure to be loaded. It must already have been initialized (for example
  with :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>`).
* ``name``: Human-readable name of the attribute (may be ``NULL``).
* ``key``: The attribute string key to store in the structure's ``string``
  field (may be ``NULL``).
* ``type``: The :ref:`data type <man5-pmix_regattr_t>` (``pmix_data_type_t``)
  of the value associated with the attribute.
* ``description``: A prose description of the attribute (may be ``NULL``).


DESCRIPTION
-----------

Populate the fields of a :ref:`pmix_regattr_t <man5-pmix_regattr_t>` structure
with the supplied values. The string arguments are **copied** into the
structure: ``name`` is duplicated into the ``name`` field, ``key`` is loaded
into the fixed-size ``string`` key field, and ``description`` is appended to the
``description`` string array. The ``type`` value is stored directly.

Any argument passed as ``NULL`` leaves the corresponding field unchanged from
its constructed (initialized) state; ``type`` is always assigned.

Because the string arguments are copied, the caller retains ownership of the
strings it passes in and may free or reuse them after the call. The copies held
by the structure are released by :ref:`PMIx_Regattr_destruct(3)
<man3-PMIx_Regattr_destruct>` or :ref:`PMIx_Regattr_free(3)
<man3-PMIx_Regattr_free>`.


RETURN VALUE
------------

``PMIx_Regattr_load`` returns no value (``void``).


NOTES
-----

The ``PMIX_REGATTR_LOAD`` convenience macro is a direct wrapper for this
function.

``PMIx_Regattr_load`` appends to the ``description`` array rather than
replacing it; call it on a freshly constructed structure to obtain a
single-element description.


EXAMPLES
--------

Load a registration-attribute structure:

.. code-block:: c

    pmix_regattr_t attr;

    PMIx_Regattr_construct(&attr);
    PMIx_Regattr_load(&attr, "PMIX_TIMEOUT",
                      PMIX_TIMEOUT, PMIX_INT,
                      "Time in seconds before the operation times out");
    /* ... use attr ... */
    PMIx_Regattr_destruct(&attr);


.. seealso::
   :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>`,
   :ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>`,
   :ref:`PMIx_Regattr_create(3) <man3-PMIx_Regattr_create>`,
   :ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>`,
   :ref:`PMIx_Regattr_xfer(3) <man3-PMIx_Regattr_xfer>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
