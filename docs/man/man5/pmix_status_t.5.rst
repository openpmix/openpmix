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

`pmix_status_t` is the common return type for essentially every PMIx API, and
is also used to carry PMIx event and error codes. It is a simple ``int`` so
that status codes are directly comparable and can be stored in a
:ref:`pmix_value_t <man5-pmix_value_t>`.

PMIx return values other than ``PMIX_SUCCESS`` are required to always be
negative. The return status value for a successful operation is
``PMIX_SUCCESS``, which must have an integer value of 0.

The full set of status and event codes is large and is authoritatively defined
in ``pmix_common.h`` (with deprecated codes in ``pmix_deprecated.h``). Rather
than enumerate the hundreds of individual codes here, they can be grouped into
broad categories:

- **Success** |mdash| ``PMIX_SUCCESS`` (value 0).
- **General and operational errors** |mdash| e.g., ``PMIX_ERROR``,
  ``PMIX_ERR_BAD_PARAM``, ``PMIX_ERR_NOT_FOUND``, ``PMIX_ERR_NOT_SUPPORTED``,
  ``PMIX_ERR_OUT_OF_RESOURCE``.
- **Communication and packing errors** |mdash| e.g., ``PMIX_ERR_UNREACH``,
  ``PMIX_ERR_TIMEOUT``, ``PMIX_ERR_PACK_FAILURE``,
  ``PMIX_ERR_UNPACK_FAILURE``, ``PMIX_ERR_TYPE_MISMATCH``.
- **Fault-tolerance and process/job state events** |mdash| e.g.,
  ``PMIX_ERR_PROC_RESTART``, ``PMIX_ERR_PROC_MIGRATE``,
  ``PMIX_ERR_PROC_ABORTED``, ``PMIX_ERR_LOST_CONNECTION``.
- **Non-error events and notifications** |mdash| event codes delivered to
  handlers registered via
  :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
  including the reserved system-event range and model/monitoring events.

The string form of any status code can be obtained with
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`, and a string can be
converted back to its code with
:ref:`PMIx_Error_code(3) <man3-PMIx_Error_code>`.


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Initialized(3) <man3-PMIx_Initialized>`,
   :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`,
   :ref:`PMIx_Error_code(3) <man3-PMIx_Error_code>`
