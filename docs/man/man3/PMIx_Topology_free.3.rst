.. _man3-PMIx_Topology_free:

PMIx_Topology_free
==================

.. include_body

``PMIx_Topology_free`` |mdash| Release an array of
:ref:`pmix_topology_t(5) <man5-pmix_topology_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Topology_free(pmix_topology_t *t, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``t``: Pointer to an array of :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
  structures previously returned by :ref:`PMIx_Topology_create(3)
  <man3-PMIx_Topology_create>`.
* ``n``: Number of structures in the array |mdash| must be the same count that
  was passed to :ref:`PMIx_Topology_create(3) <man3-PMIx_Topology_create>`.


DESCRIPTION
-----------

Release an array of :ref:`pmix_topology_t <man5-pmix_topology_t>` structures
that was allocated by :ref:`PMIx_Topology_create(3)
<man3-PMIx_Topology_create>`. The function destructs the contents of each of
the ``n`` elements |mdash| releasing any embedded ``hwloc`` topology object and
``source`` string as if by :ref:`PMIx_Topology_destruct(3)
<man3-PMIx_Topology_destruct>` |mdash| and then frees the array storage itself.

If ``t`` is ``NULL`` the function returns without action.


RETURN VALUE
------------

``PMIx_Topology_free`` returns no value (``void``).


NOTES
-----

``PMIx_Topology_free`` is the counterpart to :ref:`PMIx_Topology_create(3)
<man3-PMIx_Topology_create>`. Do not call it on a single structure that was
merely constructed with :ref:`PMIx_Topology_construct(3)
<man3-PMIx_Topology_construct>`; use :ref:`PMIx_Topology_destruct(3)
<man3-PMIx_Topology_destruct>` for that case, since the structure storage was
not allocated by the library.


.. seealso::
   :ref:`PMIx_Topology_create(3) <man3-PMIx_Topology_create>`,
   :ref:`PMIx_Topology_construct(3) <man3-PMIx_Topology_construct>`,
   :ref:`PMIx_Topology_destruct(3) <man3-PMIx_Topology_destruct>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
