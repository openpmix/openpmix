.. _man3-PMIx_Info_list_add:

PMIx_Info_list_add
==================

.. include_body

``PMIx_Info_list_add`` |mdash| Append a key/value entry to an opaque
:ref:`pmix_info_t(5) <man5-pmix_info_t>` list


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_add(void *ptr,
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

Create a new :ref:`pmix_info_t <man5-pmix_info_t>` entry from the provided
``key``, ``value``, and ``type``, and append it to the end of the list
identified by ``ptr``. The key and value are **copied** into the entry, so the
caller's source key and any data referenced by ``value`` may be modified or
freed once the function returns without affecting the stored copy.

This routine does not check for a duplicate key; if the same key is added more
than once, multiple entries with that key will appear on the list. Use
:ref:`PMIx_Info_list_add_unique(3) <man3-PMIx_Info_list_add_unique>` when
duplicate suppression is required.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, or a negative PMIx error constant on
failure |mdash| most commonly ``PMIX_ERR_NOMEM`` if the entry could not be
allocated.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add_unique(3) <man3-PMIx_Info_list_add_unique>`,
   :ref:`PMIx_Info_list_add_value(3) <man3-PMIx_Info_list_add_value>`,
   :ref:`PMIx_Info_list_prepend(3) <man3-PMIx_Info_list_prepend>`,
   :ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
