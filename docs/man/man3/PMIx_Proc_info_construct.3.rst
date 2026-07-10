.. _man3-PMIx_Proc_info_construct:

PMIx_Proc_info_construct
========================

.. include_body

``PMIx_Proc_info_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Proc_info_construct(pmix_proc_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
  structure to be initialized. The storage for the structure itself must already
  exist (caller supplied, typically declared on the stack or embedded within
  another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_proc_info_t <man5-pmix_proc_info_t>` that
the caller has already allocated. The function zeroes the entire structure
|mdash| clearing the embedded ``proc`` identifier, the ``hostname`` and
``executable_name`` pointers, the ``pid``, and the ``exit_code`` |mdash| and sets
the ``state`` field to ``PMIX_PROC_STATE_UNDEF``. No heap memory is allocated.

Because ``PMIx_Proc_info_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Proc_info_destruct(3)
<man3-PMIx_Proc_info_destruct>` to release any payload (such as the ``hostname``
or ``executable_name`` strings) the structure subsequently acquires. Do **not**
pass a constructed (as opposed to created) structure to
:ref:`PMIx_Proc_info_free(3) <man3-PMIx_Proc_info_free>`, as that would attempt
to free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Proc_info_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Proc_info_construct`` is an OpenPMIx convenience routine. The corresponding
``PMIX_PROC_INFO_CONSTRUCT`` macro is implemented as a direct call to this
function.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_proc_info_t pinfo;

    PMIx_Proc_info_construct(&pinfo);


.. seealso::
   :ref:`PMIx_Proc_info_destruct(3) <man3-PMIx_Proc_info_destruct>`,
   :ref:`PMIx_Proc_info_create(3) <man3-PMIx_Proc_info_create>`,
   :ref:`PMIx_Proc_info_free(3) <man3-PMIx_Proc_info_free>`,
   :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
