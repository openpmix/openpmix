.. _man3-PMIx_Proc_info_free:

PMIx_Proc_info_free
===================

.. include_body

``PMIx_Proc_info_free`` |mdash| Release an array of
:ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Proc_info_free(pmix_proc_info_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the array of :ref:`pmix_proc_info_t(5)
  <man5-pmix_proc_info_t>` structures to be released, as returned by
  :ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>`.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Destruct each of the ``n`` elements of the array |mdash| freeing any
``hostname`` and ``executable_name`` strings held by each element |mdash| and
then free the array storage itself. This is the counterpart to
:ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>`.

If ``p`` is ``NULL`` or ``n`` is zero, the function does nothing.

Only pass storage that was heap-allocated by
:ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>` to this function. A
structure that was initialized with :ref:`PMIx_Proc_info_construct(3)
<man3-PMIx_Proc_info_construct>` on caller-provided memory must be cleared with
:ref:`PMIx_Proc_info_destruct(3) <man3-PMIx_Proc_info_destruct>` instead |mdash|
passing it to ``PMIx_Proc_info_free`` would attempt to free storage the library
did not allocate.


RETURN VALUE
------------

``PMIx_Proc_info_free`` returns no value (``void``).


NOTES
-----

``PMIx_Proc_info_free`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_INFO_FREE`` macro calls this function and then sets the caller's
pointer to ``NULL``.


.. seealso::
   :ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>`,
   :ref:`PMIx_Proc_info_construct(3) <man3-PMIx_Proc_info_construct>`,
   :ref:`PMIx_Proc_info_destruct(3) <man3-PMIx_Proc_info_destruct>`,
   :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
