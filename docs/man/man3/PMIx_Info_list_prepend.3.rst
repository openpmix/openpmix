.. _man3-PMIx_Info_list_prepend:

PMIx_Info_list_prepend
======================

.. include_body

``PMIx_Info_list_prepend`` |mdash| Insert a key/value entry at the head of an
opaque :ref:`pmix_info_t(5) <man5-pmix_info_t>` list


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_prepend(void *ptr,
                                        const char *key,
                                        const void *value,
                                        pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``key``: String key to be loaded. Must be no longer than ``PMIX_MAX_KEYLEN``.
* ``value``: Pointer to the data value to be loaded.
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` of the provided value.


DESCRIPTION
-----------

``PMIx_Info_list_prepend`` behaves exactly like
:ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>` except that the new
:ref:`pmix_info_t <man5-pmix_info_t>` entry is inserted at the **head** of the
list identified by ``ptr`` rather than appended at the tail. This is useful when
the order of entries matters and a particular directive must appear first.

The key and value are copied into the entry, so the caller's source data may be
modified or freed once the function returns. No duplicate-key check is
performed.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, or a negative PMIx error constant on
failure |mdash| most commonly ``PMIX_ERR_NOMEM`` if the entry could not be
allocated.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
