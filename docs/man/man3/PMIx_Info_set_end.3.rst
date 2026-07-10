.. _man3-PMIx_Info_set_end:

PMIx_Info_set_end
=================

.. include_body

``PMIx_Info_set_end`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
as the end of an array.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_set_end(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to mark.


DESCRIPTION
-----------

Set the ``PMIX_INFO_ARRAY_END`` flag in the ``flags`` field of the referenced
:ref:`pmix_info_t <man5-pmix_info_t>` structure. This marks the structure as
the final element of an array of ``pmix_info_t`` structures, allowing the array
to be traversed without a separately maintained count.

The marker is subsequently tested with
:ref:`PMIx_Info_is_end(3) <man3-PMIx_Info_is_end>`.


RETURN VALUE
------------

``PMIx_Info_set_end`` returns no value (``void``).


.. seealso::
   :ref:`PMIx_Info_is_end(3) <man3-PMIx_Info_is_end>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
