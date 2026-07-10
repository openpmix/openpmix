.. _man3-PMIx_Info_list_xfer_unique:

PMIx_Info_list_xfer_unique
==========================

.. include_body

``PMIx_Info_list_xfer_unique`` |mdash| Copy an existing
:ref:`pmix_info_t(5) <man5-pmix_info_t>` onto an opaque list only if its key is
not already present


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_xfer_unique(void *ptr,
                                            const pmix_info_t *info,
                                            bool overwrite);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``info``: Pointer to the source :ref:`pmix_info_t <man5-pmix_info_t>` to be
  copied onto the list.
* ``overwrite``: If ``true`` and an entry with the same key already exists, its
  value is replaced with a copy of the source value. If ``false``, an existing
  entry is left unchanged.


DESCRIPTION
-----------

``PMIx_Info_list_xfer_unique`` is the key-unique variant of
:ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`. The list identified by
``ptr`` is first scanned for an existing entry whose key matches that of the
source ``info``:

* If a match is found and ``overwrite`` is ``true``, the existing value is
  destructed and replaced with a copy of the source value. No new entry is
  appended.
* If a match is found and ``overwrite`` is ``false``, the list is left
  unchanged.
* If no match is found, a full copy of the source ``info`` is appended to the
  end of the list.

The transfer is a deep copy; the source ``info`` may be modified or freed once
the function returns.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the entry was added, updated, or intentionally
left unchanged; or a negative PMIx error constant such as ``PMIX_ERR_NOMEM`` if
a new entry could not be allocated.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_add_unique(3) <man3-PMIx_Info_list_add_unique>`,
   :ref:`PMIx_Info_list_add_value_unique(3) <man3-PMIx_Info_list_add_value_unique>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
