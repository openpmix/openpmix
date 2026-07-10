.. _man3-PMIx_Device_distance_create:

PMIx_Device_distance_create
===========================

.. include_body

``PMIx_Device_distance_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_device_distance_t* PMIx_Device_distance_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of
  :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>` structures to
  allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n``
:ref:`pmix_device_distance_t <man5-pmix_device_distance_t>` structures on the
heap and construct each element, as though
:ref:`PMIx_Device_distance_construct(3) <man3-PMIx_Device_distance_construct>`
had been called on every member (``uuid`` and ``osname`` set to ``NULL``,
``type`` set to ``PMIX_DEVTYPE_UNKNOWN``, and both ``mindist`` and ``maxdist``
set to ``UINT16_MAX``).

The returned array is owned by the caller and must eventually be released with
:ref:`PMIx_Device_distance_free(3) <man3-PMIx_Device_distance_free>`, which
destructs the contents of each element and frees the array storage.


RETURN VALUE
------------

Returns a pointer to the newly allocated array of
:ref:`pmix_device_distance_t <man5-pmix_device_distance_t>` structures, or
``NULL`` if ``n`` is zero or the allocation fails.


EXAMPLES
--------

Create and later release an array of device-distance structures:

.. code-block:: c

    pmix_device_distance_t *dist;

    dist = PMIx_Device_distance_create(4);
    if (NULL == dist) {
        /* handle allocation failure */
    }

    /* ... populate and use the array ... */

    PMIx_Device_distance_free(dist, 4);


.. seealso::
   :ref:`PMIx_Device_distance_free(3) <man3-PMIx_Device_distance_free>`,
   :ref:`PMIx_Device_distance_construct(3) <man3-PMIx_Device_distance_construct>`,
   :ref:`PMIx_Device_distance_destruct(3) <man3-PMIx_Device_distance_destruct>`,
   :ref:`pmix_device_distance_t(5) <man5-pmix_device_distance_t>`
