.. _man3-PMIx_Value_xfer:

PMIx_Value_xfer
===============

.. include_body

``PMIx_Value_xfer`` |mdash| Copy the contents of one
:ref:`pmix_value_t(5) <man5-pmix_value_t>` into another.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Value_xfer(pmix_value_t *dest,
                                 const pmix_value_t *src);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``dest``: Pointer to the destination :ref:`pmix_value_t(5)
  <man5-pmix_value_t>` structure. On success it receives a copy of the type
  and data held in ``src``.


INPUT PARAMETERS
----------------

* ``src``: Pointer to the source :ref:`pmix_value_t(5) <man5-pmix_value_t>`
  structure to be copied.


DESCRIPTION
-----------

Transfer the contents of ``src`` into ``dest``. Despite the name, the
operation is executed as a **copy**: the source value is not altered, and
for payloads that own heap storage (strings, byte objects, nested data
arrays, and the like) a deep copy is made so that ``dest`` and ``src`` do
not share memory. The ``type`` field of ``dest`` is set to match ``src``.

``dest`` must already have been initialized with
:ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>` (or created via
:ref:`PMIx_Value_create(3) <man3-PMIx_Value_create>`). Because the copy may
allocate memory, ``dest`` must eventually be released with
:ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>` or
:ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` The contents were copied successfully.
* ``PMIX_ERROR`` The source value has a type that is not supported by the
  transfer operation.

Other PMIx error constants (for example, an out-of-memory indication) may
be returned if copying a compound payload fails.


NOTES
-----

``PMIx_Value_xfer`` is an OpenPMIx convenience routine. The corresponding
macro-style interface for this operation is ``PMIX_VALUE_XFER``.


.. seealso::
   :ref:`PMIx_Value_load(3) <man3-PMIx_Value_load>`,
   :ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`,
   :ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>`,
   :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
