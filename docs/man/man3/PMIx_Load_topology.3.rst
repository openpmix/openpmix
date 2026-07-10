.. _man3-PMIx_Load_topology:

PMIx_Load_topology
==================

.. include_body

``PMIx_Load_topology`` |mdash| Load the local hardware topology description.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Load_topology(pmix_topology_t *topo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  rc = foo.load_topology()


INPUT PARAMETERS
----------------

* ``topo``: Pointer to a ``pmix_topology_t`` structure into which the topology
  description of the local node is to be loaded. The structure must be
  initialized by the caller prior to the call (for example, with
  ``PMIx_Topology_construct()``). If a particular topology *source* is required
  (e.g., ``"hwloc"``), the ``source`` field of the structure must be set to that
  value before the call; otherwise the library selects an available source.


DESCRIPTION
-----------

Obtain the topology description of the local node and load it into the provided
``pmix_topology_t`` structure. This is a blocking operation that returns once the
topology has been loaded.

If the ``source`` field of the provided ``pmix_topology_t`` is set, the PMIx
library must return a description from the specified implementation or else
indicate that the implementation is not available by returning
``PMIX_ERR_NOT_SUPPORTED``.

The returned description should be treated as a read-only object; attempts to
modify it may result in errors. The PMIx library is responsible for performing
any required cleanup when the client library finalizes.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when a valid topology description has been loaded. On
error, a negative value corresponding to a PMIx error constant is returned,
including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the requested topology could not be found.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the requested source is not available in the
  current implementation.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

It is the responsibility of the caller to ensure that the ``topo`` argument is
properly initialized prior to calling this API. If no ``source`` was specified,
the caller should inspect the returned ``source`` field to verify that the
returned topology description is compatible with the caller's code.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
