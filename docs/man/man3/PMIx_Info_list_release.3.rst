.. _man3-PMIx_Info_list_release:

PMIx_Info_list_release
======================

.. include_body

``PMIx_Info_list_release`` |mdash| Free an opaque
:ref:`pmix_info_t(5) <man5-pmix_info_t>` list and all of its entries


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_list_release(void *ptr);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Opaque list handle returned by
  :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`.


DESCRIPTION
-----------

Release the list identified by ``ptr``. All :ref:`pmix_info_t <man5-pmix_info_t>`
entries on the list are destructed |mdash| freeing the data each holds |mdash|
and the backing list object allocated by
:ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>` is itself freed. After
this call the handle is no longer valid and must not be reused.

This is the terminal step of the list-building workflow. It is safe (and
expected) to call ``PMIx_Info_list_release`` after
:ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`, because the
array produced by that conversion is an independent deep copy that is unaffected
by releasing the list.

Note that entries added with
:ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>` were stored by
reference and marked persistent; their payload is **not** freed by this call,
and remains the caller's responsibility.


RETURN VALUE
------------

``PMIx_Info_list_release`` returns no value (``void``).


NOTES
-----

Every handle obtained from
:ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>` must be released
exactly once to avoid leaking memory.


.. seealso::
   :ref:`PMIx_Info_list_start(3) <man3-PMIx_Info_list_start>`,
   :ref:`PMIx_Info_list_add(3) <man3-PMIx_Info_list_add>`,
   :ref:`PMIx_Info_list_insert(3) <man3-PMIx_Info_list_insert>`,
   :ref:`PMIx_Info_list_convert(3) <man3-PMIx_Info_list_convert>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
