.. _man3-PMIx_Info_list_add_value:

PMIx_Info_list_add_value
========================

.. include_body

``PMIx_Info_list_add_value`` |mdash| Append an entry to an opaque
:ref:`pmix_info_t(5) <man5-pmix_info_t>` list from a key and a
``pmix_value_t``


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_add_value(void *ptr,
                                          const char *key,
                                          const pmix_value_t *value);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``key``: String key to be loaded. Must be no longer than ``PMIX_MAX_KEYLEN``.
* ``value``: Pointer to a ``pmix_value_t`` whose contents are to be copied into
  the new entry.


DESCRIPTION
-----------

Create a new :ref:`pmix_info_t <man5-pmix_info_t>` entry from the provided
``key`` and an already-constructed ``pmix_value_t``, and append it to the end of
the list identified by ``ptr``. The key is loaded into the entry and the value
is transferred with a deep copy, so the caller's ``value`` may be modified or
freed after the function returns.

This routine is a convenience for cases where the caller already holds the data
in a ``pmix_value_t`` rather than as a raw pointer plus
:ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`. Compare
:ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`, which takes the raw
value and type directly. No duplicate-key check is performed; use
:ref:`PMIx_Info_list_add_value_unique(3) <man3-PMIx_Info_list_add_value_unique>`
for that.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, or a negative PMIx error constant on
failure |mdash| ``PMIX_ERR_NOMEM`` if the entry could not be allocated, or any
error returned while copying the value.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_add_value_unique(3) <man3-PMIx_Info_list_add_value_unique>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
