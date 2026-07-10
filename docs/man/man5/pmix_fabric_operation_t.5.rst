.. _man5-pmix_fabric_operation_t:

pmix_fabric_operation_t
=======================

.. include_body

`pmix_fabric_operation_t` |mdash| Identifies a PMIx fabric operation

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef enum {
       PMIX_FABRIC_REQUEST_INFO,
       PMIX_FABRIC_UPDATE_INFO
   } pmix_fabric_operation_t;

Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   op = PMIX_FABRIC_REQUEST_INFO

where ``op`` is one of the ``pmix_fabric_operation_t`` enumerated values.


DESCRIPTION
-----------

The `pmix_fabric_operation_t` enumeration identifies the fabric operation being
requested of a host environment on behalf of a tool or other process. The
defined values are:

* ``PMIX_FABRIC_REQUEST_INFO`` (value 0) |mdash| request information on a
  specific fabric. If the fabric is not specified (as per
  :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`), then
  information on the default fabric of the overall system is returned. The
  information to be returned is described in
  :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`.

* ``PMIX_FABRIC_UPDATE_INFO`` (value 1) |mdash| update information on a
  specific fabric. The index of the fabric to be updated
  (the ``PMIX_FABRIC_INDEX`` attribute) must be provided.


.. seealso::
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`,
   :ref:`PMIx_Fabric_deregister(3) <man3-PMIx_Fabric_deregister>`,
   :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`
