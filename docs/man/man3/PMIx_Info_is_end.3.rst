.. _man3-PMIx_Info_is_end:

PMIx_Info_is_end
================

.. include_body

``PMIx_Info_is_end`` |mdash| Test whether a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
marks the end of an array.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_is_end(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the ``PMIX_INFO_ARRAY_END`` flag is set in the ``flags`` field of
the referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure. This flag is
used to mark the final element of an array of ``pmix_info_t`` structures,
allowing the array to be traversed without a separately maintained count.

The end marker is applied with
:ref:`PMIx_Info_set_end(3) <man3-PMIx_Info_set_end>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_ARRAY_END`` flag is set in the structure,
and ``false`` otherwise.


.. seealso::
   :ref:`PMIx_Info_set_end(3) <man3-PMIx_Info_set_end>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
