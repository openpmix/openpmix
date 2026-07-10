.. _man3-PMIx_Cpuset_construct:

PMIx_Cpuset_construct
=====================

.. include_body

``PMIx_Cpuset_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Cpuset_construct(pmix_cpuset_t *cpuset);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``cpuset``: Pointer to the :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on the
  stack or embedded within another object).


DESCRIPTION
-----------

Initialize a :ref:`pmix_cpuset_t <man5-pmix_cpuset_t>` that the caller has
already allocated. The function zeroes the entire structure, clearing the
``source`` string pointer and the ``bitmap`` pointer. No heap memory is
allocated and no bitmap is attached.


RETURN VALUE
------------

``PMIx_Cpuset_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Cpuset_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Cpuset_destruct(3) <man3-PMIx_Cpuset_destruct>` to
release any bitmap the structure subsequently acquires. Do **not** pass a
constructed (as opposed to created) structure to :ref:`PMIx_Cpuset_free(3)
<man3-PMIx_Cpuset_free>`, as that would attempt to free storage the library did
not allocate.

The convenience macro ``PMIX_CPUSET_CONSTRUCT`` is provided as a direct wrapper
around this function.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_cpuset_t cpuset;

    PMIx_Cpuset_construct(&cpuset);


.. seealso::
   :ref:`PMIx_Cpuset_destruct(3) <man3-PMIx_Cpuset_destruct>`,
   :ref:`PMIx_Cpuset_create(3) <man3-PMIx_Cpuset_create>`,
   :ref:`PMIx_Cpuset_free(3) <man3-PMIx_Cpuset_free>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
