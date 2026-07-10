.. _man3-PMIx_Topology_destruct:

PMIx_Topology_destruct
======================

.. include_body

``PMIx_Topology_destruct`` |mdash| Release the contents of a
:ref:`pmix_topology_t(5) <man5-pmix_topology_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Topology_destruct(pmix_topology_t *topo);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``topo``: Pointer to the :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release the internal payload held within a :ref:`pmix_topology_t
<man5-pmix_topology_t>` structure. When the structure describes an ``hwloc``
topology (its ``source`` string begins with ``"hwloc"``), the function destroys
the embedded ``hwloc`` topology object, frees the ``source`` string, and sets
both pointers to ``NULL``. Structures with any other ``source`` (or none) are
left untouched, since their ``topology`` payload is not owned by the library.

``PMIx_Topology_destruct`` releases only the *contents* of the structure; it
does **not** free the storage of the ``pmix_topology_t`` structure itself. When
the structure was allocated on the stack or embedded in another object, that
storage is reclaimed by its owner.


RETURN VALUE
------------

``PMIx_Topology_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Topology_destruct`` is the counterpart to :ref:`PMIx_Topology_construct(3)
<man3-PMIx_Topology_construct>`. To release an array of structures that was
allocated by :ref:`PMIx_Topology_create(3) <man3-PMIx_Topology_create>`,
including the array storage itself, use :ref:`PMIx_Topology_free(3)
<man3-PMIx_Topology_free>` instead.


.. seealso::
   :ref:`PMIx_Topology_construct(3) <man3-PMIx_Topology_construct>`,
   :ref:`PMIx_Topology_create(3) <man3-PMIx_Topology_create>`,
   :ref:`PMIx_Topology_free(3) <man3-PMIx_Topology_free>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
