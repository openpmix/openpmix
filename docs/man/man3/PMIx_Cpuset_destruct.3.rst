.. _man3-PMIx_Cpuset_destruct:

PMIx_Cpuset_destruct
====================

.. include_body

``PMIx_Cpuset_destruct`` |mdash| Release the contents of a
:ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Cpuset_destruct(pmix_cpuset_t *cpuset);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``cpuset``: Pointer to the :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release the contents of a :ref:`pmix_cpuset_t <man5-pmix_cpuset_t>`. The
``source`` string and the ``bitmap`` object are freed via the underlying
hardware-locality (HWLOC) support, and the corresponding fields are cleared.

Only the contents are released; the storage for the structure itself is not
freed. This routine is therefore the correct counterpart to
:ref:`PMIx_Cpuset_construct(3) <man3-PMIx_Cpuset_construct>` for
caller-provided (stack or embedded) structures.


RETURN VALUE
------------

``PMIx_Cpuset_destruct`` returns no value (``void``).


NOTES
-----

To release an array of cpuset structures obtained from
:ref:`PMIx_Cpuset_create(3) <man3-PMIx_Cpuset_create>`, including the array
storage itself, use :ref:`PMIx_Cpuset_free(3) <man3-PMIx_Cpuset_free>` instead.

The convenience macro ``PMIX_CPUSET_DESTRUCT`` is provided as a direct wrapper
around this function.


.. seealso::
   :ref:`PMIx_Cpuset_construct(3) <man3-PMIx_Cpuset_construct>`,
   :ref:`PMIx_Cpuset_create(3) <man3-PMIx_Cpuset_create>`,
   :ref:`PMIx_Cpuset_free(3) <man3-PMIx_Cpuset_free>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
