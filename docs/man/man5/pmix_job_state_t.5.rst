.. _man5-pmix_job_state_t:

pmix_job_state_t
================

.. include_body

`pmix_job_state_t` |mdash| Describes the execution state of a job

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_job_state_t;

   #define PMIX_JOB_STATE_UNDEF                     0
   #define PMIX_JOB_STATE_AWAITING_ALLOC            1
   #define PMIX_JOB_STATE_LAUNCH_UNDERWAY           2
   #define PMIX_JOB_STATE_RUNNING                   3
   #define PMIX_JOB_STATE_SUSPENDED                 4
   #define PMIX_JOB_STATE_CONNECTED                 5

   #define PMIX_JOB_STATE_UNTERMINATED             15

   #define PMIX_JOB_STATE_TERMINATED               20

   #define PMIX_JOB_STATE_TERMINATED_WITH_ERROR    50


DESCRIPTION
-----------

The `pmix_job_state_t` type is an 8-bit unsigned integer describing the
execution state of a job. It takes one of the following values:

``PMIX_JOB_STATE_UNDEF`` (0)
   Undefined job state.

``PMIX_JOB_STATE_AWAITING_ALLOC`` (1)
   The job is waiting for resources to be allocated to it.

``PMIX_JOB_STATE_LAUNCH_UNDERWAY`` (2)
   Job launch is underway.

``PMIX_JOB_STATE_RUNNING`` (3)
   All processes have been spawned.

``PMIX_JOB_STATE_SUSPENDED`` (4)
   The job has been suspended.

``PMIX_JOB_STATE_CONNECTED`` (5)
   All processes have connected to their PMIx server.

``PMIX_JOB_STATE_UNTERMINATED`` (15)
   A boundary value so users can easily and quickly determine whether a job is
   still running |mdash| any value less than this one means the job has not
   terminated.

``PMIX_JOB_STATE_TERMINATED`` (20)
   The job has terminated and is no longer running |mdash| typically
   accompanied by the job exit status in response to a query.

``PMIX_JOB_STATE_TERMINATED_WITH_ERROR`` (50)
   The job has terminated and is no longer running |mdash| typically
   accompanied by a job-related error code in response to a query. This value
   also serves as a boundary so users can easily and quickly determine whether
   a job abnormally terminated.

The :ref:`PMIx_Job_state_string(3) <man3-PMIx_Job_state_string>` function
returns a string representation of a `pmix_job_state_t` value.


.. seealso::
   :ref:`PMIx_Job_state_string(3) <man3-PMIx_Job_state_string>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
