.. _man5-pmix_fabric_t:

pmix_fabric_t
=============

.. include_body

`pmix_fabric_t` |mdash| Describes a fabric for use with PMIx fabric
interfaces

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_fabric_s {
       char *name;
       size_t index;
       pmix_info_t *info;
       size_t ninfo;
       void *module;
   } pmix_fabric_t;


DESCRIPTION
-----------

The `pmix_fabric_t` structure is used by a Workload Manager (WLM) to
interact with fabric-related PMIx interfaces, and to provide information
about the fabric for use in scheduling algorithms or other purposes.

The fields of the structure are:

* ``name`` |mdash| an optional user-supplied string name identifying the
  fabric referenced by this structure. If provided, the field must be a
  ``NULL``-terminated string composed of standard alphanumeric values
  supported by common utilities such as ``strcmp``.
* ``index`` |mdash| a PMIx-provided number identifying this registration
  object.
* ``info`` |mdash| an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures containing information (provided by the PMIx library) about
  the fabric.
* ``ninfo`` |mdash| the number of elements in the ``info`` array.
* ``module`` |mdash| an opaque object pointer reserved for use by the
  PMIx server library.

Only the ``name`` field is provided by the user |mdash| all other fields
are provided by the PMIx library and must not be modified by the user. The
``info`` array contains a varying amount of information depending upon both
the PMIx implementation and the information available from the fabric
vendor.

For performance reasons, the PMIx library does not provide thread
protection for accessing the information in the `pmix_fabric_t` structure.
Callers must therefore coordinate their access to the structure with any
updates driven by the PMIx library.


.. seealso::
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`,
   :ref:`PMIx_Fabric_deregister(3) <man3-PMIx_Fabric_deregister>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>`
