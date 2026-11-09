.. _label-host-env-starting:

Starting the Server
===================

What a host passes to
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>` decides what the
server is for. Most behaviors a host may want need more than one
attribute, and some need a setting from the site or a build option as
well. This page lists, for each goal, everything that has to be in
place. A launcher or scheduler that comes up through
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` passes the same
attributes there.

For the combinations that do not do what they look as though they
should, and for the full tables, see :ref:`label-settings-matrix`.


Serve clients
-------------

Nothing to pass. Register each job and each local process
(``PMIx_server_register_nspace``, ``PMIx_server_register_client``) and
prepare each process's environment (``PMIx_server_setup_fork``) before
starting it. The environment carries the server's address; a client
needs nothing else to find it.

**A client of another user.** A server running as root for a job's user
names that user as ``PMIX_USERID``, which makes the user the owner of
the server's local socket. Otherwise only the server's own user can
connect.


Serve tools
-----------

Pass ``PMIX_SERVER_TOOL_SUPPORT``. Without it the server refuses every
tool, including one started from a job script, which has the server's
address in its environment. With it, the server writes the files a tool
finds it by - by pid and by namespace - and accepts tools of its own
user, root, and the user named as ``PMIX_USERID``. The host then makes
the final decision on each one, in ``tool_connected2`` (see
:doc:`connections`).

**Tools of other users** need two more things:

* ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``, so that PMIx accepts them and
  makes the rendezvous files readable to them; and
* a socket they can open: ``PMIX_SOCKET_MODE`` giving them write
  permission, with ``PMIX_GRPID`` naming a group they belong to (mode
  0660), or for all users 0666.

Either one alone admits nobody new.

**Tools of other users must also trust the server.** A tool talks only
to a server running as its own user or as root. A server running as a
service account is reached only by tools whose
``ptl_base_server_access`` names that account, which is a setting in
the users' parameter files, not the host's.


Be found at a known name
------------------------

Each of these but the scheduler and the system controller needs
``PMIX_SERVER_TOOL_SUPPORT`` too. Without it the file is either not
written or, where it is, every tool that reads it is refused. Those two
roles imply it, since tools are all they serve: a host that passes
``PMIX_SERVER_TOOL_SUPPORT`` false with either fails
``PMIx_server_init``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - To be
     - Pass
   * - the node's system server
     - ``PMIX_SERVER_SYSTEM_SUPPORT``. A root server's file goes in
       ``/run/pmix`` unless ``PMIX_SYSTEM_TMPDIR`` names another
       directory.
   * - a session's server
     - ``PMIX_SERVER_SESSION_SUPPORT`` and ``PMIX_SESSION_RNDZ_ID``, an ID
       unique among the session servers on the node; or, failing it,
       ``PMIX_SESSION_ID``. With neither, ``PMIx_server_init`` fails.
   * - the system scheduler
     - ``PMIX_SERVER_SCHEDULER``. Runtimes attach to a scheduler as
       tools, and the role implies ``PMIX_SERVER_TOOL_SUPPORT``. See
       :doc:`/transition-v7/schedulers`.
   * - the system controller
     - ``PMIX_SERVER_SYS_CONTROLLER``.
   * - found at a path of the host's choosing - by a debugger, say
     - ``PMIX_LAUNCHER_RENDEZVOUS_FILE``.

One server per name on a node: a second server given the same
namespace, system role or session ID fails to initialize while the
first runs.


Serve processes of releases before v7.0
---------------------------------------

Three things, and the host controls only one:

#. PMIx configured ``--enable-legacy-clients``, which is off by default.
#. The support turned on: the host passes ``PMIX_SERVER_LEGACY_PEERS``
   set true, or passes nothing and the site sets
   ``ptl_base_legacy_clients``.
#. The server listens for them on a loopback interface, so they must be
   on its host - as every client and tool must.

The attribute overrides the site's parameter in both directions. A host
that passes it false turns the support off even where the site asked
for it; a host that passes it true in a build without the support fails
``PMIx_server_init``. A host that does not need to decide should not
pass it, and should say in its own documentation that the site's
parameter applies. PRRTE passes nothing.

Without all three, a client of an earlier release - an Open MPI 5
process, say - runs as a singleton, and a tool of an earlier release
cannot connect. See :ref:`clients of earlier releases
<label-conn-earlier-clients>`.


Report the server's address
---------------------------

``PMIX_TCP_REPORT_URI`` set to ``-`` or ``+`` writes the local socket's
address to stdout or stderr: the one a process of this release uses.
``tcp-`` and ``tcp+`` write the TCP address, for a process of an
earlier release, and fail ``PMIx_server_init`` when the server takes no
legacy clients. A number writes to that file descriptor; anything else
names a file. See :doc:`/how-things-work/connections`.


Directories
-----------

The server makes its local socket under ``PMIX_SERVER_TMPDIR`` (else
``TMPDIR``, else ``/tmp``). The files tools find it by go under
``PMIX_SYSTEM_TMPDIR`` (with the same fallbacks), in
``pmix-user.<uid>`` - or, for a root system server, scheduler or system
controller given no system tmpdir, in ``/run/pmix``. Two things fail
``PMIx_server_init``:

* a socket path longer than about 100 characters - macOS's default
  ``TMPDIR`` is too long;
* a directory on the path that another user could change, unless the
  site sets ``ptl_base_tmpdir_check`` to ``warn`` or ``silent``.

See :ref:`PMIx_server_init(3) <man3-PMIx_server_init>` ("The tmpdirs").


What not to pass
----------------

* ``PMIX_SERVER_REMOTE_CONNECTIONS`` set true fails
  ``PMIx_server_init``: a server takes connections from its own host
  only.
* After a failed ``PMIx_server_init``, do not call
  ``PMIx_server_finalize``. Correct what was refused and call
  ``PMIx_server_init`` again.
