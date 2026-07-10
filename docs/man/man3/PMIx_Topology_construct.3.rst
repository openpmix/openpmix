.. _man3-PMIx_Topology_construct:

PMIx_Topology_construct
=======================

.. include_body

``PMIx_Topology_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_topology_t(5) <man5-pmix_topology_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Topology_construct(pmix_topology_t *t);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``t``: Pointer to the :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on the
  stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_topology_t <man5-pmix_topology_t>` that
the caller has already allocated. The function zeroes the entire structure,
clearing the ``source`` string pointer and the ``topology`` pointer.

No heap memory is allocated and no topology object is attached; the caller is
responsible for populating the fields after construction.


RETURN VALUE
------------

``PMIx_Topology_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Topology_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Topology_destruct(3) <man3-PMIx_Topology_destruct>`
to release any payload the structure subsequently acquires. Do **not** pass a
constructed (as opposed to created) structure to :ref:`PMIx_Topology_free(3)
<man3-PMIx_Topology_free>`, as that would attempt to free storage the library
did not allocate.

An equivalent static initializer, ``PMIX_TOPOLOGY_STATIC_INIT``, is available
for initializing a structure at the point of declaration.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_topology_t topo;

    PMIx_Topology_construct(&topo);


.. seealso::
   :ref:`PMIx_Topology_destruct(3) <man3-PMIx_Topology_destruct>`,
   :ref:`PMIx_Topology_create(3) <man3-PMIx_Topology_create>`,
   :ref:`PMIx_Topology_free(3) <man3-PMIx_Topology_free>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
