.. _man3-PMIx_Value_load:

PMIx_Value_load
===============

.. include_body

``PMIx_Value_load`` |mdash| Copy typed data into a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Value_load(pmix_value_t *val,
                                 const void *data,
                                 pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``val``: Pointer to the destination :ref:`pmix_value_t(5)
  <man5-pmix_value_t>` structure. On return, its ``type`` field is set to
  ``type`` and its ``data`` union holds a copy of the supplied data.


INPUT PARAMETERS
----------------

* ``data``: Pointer to the source data to be copied into ``val``. May be
  ``NULL``, in which case the value's payload is zeroed (and, when ``type``
  is ``PMIX_BOOL``, the boolean flag is set to ``true`` to reflect that the
  mere presence of the attribute implies "true").
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
  describing the data being loaded.


DESCRIPTION
-----------

Load data of an arbitrary PMIx data type into a
:ref:`pmix_value_t(5) <man5-pmix_value_t>`. The data is **copied** into the
value structure; the caller retains ownership of the original ``data`` and
may free or modify it after the call returns. For types that own heap
storage (strings, byte objects, data arrays, and similar), the copy is a
deep copy, so the resulting value must eventually be released with
:ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>` or
:ref:`PMIx_Value_free(3) <man3-PMIx_Value_free>`.

Because a single :ref:`pmix_value_t(5) <man5-pmix_value_t>` can hold any
PMIx type |mdash| including a compound ``pmix_data_array_t`` |mdash| the
load operation dispatches on ``type`` to copy the appropriate union member.

``PMIx_Value_load`` is the inverse of
:ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`, which extracts a copy
of the data back out of a value.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on completion.


NOTES
-----

``PMIx_Value_load`` is an OpenPMIx convenience routine. The corresponding
macro-style interface for this operation is ``PMIX_VALUE_LOAD``.


EXAMPLES
--------

Load a string into a value:

.. code-block:: c

    pmix_value_t val;

    PMIx_Value_construct(&val);
    PMIx_Value_load(&val, "example", PMIX_STRING);

    /* ... use val ... */

    PMIx_Value_destruct(&val);


.. seealso::
   :ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`,
   :ref:`PMIx_Value_xfer(3) <man3-PMIx_Value_xfer>`,
   :ref:`PMIx_Value_construct(3) <man3-PMIx_Value_construct>`,
   :ref:`PMIx_Value_destruct(3) <man3-PMIx_Value_destruct>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
