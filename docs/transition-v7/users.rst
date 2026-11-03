Running Jobs with PMIx v7
=========================

For people who run jobs. If you install PMIx or administer the system,
see :doc:`admins`.

Most jobs run under PMIx v7 exactly as before: same commands, same
output, no need to rebuild. This page covers the cases where you may
notice a difference.


Containers and older applications
---------------------------------

**What changed.** An application carries the PMIx library it was
built with - a container image always does. A v7 server takes an
application whose PMIx is older than v7 only if your site has set it
up to. By default it does not.

**What you see.** When the server does not take it, each process of
the older application starts as a *singleton*. It prints one error
line, then runs alone: it does not know about the other processes in
its job. An MPI program, for example, runs as many independent
one-process jobs. Nothing else fails, so the job can look as if it is
working.

**What to do.**

* Ask your site whether its servers take applications of earlier
  PMIx releases. If they do not, ask for it, or
* update the PMIx inside the container to v7.

An application built with PMIx v7 runs under servers from PMIx v3.2
on, so a newer container on an older system is not a problem.


Running tools and launchers
---------------------------

**What changed.** A tool - ``prun``, ``pterm``, a debugger, a monitor
- can reach a PMIx server only on the node it runs on. A tool on a
login node cannot reach a DVM running on compute nodes.

**What to do.** Run the tool on the node where the server it talks to
runs. For a launcher or a debugger, ssh to the node where the DVM
controller (``prte`` or ``mpirun``) is running, and start the tool
there. This is also the approach the DDT debugger recommends.

**Coming.** Launching from a login node will remain possible. The
method is being worked out: most likely ``prun`` will send its launch
request through PRRTE's own messaging rather than through PMIx.
Debuggers are not settled yet.

.. DRAFT: update when the login-node method is decided.


Other users' jobs
-----------------

**What changed.** A job's data, output, monitoring, and control
belong to the user who owns it. Another user's tool is refused, and
sees nothing of the job.

**What to do.** If you want a colleague, a group, or a monitoring
account to see your job, say so when you start it. The launcher sets
the job's access list from what you give it; for PRRTE, see the
``--rtos`` option of ``prun``. Whether tools of other users may
connect at all is the host's choice, not yours - ask your site.


Output files
------------

**What changed.** Files and directories created by ``--output
file=...`` or ``--output dir=...`` are created as you, the job's
owner, and only you can read them unless the job's access list names
a group.

* If the DVM runs under a service account, ``prun`` writes the files,
  as you, on the node where ``prun`` runs.
* An existing file owned by someone else is not overwritten. The job
  runs, and PMIx reports once which file it could not open.


Temporary directory
-------------------

**Keep** ``$TMPDIR`` **short.** PMIx creates a local socket under it,
and a socket path is limited to about 100 characters. A server whose
path is too long fails to start. The default ``$TMPDIR`` on macOS is
too long: start the DVM or ``mpirun`` with a shorter one, for example
``TMPDIR=/tmp``.


Programs that exec or start helpers
-----------------------------------

**What changed.** Each process connects to its PMIx server once.

* A process that ``exec``\ s another program loses its connection.
  The new program is a new process to the server, not a member of the
  job.
* A helper process started *after* the parent called ``PMIx_Init``
  does not inherit the parent's identity, so it is not part of the job
  either.
* A helper started *before* ``PMIx_Init`` that calls ``PMIx_Init``
  itself, as the same rank, is refused: that rank already has its
  connection.

**What to do.** A wrapper script that sets things up and then
``exec``\ s the application is fine: the application is the first
program to call ``PMIx_Init``. If the wrapper itself uses PMIx, it
must not ``exec`` afterwards.


Settings that now fail
----------------------

**What changed.** PMIx no longer ignores a setting it cannot honor.
An MCA parameter naming a component that no longer exists, a user the
system does not know, or a relative path makes the process fail to
start, with a message saying which setting.

**What to do.** Check old parameter files and ``PMIX_MCA_*`` variables
in your scripts and shell start-up files. The removed and renamed
components are listed in :ref:`label-transition-v7-admin-settings`.
