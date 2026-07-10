.. _man3-PMIx_Info_list_insert:

PMIx_Info_list_insert
=====================

.. include_body

``PMIx_Info_list_insert`` |mdash| Append an existing
:ref:`pmix_info_t(5) <man5-pmix_info_t>` to an opaque list by reference, without
copying its payload


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_insert(void *ptr, pmix_info_t *info);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``info``: Pointer to an existing :ref:`pmix_info_t <man5-pmix_info_t>` whose
  contents are to be placed on the list.


DESCRIPTION
-----------

Append the provided :ref:`pmix_info_t <man5-pmix_info_t>` to the end of the list
identified by ``ptr`` **without deep-copying its payload**. The structure is
copied shallowly (via ``memcpy``), so any pointers held in ``info`` |mdash| the
key string, value data, embedded arrays |mdash| are preserved and the list entry
references the same underlying memory as the caller's ``info``.

To prevent a later double-free, the new list entry is marked persistent
(``PMIX_INFO_SET_PERSISTENT``), meaning its payload will **not** be released when
the list is released or destructed. Ownership of the referenced memory therefore
remains with the caller.

This differs sharply from
:ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`, which performs a deep
copy and takes ownership of the copied data. Use ``PMIx_Info_list_insert`` only
when the caller intends to keep the source ``info`` (and its backing memory)
alive for as long as the list is in use.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, or a negative PMIx error constant such as
``PMIX_ERR_NOMEM`` if the list entry could not be allocated.


NOTES
-----

Because the entry is stored by reference and marked persistent, the caller must
ensure the memory referenced by ``info`` remains valid for the lifetime of the
list, and is responsible for freeing that memory itself. Converting the list via
:ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>` produces an
independent deep copy in the resulting array.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_prepend(3) <man3-PMIx_Info_list_prepend>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
