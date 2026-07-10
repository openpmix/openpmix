.. _man3-PMIx_Info_list_start:

PMIx_Info_list_start
====================

.. include_body

``PMIx_Info_list_start`` |mdash| Initialize an opaque list for accumulating
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structures


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void* PMIx_Info_list_start(void);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

None


DESCRIPTION
-----------

Constructing an array of :ref:`pmix_info_t <man5-pmix_info_t>` structures for
passing to a PMIx API can be tedious, since the ``pmix_info_t`` is not itself a
list object and the number of entries is frequently not known in advance. The
``PMIx_Info_list_start`` function begins that process by allocating an opaque,
implementation-dependent list object and returning a ``void*`` handle to it.

The returned handle is passed to the other ``PMIx_Info_list_*`` routines to
accumulate entries |mdash| for example
:ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
:ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`, or
:ref:`PMIx_Info_list_prepend(3) <man3-PMIx_Info_list_prepend>` |mdash| and is
ultimately converted into a contiguous
:ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>` of ``pmix_info_t`` via
:ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`. When the caller
is finished with the list, the handle must be passed to
:ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>` to free all
associated storage.

The object referenced by the returned handle is opaque: the caller must not
modify or dereference it.


RETURN VALUE
------------

Returns a ``void*`` handle to the newly created list, or ``NULL`` if the object
could not be allocated.


NOTES
-----

``PMIx_Info_list_start`` begins a workflow that is completed by
:ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`. Every handle
obtained from ``PMIx_Info_list_start`` must eventually be released to avoid
leaking memory.


EXAMPLES
--------

Build a list, convert it, and release it:

.. code-block:: c

    void *ilist;
    pmix_data_array_t darray;
    pmix_status_t rc;

    ilist = PMIx_Info_list_start();

    PMIx_Info_list_add(ilist, PMIX_RANK, &rank, PMIX_PROC_RANK);
    PMIx_Info_list_add(ilist, PMIX_HOSTNAME, host, PMIX_STRING);

    rc = PMIx_Info_list_convert(ilist, &darray);

    PMIx_Info_list_release(ilist);


.. seealso::
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_add_unique(3) <man3-PMIx_Info_list_add_unique>`,
   :ref:`PMIx_Info_list_add_value(3) <man3-PMIx_Info_list_add_value>`,
   :ref:`PMIx_Info_list_add_value_unique(3) <man3-PMIx_Info_list_add_value_unique>`,
   :ref:`PMIx_Info_list_prepend(3) <man3-PMIx_Info_list_prepend>`,
   :ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>`,
   :ref:`PMIx_Info_list_xfer(3) <man3-PMIx_Info_list_xfer>`,
   :ref:`PMIx_Info_list_xfer_unique(3) <man3-PMIx_Info_list_xfer_unique>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`PMIx_Info_list_get_info(3) <man3-PMIx_Info_list_get_info>`,
   :ref:`PMIx_Info_list_get_size(3) <man3-PMIx_Info_list_get_size>`,
   :ref:`PMIx_Info_list_release(3) <man3-PMIx_Info_list_release>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_data_array_t(5) <man5-pmix_data_array_t>`
