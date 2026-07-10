.. _man3-PMIx_Device_construct:

PMIx_Device_construct
=====================

.. include_body

``PMIx_Device_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_device_t(5) <man5-pmix_device_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Device_construct(pmix_device_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_device_t(5) <man5-pmix_device_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on
  the stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_device_t <man5-pmix_device_t>` that the
caller has already allocated. The function zeroes the entire structure |mdash|
clearing the ``uuid`` and ``osname`` string pointers |mdash| and sets the
``type`` field to ``PMIX_DEVTYPE_UNKNOWN``. No heap memory is allocated.

A structure must be constructed (or created with
:ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`) before any of its
fields are used; operating on an uninitialized structure produces undefined
behavior.

Because ``PMIx_Device_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Device_destruct(3) <man3-PMIx_Device_destruct>` to
release any ``uuid`` or ``osname`` strings the structure subsequently
acquires. Do **not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Device_free(3) <man3-PMIx_Device_free>`, as that would attempt to
free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Device_construct`` returns no value (``void``).


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_device_t dev;

    PMIx_Device_construct(&dev);


.. seealso::
   :ref:`PMIx_Device_destruct(3) <man3-PMIx_Device_destruct>`,
   :ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`,
   :ref:`PMIx_Device_free(3) <man3-PMIx_Device_free>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`
