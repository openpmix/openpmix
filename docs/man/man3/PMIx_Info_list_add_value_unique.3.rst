.. _man3-PMIx_Info_list_add_value_unique:

PMIx_Info_list_add_value_unique
===============================

.. include_body

``PMIx_Info_list_add_value_unique`` |mdash| Append an entry to an opaque
:ref:`pmix_info_t(5) <man5-pmix_info_t>` list from a key and ``pmix_value_t``,
only if the key is not already present


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_add_value_unique(void *ptr,
                                                 const char *key,
                                                 const pmix_value_t *value,
                                                 bool overwrite);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``key``: String key to be loaded. Must be no longer than ``PMIX_MAX_KEYLEN``.
* ``value``: Pointer to a ``pmix_value_t`` whose contents are to be copied into
  the entry.
* ``overwrite``: If ``true`` and an entry with the same key already exists, its
  value is replaced with a copy of the provided one. If ``false``, an existing
  entry is left unchanged.


DESCRIPTION
-----------

Add a key/value entry to the list identified by ``ptr`` using an
already-constructed ``pmix_value_t``, enforcing key uniqueness. This is the
``pmix_value_t`` analogue of
:ref:`PMIx_Info_list_add_unique(3) <man3-PMIx_Info_list_add_unique>`.

The list is first scanned for an existing entry whose key matches ``key``:

* If a match is found and ``overwrite`` is ``true``, the existing value is
  destructed and replaced with a copy of the provided ``value``. No new entry is
  appended.
* If a match is found and ``overwrite`` is ``false``, the list is left
  unchanged.
* If no match is found, a new entry is created from ``key`` and ``value`` and
  appended to the end of the list.

The value is transferred with a deep copy; the caller's ``value`` may be
modified or freed once the function returns.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the entry was added, updated, or intentionally
left unchanged; or a negative PMIx error constant such as ``PMIX_ERR_NOMEM`` if
a new entry could not be allocated, or any error returned while copying the
value.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add_value(3) <man3-PMIx_Info_list_add_value>`,
   :ref:`PMIx_Info_list_add_unique(3) <man3-PMIx_Info_list_add_unique>`,
   :ref:`PMIx_Info_list_xfer_unique(3) <man3-PMIx_Info_list_xfer_unique>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
