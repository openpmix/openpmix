.. _man3-PMIx_Info_list_xfer:

PMIx_Info_list_xfer
===================

.. include_body

``PMIx_Info_list_xfer`` |mdash| Copy an existing
:ref:`pmix_info_t(5) <man5-pmix_info_t>` onto an opaque list


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_list_xfer(void *ptr,
                                     const pmix_info_t *info);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.
* ``info``: Pointer to the source :ref:`pmix_info_t <man5-pmix_info_t>` to be
  copied onto the list.


DESCRIPTION
-----------

Create a new :ref:`pmix_info_t <man5-pmix_info_t>` entry on the list identified
by ``ptr`` and copy the full contents of the source ``info`` into it. All fields
|mdash| the key, the value, and the info directives (flags) |mdash| are
deep-copied. The new entry is appended to the end of the list.

Because the transfer is a copy, the source ``info`` (and any memory it
references) may be modified or freed once the function returns without affecting
the stored copy. This is the key distinction from
:ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>`, which stores the
source by reference and leaves ownership with the caller.

No duplicate-key check is performed; use
:ref:`PMIx_Info_list_xfer_unique(3) <man3-PMIx_Info_list_xfer_unique>` when
duplicate suppression is required.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, or a negative PMIx error constant such as
``PMIX_ERR_NOMEM`` if the entry could not be allocated.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_xfer_unique(3) <man3-PMIx_Info_list_xfer_unique>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
