.. _man3-PMIx_Device_distance_construct:

PMIx_Device_distance_construct
==============================

.. include_body

``PMIx_Device_distance_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Device_distance_construct(pmix_device_distance_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the
  :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>` structure to
  be initialized. The storage for the structure itself must already exist
  |mdash| it is supplied by the caller (typically declared on the stack or
  embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a
:ref:`pmix_device_distance_t <man5-pmix_device_distance_t>` that the caller has
already allocated. The function zeroes the entire structure |mdash| clearing
the ``uuid`` and ``osname`` string pointers |mdash| sets the ``type`` field to
``PMIX_DEVTYPE_UNKNOWN``, and sets both the ``mindist`` and ``maxdist`` fields
to ``UINT16_MAX``. No heap memory is allocated.

A structure must be constructed (or created with
:ref:`PMIx_Device_distance_create(3) <man3-PMIx_Device_distance_create>`)
before any of its fields are used; operating on an uninitialized structure
produces undefined behavior.

Because ``PMIx_Device_distance_construct`` operates on caller-provided storage,
it must be paired with
:ref:`PMIx_Device_distance_destruct(3) <man3-PMIx_Device_distance_destruct>` to
release any ``uuid`` or ``osname`` strings the structure subsequently
acquires. Do **not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Device_distance_free(3) <man3-PMIx_Device_distance_free>`, as that
would attempt to free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Device_distance_construct`` returns no value (``void``).


NOTES
-----

The ``mindist`` and ``maxdist`` fields are initialized to ``UINT16_MAX``
rather than zero, so that an unset distance is distinguishable from a valid
distance of zero. Note that this differs from the
``PMIX_DEVICE_DIST_STATIC_INIT`` initializer, which sets both fields to zero.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_device_distance_t dist;

    PMIx_Device_distance_construct(&dist);


.. seealso::
   :ref:`PMIx_Device_distance_destruct(3) <man3-PMIx_Device_distance_destruct>`,
   :ref:`PMIx_Device_distance_create(3) <man3-PMIx_Device_distance_create>`,
   :ref:`PMIx_Device_distance_free(3) <man3-PMIx_Device_distance_free>`,
   :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>`
