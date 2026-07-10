.. _man3-PMIx_Proc_info_destruct:

PMIx_Proc_info_destruct
=======================

.. include_body

``PMIx_Proc_info_destruct`` |mdash| Release the contents of a
:ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Proc_info_destruct(pmix_proc_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
  structure whose contents are to be released. The storage for the structure
  itself is not freed.


DESCRIPTION
-----------

Release the heap-allocated payload of a :ref:`pmix_proc_info_t
<man5-pmix_proc_info_t>` structure. If the ``hostname`` or ``executable_name``
fields are non-``NULL``, the strings they point to are freed. The structure is
then re-initialized to its constructed state |mdash| zeroed, with ``state`` set
to ``PMIX_PROC_STATE_UNDEF`` |mdash| exactly as
:ref:`PMIx_Proc_info_construct(3) <man3-PMIx_Proc_info_construct>` would leave
it.

``PMIx_Proc_info_destruct`` is the counterpart to
:ref:`PMIx_Proc_info_construct(3) <man3-PMIx_Proc_info_construct>` and releases
only the contents of the structure, not the structure storage itself. To release
an array of proc info structures that was heap-allocated by
:ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>`, use
:ref:`PMIx_Proc_info_free(3) <man3-PMIx_Proc_info_free>` instead.


RETURN VALUE
------------

``PMIx_Proc_info_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Proc_info_destruct`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_INFO_DESTRUCT`` macro is implemented as a direct call to this
function.


.. seealso::
   :ref:`PMIx_Proc_info_construct(3) <man3-PMIx_Proc_info_construct>`,
   :ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>`,
   :ref:`PMIx_Proc_info_free(3) <man3-PMIx_Proc_info_free>`,
   :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
