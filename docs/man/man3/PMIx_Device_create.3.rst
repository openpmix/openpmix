.. _man3-PMIx_Device_create:

PMIx_Device_create
==================

.. include_body

``PMIx_Device_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_device_t(5) <man5-pmix_device_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_device_t* PMIx_Device_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_device_t(5) <man5-pmix_device_t>` structures to
  allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_device_t <man5-pmix_device_t>`
structures on the heap and construct each element, as though
:ref:`PMIx_Device_construct(3) <man3-PMIx_Device_construct>` had been called on
every member (``uuid`` and ``osname`` set to ``NULL``, ``type`` set to
``PMIX_DEVTYPE_UNKNOWN``).

The returned array is owned by the caller and must eventually be released with
:ref:`PMIx_Device_free(3) <man3-PMIx_Device_free>`, which destructs the
contents of each element and frees the array storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of
:ref:`pmix_device_t <man5-pmix_device_t>` structures, or ``NULL`` if ``n`` is
zero or the allocation fails.


EXAMPLES
--------

Create and later release an array of three device structures:

.. code-block:: c

    pmix_device_t *devs;

    devs = PMIx_Device_create(3);
    if (NULL == devs) {
        /* handle allocation failure */
    }

    /* ... populate and use the array ... */

    PMIx_Device_free(devs, 3);


.. seealso::
   :ref:`PMIx_Device_free(3) <man3-PMIx_Device_free>`,
   :ref:`PMIx_Device_construct(3) <man3-PMIx_Device_construct>`,
   :ref:`PMIx_Device_destruct(3) <man3-PMIx_Device_destruct>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`
