.. _man3-PMIx_Spawn:

PMIx_Spawn
==========

.. include_body

``PMIx_Spawn``, ``PMIx_Spawn_nb`` |mdash| Spawn a new job consisting of one or
more applications.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Spawn(const pmix_info_t job_info[], size_t ninfo,
                            const pmix_app_t apps[], size_t napps,
                            pmix_nspace_t nspace);

   pmix_status_t PMIx_Spawn_nb(const pmix_info_t job_info[], size_t ninfo,
                               const pmix_app_t apps[], size_t napps,
                               pmix_spawn_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # jobInfo is a list of Python ``pmix_info_t`` dictionaries
  jobInfo = [{'key': PMIX_NOTIFY_COMPLETION,
              'value': {'value': True, 'val_type': PMIX_BOOL}}]
  # pyapps is a list of Python ``pmix_app_t`` dictionaries; each app
  # provides at least a command and its argv, plus optional per-app info
  pyapps = [{'cmd': "hello", 'argv': ["hello", "world"],
             'maxprocs': 4, 'info': []}]
  rc, nspace = foo.spawn(jobInfo, pyapps)


INPUT PARAMETERS
----------------

* ``job_info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying job-level directives that apply to the spawned job as a
  whole (see `DIRECTIVES`_). A ``NULL`` value is supported when no job-level
  directives are desired.
* ``ninfo``: Number of elements in the ``job_info`` array.
* ``apps``: Pointer to an array of ``pmix_app_t`` structures, one per application
  to be launched. Each ``pmix_app_t`` describes a single application via the
  following fields:

  * ``cmd`` (char\*) |mdash| the executable to launch.
  * ``argv`` (char\*\*) |mdash| the ``NULL``-terminated argument vector.
  * ``env`` (char\*\*) |mdash| a ``NULL``-terminated array of environment
    variables (in ``name=value`` form) to be set for this application.
  * ``cwd`` (char\*) |mdash| the working directory in which to start the
    application's processes.
  * ``maxprocs`` (int) |mdash| the number of processes to start for this
    application.
  * ``info`` / ``ninfo`` |mdash| an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
    structures conveying per-application directives (see `DIRECTIVES`_), and the
    number of elements in it.

  At least one of ``cmd`` or ``argv`` must be provided in every element; an
  element that supplies neither causes the operation to fail with
  ``PMIX_ERR_BAD_PARAM``.
* ``napps``: Number of elements in the ``apps`` array.

The non-blocking form replaces the ``nspace`` output parameter with a callback:

* ``cbfunc``: Callback function of type :ref:`pmix_spawn_cbfunc_t <man5-pmix_spawn_cbfunc_t>` invoked when the
  applications have been launched (or the launch has failed).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


OUTPUT PARAMETERS
-----------------

* ``nspace`` (blocking form): A ``pmix_nspace_t`` (a fixed-size character array of
  at least ``PMIX_MAX_NSLEN + 1`` bytes) into which the namespace assigned to the
  newly spawned job is copied on success. Because ``pmix_nspace_t`` is an array,
  this parameter cannot itself be ``NULL``.

For the non-blocking form, the assigned namespace is instead delivered as the
``nspace`` argument to ``cbfunc``. That value is not protected after the callback
returns, so the receiver must copy it if it needs to be retained.


DESCRIPTION
-----------

Spawn a new job consisting of the applications described in the ``apps`` array.
``PMIx_Spawn`` is the blocking form: it does not return until the job has been
launched (or the operation fails), at which point the namespace assigned to the
new job is returned in ``nspace``. ``PMIx_Spawn_nb`` is the non-blocking form: it
returns immediately, and the provided ``cbfunc`` is invoked with the final status
and the assigned namespace once the launch completes.

Each element of the ``apps`` array describes one application (multiple elements
therefore describe an MPMD job launched as a single unit). Directives that apply
to the job as a whole are passed in ``job_info``; directives that apply to only a
single application are passed in that application's ``info`` array. Where an
attribute is meaningful at both levels, a value in an application's ``info`` array
applies to that application only, while a value in ``job_info`` applies to every
application in the job.

By default, the spawned processes will be PMIx "connected" to the parent process
upon successful launch (see :ref:`PMIx_Connect(3) <man3-PMIx_Connect>` for
details). In practice this means the parent process is given a copy of the new
job's job-level information |mdash| so it can query that information without
incurring communication penalties |mdash| and that both the parent and the members
of the child job receive notification of errors arising anywhere in their combined
assemblage.

Behavior of individual resource managers may differ, but it is expected that
failure of any application process to start will result in termination and cleanup
of *all* processes in the newly spawned job and return of an error code to the
caller.

As with all non-blocking PMIx APIs, callers of ``PMIx_Spawn_nb`` **must** keep the
``job_info`` and ``apps`` arrays (and everything they reference) valid until
``cbfunc`` is invoked.


DIRECTIVES
----------

PMIx libraries are not required to directly support any attributes for this
operation; any provided attributes are passed to the host environment for
processing. Unless otherwise noted, each attribute below may be placed either in
the ``job_info`` array (to apply to the whole job) or in the ``info`` array of an
individual ``pmix_app_t`` (to apply to that application only).

Host environments are **required** to support the following attributes when
present:

* ``PMIX_WDIR`` (char\*) |mdash| working directory in which to start the spawned
  processes.
* ``PMIX_SET_SESSION_CWD`` (bool) |mdash| set the application's current working
  directory to the session working directory assigned by the RM.
* ``PMIX_PREFIX`` (char\*) |mdash| prefix directory to use when looking for the
  application's executables and libraries.
* ``PMIX_HOST`` (char\*) |mdash| comma-delimited list of hosts to use for the
  spawned processes.
* ``PMIX_HOSTFILE`` (char\*) |mdash| hostfile naming the resources to use for the
  spawned processes.

The following attributes are **optional** for host environments that support this
operation. Placement, mapping, and resource selection:

* ``PMIX_ADD_HOST`` (char\*) / ``PMIX_ADD_HOSTFILE`` (char\*) |mdash| add the named
  hosts, or the hosts named in the given hostfile, to the existing allocation.
* ``PMIX_PRELOAD_BIN`` (bool) |mdash| preload the application binaries onto the
  target nodes.
* ``PMIX_PRELOAD_FILES`` (char\*) |mdash| comma-delimited list of files to
  pre-position on the target nodes.
* ``PMIX_PERSONALITY`` (char\*) |mdash| name of the programming-model personality
  to assume for the application (e.g., ``"ompi"``).
* ``PMIX_MAPBY`` (char\*) / ``PMIX_RANKBY`` (char\*) / ``PMIX_BINDTO`` (char\*)
  |mdash| mapping, ranking, and binding policies for the spawned processes.
* ``PMIX_PPR`` (char\*) |mdash| number of processes to spawn on each identified
  resource.
* ``PMIX_CPUS_PER_PROC`` (uint32_t) |mdash| number of CPUs to assign to each rank.
* ``PMIX_CPU_LIST`` (char\*) |mdash| list of CPUs to use for this job.
* ``PMIX_NO_PROCS_ON_HEAD`` (bool) |mdash| do not place any processes on the head
  node.
* ``PMIX_NO_OVERSUBSCRIBE`` (bool) |mdash| do not oversubscribe the CPUs.
* ``PMIX_COLOCATE_PROCS`` (pmix_data_array_t\*) |mdash| array of ``pmix_proc_t``
  identifying the processes with which the new job's processes are to be
  colocated.
* ``PMIX_COLOCATE_NPERPROC`` (uint16_t) / ``PMIX_COLOCATE_NPERNODE`` (uint16_t)
  |mdash| number of processes to colocate with each identified process, or on the
  node of each identified process, respectively.
* ``PMIX_SPAWN_TARGET`` (varies) |mdash| specify the allocation(s) to use when
  mapping the applications. The value is a ``char*`` ``PMIX_ALLOC_ID``, or a
  ``pmix_data_array_t`` of such allocation-ID strings.
* ``PMIX_APP_ARGV`` (char\*) |mdash| the consolidated ``argv`` passed to the spawn
  command for the given application.
* ``PMIX_INDEX_ARGV`` (bool) |mdash| mark each process's ``argv`` with the rank of
  the process.
* ``PMIX_WDIR_USER_SPECIFIED`` (bool) |mdash| indicate that the working directory
  was explicitly specified by the user (accompanies ``PMIX_WDIR``).
* ``PMIX_DISPLAY_MAP`` (bool) |mdash| display the resulting process placement map
  upon spawn.
* ``PMIX_DISPLAY_MAP_DETAILED`` (bool) |mdash| display a highly detailed placement
  map upon spawn.
* ``PMIX_STDIN_TGT`` (pmix_proc_t\*) |mdash| the process that is to receive
  forwarded stdin.

Diagnostic and dry-run output:

* ``PMIX_DISPLAY_ALLOCATION`` (bool) |mdash| display the resource allocation.
* ``PMIX_DISPLAY_TOPOLOGY`` (char\*) |mdash| comma-delimited list of hosts whose
  topology is to be displayed.
* ``PMIX_DISPLAY_PROCESSORS`` (char\*) |mdash| comma-delimited list of hosts whose
  available CPUs are to be displayed.
* ``PMIX_DISPLAY_PARSEABLE_OUTPUT`` (bool) |mdash| render the requested display
  information in a format more amenable to machine parsing.
* ``PMIX_REPORT_BINDINGS`` (bool) |mdash| report the binding of each individual
  process.
* ``PMIX_REPORT_PHYSICAL_CPUS`` (bool) |mdash| report bindings using physical CPU
  IDs.
* ``PMIX_SHOW_LAUNCH_PROGRESS`` (bool) |mdash| provide periodic progress reports on
  the job launch procedure (e.g., after every 100 processes have been spawned).
* ``PMIX_DO_NOT_LAUNCH`` (bool) |mdash| execute all procedures to prepare the
  requested job for launch, but do not launch it. Typically combined with
  ``PMIX_DISPLAY_MAP`` or ``PMIX_DISPLAY_MAP_DETAILED`` for debugging.
* ``PMIX_AGGREGATE_HELP`` (bool) |mdash| aggregate help messages, reporting each
  unique message once accompanied by the number of processes that reported it.

Environment control:

* ``PMIX_SET_ENVAR`` / ``PMIX_ADD_ENVAR`` / ``PMIX_PREPEND_ENVAR`` /
  ``PMIX_APPEND_ENVAR`` (pmix_envar_t\*) |mdash| set, add, prepend to, or append to
  an environment variable in the spawned processes' environment.
* ``PMIX_ENVARS_HARVESTED`` (bool) |mdash| indicates that the requestor has already
  harvested and included the relevant environment variables.
* ``PMIX_FWD_ENVIRONMENT`` (bool) |mdash| forward the local environment to each
  spawned process.
* ``PMIX_SETUP_APP_ENVARS`` (bool) |mdash| direct the spawn library to harvest the
  environment variables relevant to the application's programming model and include
  them with the request. May be placed in the ``job_info`` array or in an individual
  application's ``info`` array. The full set of application-setup-data attributes is
  described under
  :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`.

Behavior, restart, and tool support:

* ``PMIX_JOB_RECOVERABLE`` (bool) |mdash| the application supports recoverable
  operations.
* ``PMIX_MAX_RESTARTS`` (uint32_t) |mdash| maximum number of times to restart a
  failed process.
* ``PMIX_COSPAWN_APP`` (bool) |mdash| the designated application is to be spawned
  as a disconnected job.
* ``PMIX_JOB_CONTINUOUS`` (bool) |mdash| the application is continuous; any failed
  processes should be immediately restarted.
* ``PMIX_ABORT_NON_ZERO_TERM`` (bool) |mdash| abort the spawned job if any process
  terminates with non-zero status.
* ``PMIX_SPAWN_CHILD_SEP`` (bool) |mdash| treat the spawned job as independent from
  the parent |mdash| i.e., do not terminate it if the parent terminates.
* ``PMIX_REPORT_CHILD_SEP`` (bool) |mdash| report the exit status of any child jobs
  separately. If ``false``, the reported status is zero when the primary and all
  child jobs exit normally, or the first non-zero status returned by any of them.
* ``PMIX_RUNTIME_OPTIONS`` (char\*) |mdash| environment-specific runtime directives
  that control job behavior.
* ``PMIX_FORKEXEC_AGENT`` (char\*) |mdash| command line of a fork/exec agent to be
  used for starting local processes.
* ``PMIX_SPAWN_TOOL`` (bool) |mdash| the job being spawned is a tool.
* ``PMIX_CMD_LINE`` (char\*) |mdash| the command line executing in the specified
  namespace. This is informational |mdash| e.g., recorded for, or retrieved from, a
  running job |mdash| rather than a launch directive.

Debugger support:

These attributes support co-launching an application under debugger control and
spawning debugger daemons alongside it.

* ``PMIX_DEBUG_STOP_ON_EXEC`` (varies) |mdash| stop the specified rank(s) upon
  ``exec`` and notify that they are ready to be debugged. The value may be a
  ``bool`` (``true`` for all ranks, ``false`` for none), a ``pmix_rank_t`` (a
  single rank, or ``PMIX_RANK_WILDCARD`` for all), or a ``pmix_data_array_t`` of
  individual processes.
* ``PMIX_DEBUG_STOP_IN_INIT`` (varies) |mdash| stop the specified rank(s) inside
  ``PMIx_Init`` and notify that they are ready to be debugged. Same value forms
  as ``PMIX_DEBUG_STOP_ON_EXEC``.
* ``PMIX_DEBUG_STOP_IN_APP`` (varies) |mdash| direct the specified rank(s) to
  stop at an application-defined point and notify that they are ready to be
  debugged. Same value forms as ``PMIX_DEBUG_STOP_ON_EXEC``.
* ``PMIX_DEBUG_TARGET`` (pmix_proc_t\*) |mdash| identifier of the process(es) to
  be debugged. A launcher spawning debugger daemons places this in the daemons'
  job-level information so the daemons can self-determine their specific targets.
* ``PMIX_DEBUG_DAEMONS_PER_PROC`` (uint16_t) |mdash| number of debugger daemons
  to spawn per application process.
* ``PMIX_DEBUG_DAEMONS_PER_NODE`` (uint16_t) |mdash| number of debugger daemons
  to spawn on each node where the target job is executing.
* ``PMIX_DEBUGGER_DAEMONS`` (bool) |mdash| the spawned application consists of
  debugger daemons.

When a process stopped by one of the ``PMIX_DEBUG_STOP_*`` directives is ready to
be debugged, it generates a ``PMIX_READY_FOR_DEBUG`` event accompanied by the
following OUT attribute (delivered to the debugger's event handler, not passed to
``PMIx_Spawn``):

* ``PMIX_BREAKPOINT`` (char\*) |mdash| string identifier of the breakpoint at
  which the process(es) are waiting.

Timeouts:

* ``PMIX_TIMEOUT`` (int) |mdash| time, in seconds, before the operation should be
  declared to have timed out (``0`` means infinite).
* ``PMIX_JOB_TIMEOUT`` (int) |mdash| time, in seconds, before the spawned job
  should time out.
* ``PMIX_SPAWN_TIMEOUT`` (int) |mdash| time, in seconds, before the spawn operation
  itself should time out.
* ``PMIX_TIMEOUT_STACKTRACES`` (bool) |mdash| include process stacktraces in the
  timeout report from a job.
* ``PMIX_TIMEOUT_REPORT_STATE`` (bool) |mdash| report process states in the timeout
  report from a job.

Completion and termination notification:

* ``PMIX_NOTIFY_COMPLETION`` (bool) |mdash| notify the parent process when the
  spawned job terminates, either normally or with an error.
* ``PMIX_NOTIFY_PROC_TERMINATION`` (bool) |mdash| request that the launcher
  generate a ``PMIX_EVENT_PROC_TERMINATED`` event whenever any process in the
  spawned job terminates, normally or abnormally.
* ``PMIX_NOTIFY_PROC_ABNORMAL_TERMINATION`` (bool) |mdash| request that the launcher
  generate a ``PMIX_EVENT_PROC_TERMINATED`` event only when a process terminates
  abnormally.
* ``PMIX_NOTIFY_JOB_EVENTS`` (bool) |mdash| request that the launcher generate the
  ``PMIX_EVENT_JOB_START``, ``PMIX_LAUNCH_COMPLETE``, and ``PMIX_EVENT_JOB_END``
  events, each including at least the job's namespace and a
  ``PMIX_EVENT_TIMESTAMP``.

Output forwarding:

These attributes control which of the spawned job's streams are forwarded and how
they are tagged, formatted, and directed. They supersede the older, now-deprecated
non-``IOF`` output attributes (see the note below).

Selecting which streams to forward:

* ``PMIX_FWD_STDIN`` (bool) |mdash| forward this process's ``stdin`` to the target
  processes.
* ``PMIX_FWD_STDOUT`` (bool) |mdash| forward the spawned processes' ``stdout`` to
  this process (typically used by a tool).
* ``PMIX_FWD_STDERR`` (bool) |mdash| forward the spawned processes' ``stderr`` to
  this process (typically used by a tool).
* ``PMIX_FWD_STDDIAG`` (bool) |mdash| if a diagnostic channel exists, forward its
  output from the spawned processes to this process (typically used by a tool).

Formatting and destination of the forwarded output:

* ``PMIX_IOF_TAG_OUTPUT`` (bool) |mdash| tag each line of output with the
  ``[local jobid,rank]`` of its source and the channel it came from.
* ``PMIX_IOF_TAG_DETAILED_OUTPUT`` (bool) |mdash| tag output with the
  ``[local jobid,rank][hostname:pid]`` and channel of its source.
* ``PMIX_IOF_TAG_FULLNAME_OUTPUT`` (bool) |mdash| tag output with the
  ``[nspace,rank]`` and channel of its source.
* ``PMIX_IOF_RANK_OUTPUT`` (bool) |mdash| tag output with the rank it came from.
* ``PMIX_IOF_TIMESTAMP_OUTPUT`` (bool) |mdash| timestamp each line of output.
* ``PMIX_IOF_MERGE_STDERR_STDOUT`` (bool) |mdash| merge the ``stderr`` stream
  into the ``stdout`` stream of the application processes.
* ``PMIX_IOF_XML_OUTPUT`` (bool) |mdash| format the forwarded output in XML.
* ``PMIX_IOF_OUTPUT_RAW`` (bool) |mdash| do not buffer output into complete
  lines; emit characters as the stream delivers them.
* ``PMIX_IOF_LOCAL_OUTPUT`` (bool) |mdash| write the output streams to the local
  ``stdout``/``stderr``.
* ``PMIX_IOF_OUTPUT_TO_FILE`` (char\*) |mdash| direct application output into
  files of the form ``<filename>.rank``, with both ``stdout`` and ``stderr``
  redirected into it.
* ``PMIX_IOF_OUTPUT_TO_DIRECTORY`` (char\*) |mdash| direct application output
  into files of the form ``<directory>/<jobid>/rank.<rank>/stdout[err]``.
* ``PMIX_IOF_FILE_PATTERN`` (bool) |mdash| treat the ``PMIX_IOF_OUTPUT_TO_FILE``
  value as a pattern, suppressing the automatic annotation by nspace, rank, or
  other parameters.
* ``PMIX_IOF_FILE_ONLY`` (bool) |mdash| output only into the designated files; do
  not also emit a copy to ``stdout``/``stderr``.

.. note::
   Several attributes historically used with ``PMIx_Spawn`` to control output
   forwarding |mdash| ``PMIX_TAG_OUTPUT``, ``PMIX_TIMESTAMP_OUTPUT``,
   ``PMIX_MERGE_STDERR_STDOUT``, ``PMIX_OUTPUT_TO_FILE``, and
   ``PMIX_OUTPUT_TO_DIRECTORY`` |mdash| are now deprecated in favor of the
   ``PMIX_IOF_*`` equivalents listed above under **Output forwarding**, as is
   ``PMIX_NON_PMI`` (indicating that the spawned processes will not call
   ``PMIx_Init``); none of these should be used in new code.

Pseudo-terminal:

* ``PMIX_SPAWN_PTY`` (bool) |mdash| spawn the processes under a pseudo-terminal.
* ``PMIX_PTY_TERMIO`` (pmix_byte_object_t) |mdash| the contents of a ``termio``
  structure describing the terminal settings for the pseudo-terminal.
* ``PMIX_PTY_WSIZE`` (pmix_byte_object_t) |mdash| the contents of a window-size
  structure for the pseudo-terminal.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the job was successfully
launched and its namespace has been returned in ``nspace``. For the non-blocking
form, a return of ``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status and assigned namespace are delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the job was successfully launched.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the spawn completed
  atomically; ``cbfunc`` will **not** be called. In this case ``PMIx_Spawn``
  returns the assigned namespace in ``nspace``.
* ``PMIX_ERR_JOB_ALLOC_FAILED`` |mdash| the job could not be executed due to
  failure to obtain the specified allocation.
* ``PMIX_ERR_JOB_APP_NOT_EXECUTABLE`` |mdash| the specified executable could not be
  found or lacks execution privileges.
* ``PMIX_ERR_JOB_NO_EXE_SPECIFIED`` |mdash| the request did not specify an
  executable.
* ``PMIX_ERR_JOB_FAILED_TO_MAP`` |mdash| the launcher was unable to map the
  processes for the request.
* ``PMIX_ERR_JOB_FAILED_TO_LAUNCH`` |mdash| one or more processes failed to launch.
* ``PMIX_ERR_JOB_EXE_NOT_FOUND`` |mdash| the specified executable was not found.
* ``PMIX_ERR_JOB_INSUFFICIENT_RESOURCES`` |mdash| insufficient resources to spawn
  the job.
* ``PMIX_ERR_JOB_SYS_OP_FAILED`` |mdash| a system library operation failed.
* ``PMIX_ERR_JOB_WDIR_NOT_FOUND`` |mdash| the specified working directory was not
  found.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, an ``apps`` element supplying neither a ``cmd`` nor an ``argv``.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the PMIx server has no host environment
  capable of servicing the spawn request.
* ``PMIX_ERR_UNREACH`` |mdash| the caller is a tool or client that is not connected
  to a PMIx server, and no local fork/exec fallback is available.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

A launcher process (such as an intermediate launcher started by a tool) that is
not connected to a PMIx server defaults to launching the applications via a local
fork/exec, allowing tools to maintain a single code path for both the connected
and disconnected cases. A tool or client that is not so privileged and cannot
reach a server receives ``PMIX_ERR_UNREACH``.

When ``PMIx_Spawn_nb`` returns ``PMIX_OPERATION_SUCCEEDED``, the spawn was
satisfied atomically and no callback will be made; callers must handle this status
distinctly from ``PMIX_SUCCESS``. The blocking ``PMIx_Spawn`` absorbs this case
internally and simply returns ``PMIX_SUCCESS`` with the namespace populated.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Connect(3) <man3-PMIx_Connect>`,
   :ref:`PMIx_Disconnect(3) <man3-PMIx_Disconnect>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`PMIx_Job_control(3) <man3-PMIx_Job_control>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
