.. _man5-pmix_group_operation_t:

pmix_group_operation_t
======================

.. include_body

`pmix_group_operation_t` |mdash| Identifies a PMIx group operation

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef enum {
       PMIX_GROUP_CONSTRUCT,
       PMIX_GROUP_DESTRUCT,
       PMIX_GROUP_NONE,
       PMIX_GROUP_CANCEL
   } pmix_group_operation_t;

Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   op = PMIX_GROUP_CONSTRUCT

where ``op`` is one of the ``pmix_group_operation_t`` enumerated values.


DESCRIPTION
-----------

The `pmix_group_operation_t` enumeration identifies the group operation being
requested of a host environment. It is passed across the PMIx-to-host callback
boundary (the ``group`` entry of the server module) so the host can carry out
the corresponding action on behalf of the PMIx server library. The defined
values are:

* ``PMIX_GROUP_CONSTRUCT`` |mdash| construct a group composed of the specified
  processes. Used by a PMIx server library to direct the host operation
  underlying :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`.

* ``PMIX_GROUP_DESTRUCT`` |mdash| destruct the specified group. Used by a PMIx
  server library to direct the host operation underlying
  :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`.

* ``PMIX_GROUP_NONE`` |mdash| no group operation; used as a placeholder where
  an operation value is required but none applies.

* ``PMIX_GROUP_CANCEL`` |mdash| cancel a group operation that is in progress.

.. note::
   New values must be appended after the existing ones to preserve their
   numeric values, since this enumeration crosses the PMIx-to-host callback
   boundary. As a result the numeric ordering of the members is not
   alphabetical: ``PMIX_GROUP_CONSTRUCT`` is 0, ``PMIX_GROUP_DESTRUCT`` is 1,
   ``PMIX_GROUP_NONE`` is 2, and ``PMIX_GROUP_CANCEL`` is 3.

A human-readable string representation of a `pmix_group_operation_t` value can
be obtained from
:ref:`PMIx_Group_operation_string(3) <man3-PMIx_Group_operation_string>`.


.. seealso::
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`,
   :ref:`PMIx_Group_operation_string(3) <man3-PMIx_Group_operation_string>`
