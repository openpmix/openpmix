.. _man3-PMIx_Info_list_add_unique:

PMIx_Info_list_add_unique
=========================

.. include_body

``PMIx_Info_list_add_unique`` |mdash| Append a key/value entry to an opaque
:ref:`pmix_info_t(5) <man5-pmix_info_t>` list only if the key is not already
present


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_add_unique(void *ptr,
                                           const char *key,
                                           const void *value,
                                           pmix_data_type_t type,
                                           bool overwrite);


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
* ``overwrite``: If ``true`` and an entry with the same key already exists, its
  value is replaced with the provided one. If ``false``, an existing entry is
  left unchanged.


DESCRIPTION
-----------

Add a key/value entry to the list identified by ``ptr``, enforcing key
uniqueness. The list is first scanned for an existing entry whose key matches
``key``:

* If a match is found and ``overwrite`` is ``true``, the existing value is
  destructed and replaced with a copy of the provided ``value``/``type``. No new
  entry is appended.
* If a match is found and ``overwrite`` is ``false``, the list is left
  unchanged.
* If no match is found, a new entry is created from the provided ``key``,
  ``value``, and ``type`` and appended to the end of the list.

As with :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`, the key and
value are copied; the caller's source data may be modified or freed once the
function returns.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the entry was added, updated, or intentionally
left unchanged; or a negative PMIx error constant such as ``PMIX_ERR_NOMEM`` if
a new entry could not be allocated.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_add_value_unique(3) <man3-PMIx_Info_list_add_value_unique>`,
   :ref:`PMIx_Info_list_xfer_unique(3) <man3-PMIx_Info_list_xfer_unique>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
