.. _man1-pctrl:

pctrl
=====

.. include_body

pctrl |mdash| Request job control operations on running processes

SYNOPSIS
--------

``pctrl [options]``


DESCRIPTION
-----------

``pctrl`` is the PMIx job control tool. It allows a user to request
job control operations |mdash| such as pausing, resuming, signaling,
terminating, checkpointing, or restarting |mdash| against a specified
set of running processes. The tool connects to a PMIx server and relays
the request to the host environment for processing, returning a status
indicating whether or not the requested operation succeeded.

The processes to be acted upon are identified with the ``--targets``
option. The specific control operation is selected by the corresponding
option (e.g., ``--pause``, ``--kill``, ``--checkpoint``).


OPTIONS
-------

``pctrl`` accepts the following options:

* ``-h`` | ``--help <arg0>``: Show help message. If the optional
  argument is not provided, then a generalized help message similar
  to the information provided here is returned. If an argument is
  provided, then a more detailed help message for that specific
  command line option is returned.

* ``-v`` | ``--verbose``: Enable debug output.

* ``-V`` | ``--version``: Print version and exit.

* ``--pmixmca <arg0> <arg1>``: Set the value of a PMIx MCA parameter,
  where ``<arg0>`` is the parameter name (key) and ``<arg1>`` is its
  value.

* ``--uri <arg0>``: Specify the URI of the server to which we are to connect, or
  the name of the file (specified as ``file:filename``) that contains that info

* ``--namespace <arg0>``: Namespace of the daemon to which we should connect

* ``--nspace <arg0>``: Synonym for ``namespace``

* ``--pid <arg0>``: PID of the daemon to which we should connect (``int`` => ``PID``
  or ``file:<file>`` for file containing the PID

* ``--system-server-first``: First look for a system server and connect to it if found

* ``--system-server-only``: Connect only to a system-level server

* ``--tmpdir <arg0>``: Set the root for the session directory tree

* ``--wait-to-connect <arg0>``: Delay specified number of seconds before trying to connect

* ``--num-connect-retries <arg0>``: Max number of times to try to connect

* ``--request-id <arg0>``: String identifier for this job control request. The
  request ID can be used for subsequent query of request status and/or
  cancellation of the request, and must be unique among currently active
  requests.

* ``--pause``: Pause the specified processes (typically by applying ``SIGSTOP``).

* ``--resume``: "Un-pause" the specified processes, resuming execution
  (typically by applying ``SIGCONT``).

* ``--cancel <arg0>``: Cancel the specified request ID (``all`` => cancel all
  requests from this requestor).

* ``--kill``: Force terminate the specified processes. Typically delivers the
  sequence ``SIGCONT``, ``SIGTERM``, ``SIGKILL``.

* ``--terminate``: Politely terminate the specified processes. Typically delivers
  the sequence ``SIGCONT``, ``SIGTERM``.

* ``--signal <arg0>``: Deliver the given signal to the specified processes. The
  signal is provided by name (e.g., ``SIGTERM``, ``SIGKILL``) or as an integer
  value (e.g., ``-9``).

* ``--restart <arg0>``: Restart the specified processes using the given
  checkpoint ID.

* ``--checkpoint <arg0>``: Checkpoint the specified processes and assign the
  given ID to it. The checkpoint is conducted according to the method specified
  when the processes were originally spawned.

* ``--pset <arg0>``: Define a new process set (with the given name) whose
  membership is comprised of the specified processes.

* ``--targets <arg0>``: Comma-delimited list of target processes for the
  requested job-control operation. Each process is identified as ``nspace:rank``;
  a wildcard rank (to apply the request to all processes in the namespace) can be
  indicated with an asterisk (``*``).


EXIT STATUS
-----------

Returns 0 for success, non-zero error code if a problem occurred. Note that
success does not necessarily translate to success of the requested operation,
depending upon the provided options, but may instead mean that the request has
been accepted for processing. ``pctrl`` returns a status indicating whether or
not the requested job-control operation succeeded.


EXAMPLES
--------

Pause every process in the namespace ``myjob``::

   pctrl --targets myjob:* --pause

Forcibly terminate ranks 0 and 3 of ``myjob``::

   pctrl --targets myjob:0,myjob:3 --kill


.. seealso::
   :ref:`PMIx_Job_control(3) <man3-PMIx_Job_control>`,
   :ref:`openpmix(5) <man5-openpmix>`
