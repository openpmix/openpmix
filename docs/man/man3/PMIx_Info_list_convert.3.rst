.. _man3-PMIx_Info_list_convert:

PMIx_Info_list_convert
======================

.. include_body

``PMIx_Info_list_convert`` |mdash| Convert an opaque
:ref:`pmix_info_t(5) <man5-pmix_info_t>` list into a contiguous
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_convert(void *ptr, pmix_data_array_t *par);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``ptr``: (IN) Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``par``: (OUT) Pointer to a caller-instantiated
  :ref:`pmix_data_array_t <man5-pmix_data_array_t>` that will receive the
  resulting array of :ref:`pmix_info_t <man5-pmix_info_t>` structures.


DESCRIPTION
-----------

Transfer the accumulated contents of the list identified by ``ptr`` into the
:ref:`pmix_data_array_t <man5-pmix_data_array_t>` referenced by ``par``. The
function initializes ``par`` for type ``PMIX_INFO``, allocates an array of
:ref:`pmix_info_t <man5-pmix_info_t>` structures sized to the number of list
entries, sets ``par->type`` and ``par->size`` accordingly, and deep-copies each
list entry into successive elements of the array in list order.

The resulting array is fully independent of the list: the list may subsequently
be released (via
:ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`) without
affecting the converted array. This is the normal terminal step of the
list-building workflow |mdash| build the list, convert it to an array suitable
for passing to a PMIx API, then release the list.

The storage referenced by ``par->array`` is allocated by the library and
becomes the caller's responsibility; it should ultimately be freed with the
appropriate PMIx data-array release facilities (for example
``PMIX_DATA_ARRAY_DESTRUCT``).


RETURN VALUE
------------

Returns one of:

* ``PMIX_SUCCESS`` |mdash| the list was converted successfully.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``ptr`` or ``par`` was ``NULL``.
* ``PMIX_ERR_EMPTY`` |mdash| the list contained no entries; no array is
  allocated.
* ``PMIX_ERR_NOMEM`` |mdash| the destination array could not be allocated.


NOTES
-----

An empty list is reported as ``PMIX_ERR_EMPTY`` rather than producing a
zero-length array, so callers that may legitimately build an empty list should
handle that return value.


EXAMPLES
--------

.. code-block:: c

    void *ilist;
    pmix_data_array_t darray;
    pmix_status_t rc;

    ilist = PMIx_Info_list_start();
    PMIx_Info_list_add(ilist, PMIX_HOSTNAME, host, PMIX_STRING);

    rc = PMIx_Info_list_convert(ilist, &darray);
    if (PMIX_SUCCESS == rc) {
        /* darray.array now holds darray.size pmix_info_t entries */
    }

    PMIx_Info_list_release(ilist);


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_get_size(3) <man3-PMIx_Info_list_get_size>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
