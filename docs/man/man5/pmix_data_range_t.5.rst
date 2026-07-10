.. _man5-pmix_data_range_t:

pmix_data_range_t
=================

.. include_body

`pmix_data_range_t` |mdash| Defines the range of processes to which published
data or a generated event is to be made available

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_data_range_t;

   #define PMIX_RANGE_UNDEF        0
   #define PMIX_RANGE_RM           1   // data is intended for the host resource manager
   #define PMIX_RANGE_LOCAL        2   // available on local node only
   #define PMIX_RANGE_NAMESPACE    3   // data is available to procs in the same nspace only
   #define PMIX_RANGE_SESSION      4   // data available to all procs in session
   #define PMIX_RANGE_GLOBAL       5   // data available to all procs
   #define PMIX_RANGE_CUSTOM       6   // range is specified in a pmix_info_t
   #define PMIX_RANGE_PROC_LOCAL   7   // restrict range to the local proc
   #define PMIX_RANGE_INVALID   UINT8_MAX


Python Syntax
^^^^^^^^^^^^^

None


DESCRIPTION
-----------

The `pmix_data_range_t` type is used to specify the range of processes that
are to have access to information |mdash| for example, data published via
:ref:`PMIx_Publish(3) <man3-PMIx_Publish>` and retrieved via
:ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`, or the recipients of an event
generated via :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`. Defined
values include:

.. list-table:: Data Range Values
   :align: center
   :header-rows: 1

   * - **NAME**
     - **VALUE**
     - **DESCRIPTION**
   * - `PMIX_RANGE_UNDEF`
     - 0
     - No range has been defined.
   * - `PMIX_RANGE_RM`
     - 1
     - The data is intended for the host resource manager only.
   * - `PMIX_RANGE_LOCAL`
     - 2
     - The data is available to processes on the local node only.
   * - `PMIX_RANGE_NAMESPACE`
     - 3
     - The data is available only to processes in the same namespace (job) as the publisher.
   * - `PMIX_RANGE_SESSION`
     - 4
     - The data is available to all processes in the same session as the publisher.
   * - `PMIX_RANGE_GLOBAL`
     - 5
     - The data is available to all processes.
   * - `PMIX_RANGE_CUSTOM`
     - 6
     - The range is explicitly specified in an accompanying :ref:`pmix_info_t <man5-pmix_info_t>` structure.
   * - `PMIX_RANGE_PROC_LOCAL`
     - 7
     - The range is restricted to the local process only.
   * - `PMIX_RANGE_INVALID`
     - UINT8_MAX
     - An invalid range value, used to indicate that no valid range has been set.


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. seealso::
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>`,
   :ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
