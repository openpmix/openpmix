.. _man5-pmix_status_t:

pmix_status_t
=============

.. include_body

`pmix_status_t` |mdash| An `int` compatible value for return status codes.

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef int pmix_status_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   foo = PMIxStatus.status

where ``status`` is a PMIx-defined status code (e.g., ``PMIX_SUCCESS``)


DESCRIPTION
-----------

PMIx return values other than ``PMIX_SUCCESS`` are required to always be negative. The return status value for a
successful operation is ``PMIX_SUCCESS``, which must have an integer value of 0.


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. JMS COMMENT When more man pages are added, they can be :ref:'ed
   appropriately, so that HTML hyperlinks are created to link to the
   corresponding pages.

.. seealso::
   PMIx_Initialized(3),
   :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`,
   PMIx_Commit(3),
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   PMIx_Put(3),
   pmiAddInstance(3),
   pmiAddMetric(3)
