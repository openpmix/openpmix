.. _man3-PMIx_Device_destruct:

PMIx_Device_destruct
====================

.. include_body

``PMIx_Device_destruct`` |mdash| Release the contents of a
:ref:`pmix_device_t(5) <man5-pmix_device_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Device_destruct(pmix_device_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_device_t(5) <man5-pmix_device_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release any heap memory referenced by the fields of a
:ref:`pmix_device_t <man5-pmix_device_t>` structure. Specifically, the
``uuid`` and ``osname`` strings are freed if they are non-``NULL``. The
storage for the structure itself is **not** freed |mdash| that remains the
responsibility of the caller, since a constructed structure occupies
caller-provided storage.

``PMIx_Device_destruct`` is the counterpart to
:ref:`PMIx_Device_construct(3) <man3-PMIx_Device_construct>` and should be
used on any structure that was initialized with it. To release an array of
structures that was allocated by
:ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`, use
:ref:`PMIx_Device_free(3) <man3-PMIx_Device_free>` instead, which both
destructs the contents and frees the array storage.


RETURN VALUE
------------

``PMIx_Device_destruct`` returns no value (``void``).


.. seealso::
   :ref:`PMIx_Device_construct(3) <man3-PMIx_Device_construct>`,
   :ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`,
   :ref:`PMIx_Device_free(3) <man3-PMIx_Device_free>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`
