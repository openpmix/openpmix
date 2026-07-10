.. _man3-PMIx_Info_list_get_size:

PMIx_Info_list_get_size
=======================

.. include_body

``PMIx_Info_list_get_size`` |mdash| Return the number of
:ref:`pmix_info_t(5) <man5-pmix_info_t>` entries on an opaque list


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   size_t PMIx_Info_list_get_size(void *ptr);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.


DESCRIPTION
-----------

Return the current number of :ref:`pmix_info_t <man5-pmix_info_t>` entries
accumulated on the list identified by ``ptr``. This is the count that
:ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>` would use to size
the resulting :ref:`pmix_data_array_t <man5-pmix_data_array_t>`.


RETURN VALUE
------------

Returns the number of entries currently on the list. A newly started list, or a
list from which all entries have been removed, returns ``0``.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_get_info(3) <man3-PMIx_Info_list_get_info>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
