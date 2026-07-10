.. _man5-pmix_proc_state_t:

pmix_proc_state_t
=================

.. include_body

`pmix_proc_state_t` |mdash| Defines the execution state of a process

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_proc_state_t;

   #define PMIX_PROC_STATE_UNDEF                    0  /* undefined process state */
   #define PMIX_PROC_STATE_PREPPED                  1  /* process is ready to be launched */
   #define PMIX_PROC_STATE_LAUNCH_UNDERWAY          2  /* launch process underway */
   #define PMIX_PROC_STATE_RESTART                  3  /* the proc is ready for restart */
   #define PMIX_PROC_STATE_TERMINATE                4  /* process is marked for termination */
   #define PMIX_PROC_STATE_RUNNING                  5  /* daemon has locally fork'd process */
   #define PMIX_PROC_STATE_CONNECTED                6  /* proc connected to PMIx server */
   /*
   * Define a "boundary" so users can easily and quickly determine
   * if a proc is still running or not - any value less than
   * this one means that the proc has not terminated
   */
   #define PMIX_PROC_STATE_UNTERMINATED            15

   #define PMIX_PROC_STATE_TERMINATED              20  /* process has terminated and is no longer running */
   /* Define a boundary so users can easily and quickly determine
   * if a proc abnormally terminated - leave a little room
   * for future expansion
   */
   #define PMIX_PROC_STATE_ERROR                   50
   /* Define specific error code values */
   #define PMIX_PROC_STATE_KILLED_BY_CMD           (PMIX_PROC_STATE_ERROR +  1)  /* process was killed by cmd */
   #define PMIX_PROC_STATE_ABORTED                 (PMIX_PROC_STATE_ERROR +  2)  /* process aborted */
   #define PMIX_PROC_STATE_FAILED_TO_START         (PMIX_PROC_STATE_ERROR +  3)  /* process failed to start */
   #define PMIX_PROC_STATE_ABORTED_BY_SIG          (PMIX_PROC_STATE_ERROR +  4)  /* process aborted by signal */
   #define PMIX_PROC_STATE_TERM_WO_SYNC            (PMIX_PROC_STATE_ERROR +  5)  /* process exit'd w/o calling PMIx_Finalize */
   #define PMIX_PROC_STATE_COMM_FAILED             (PMIX_PROC_STATE_ERROR +  6)  /* process communication has failed */
   #define PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED   (PMIX_PROC_STATE_ERROR +  7)  /* process exceeded a sensor limit */
   #define PMIX_PROC_STATE_CALLED_ABORT            (PMIX_PROC_STATE_ERROR +  8)  /* process called "PMIx_Abort" */
   #define PMIX_PROC_STATE_HEARTBEAT_FAILED        (PMIX_PROC_STATE_ERROR +  9)  /* process failed to send heartbeat w/in time limit */
   #define PMIX_PROC_STATE_MIGRATING               (PMIX_PROC_STATE_ERROR + 10)  /* process failed and is waiting for resources before restarting */
   #define PMIX_PROC_STATE_CANNOT_RESTART          (PMIX_PROC_STATE_ERROR + 11)  /* process failed and cannot be restarted */
   #define PMIX_PROC_STATE_TERM_NON_ZERO           (PMIX_PROC_STATE_ERROR + 12)  /* process exited with a non-zero status, indicating abnormal */
   #define PMIX_PROC_STATE_FAILED_TO_LAUNCH        (PMIX_PROC_STATE_ERROR + 13)  /* unable to launch process */


Python Syntax
^^^^^^^^^^^^^

None


DESCRIPTION
-----------

The `pmix_proc_state_t` type describes the execution state of a process, as
reported (for example) via the ``PMIX_PROC_STATE_STATUS`` attribute. Two
sentinel "boundary" values are defined to make range comparisons easy:
any value less than `PMIX_PROC_STATE_UNTERMINATED` indicates a process that has
not yet terminated, and any value at or above `PMIX_PROC_STATE_ERROR` indicates
a process that terminated abnormally. Defined values include:

.. list-table:: Process State Values
   :align: center
   :header-rows: 1

   * - **NAME**
     - **VALUE**
     - **DESCRIPTION**
   * - `PMIX_PROC_STATE_UNDEF`
     - 0
     - Undefined process state.
   * - `PMIX_PROC_STATE_PREPPED`
     - 1
     - The process is ready to be launched.
   * - `PMIX_PROC_STATE_LAUNCH_UNDERWAY`
     - 2
     - Launch of the process is underway.
   * - `PMIX_PROC_STATE_RESTART`
     - 3
     - The process is ready for restart.
   * - `PMIX_PROC_STATE_TERMINATE`
     - 4
     - The process is marked for termination.
   * - `PMIX_PROC_STATE_RUNNING`
     - 5
     - The daemon has locally fork'd the process.
   * - `PMIX_PROC_STATE_CONNECTED`
     - 6
     - The process has connected to its PMIx server.
   * - `PMIX_PROC_STATE_UNTERMINATED`
     - 15
     - Boundary marker: any state value less than this indicates the process has not terminated.
   * - `PMIX_PROC_STATE_TERMINATED`
     - 20
     - The process has terminated normally and is no longer running.
   * - `PMIX_PROC_STATE_ERROR`
     - 50
     - Boundary marker: any state value at or above this indicates the process terminated abnormally.
   * - `PMIX_PROC_STATE_KILLED_BY_CMD`
     - PMIX_PROC_STATE_ERROR + 1 (51)
     - The process was killed by command.
   * - `PMIX_PROC_STATE_ABORTED`
     - PMIX_PROC_STATE_ERROR + 2 (52)
     - The process aborted.
   * - `PMIX_PROC_STATE_FAILED_TO_START`
     - PMIX_PROC_STATE_ERROR + 3 (53)
     - The process failed to start.
   * - `PMIX_PROC_STATE_ABORTED_BY_SIG`
     - PMIX_PROC_STATE_ERROR + 4 (54)
     - The process was aborted by a signal.
   * - `PMIX_PROC_STATE_TERM_WO_SYNC`
     - PMIX_PROC_STATE_ERROR + 5 (55)
     - The process exited without calling :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`.
   * - `PMIX_PROC_STATE_COMM_FAILED`
     - PMIX_PROC_STATE_ERROR + 6 (56)
     - Communication with the process has failed.
   * - `PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED`
     - PMIX_PROC_STATE_ERROR + 7 (57)
     - The process exceeded a sensor limit.
   * - `PMIX_PROC_STATE_CALLED_ABORT`
     - PMIX_PROC_STATE_ERROR + 8 (58)
     - The process called :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`.
   * - `PMIX_PROC_STATE_HEARTBEAT_FAILED`
     - PMIX_PROC_STATE_ERROR + 9 (59)
     - The process failed to send a heartbeat within the time limit.
   * - `PMIX_PROC_STATE_MIGRATING`
     - PMIX_PROC_STATE_ERROR + 10 (60)
     - The process failed and is waiting for resources before restarting.
   * - `PMIX_PROC_STATE_CANNOT_RESTART`
     - PMIX_PROC_STATE_ERROR + 11 (61)
     - The process failed and cannot be restarted.
   * - `PMIX_PROC_STATE_TERM_NON_ZERO`
     - PMIX_PROC_STATE_ERROR + 12 (62)
     - The process exited with a non-zero status, indicating abnormal termination.
   * - `PMIX_PROC_STATE_FAILED_TO_LAUNCH`
     - PMIX_PROC_STATE_ERROR + 13 (63)
     - The process was unable to be launched.


ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. seealso::
   :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`,
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
