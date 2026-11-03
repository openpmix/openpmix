Tools and PMIx v7
=================

For developers of tools: debuggers, launcher front ends such as
``prun``, monitors, schedulers, and anything else that calls
``PMIx_tool_init``.

The reference for this page is :doc:`/how-things-work/connections`;
each section below summarizes part of it.


Tools connect only on their own node
------------------------------------

**What changed.** PMIx messaging is node-local. A tool connects only
to a PMIx server on the node it runs on. Connections from other nodes
are gone, along with the ``ssl`` authentication that served them.

* A tool will not dial a TCP address that is not its own host's
  (``PMIX_ERR_UNREACH``).
* A server takes no connection from another node, and a host that asks
  it to (``PMIX_SERVER_REMOTE_CONNECTIONS``) fails to start.

**What it means.** A tool must run on the same node as the server it
talks to. A tool that needs something done on other nodes asks the
host to do it, through the server on its own node - as clients always
have.

**Today.** A user running a launcher or a debugger from a login node
uses ssh to reach the node where the DVM controller (``prte`` or
``mpirun``) is running, and starts the tool there. This is the
approach DDT recommends.

**Coming.** Launching from a login node will remain possible. The
method is being worked out: most likely ``prun`` becomes a PRRTE
function that sends its launch request through PRRTE's own messaging.
Debuggers are not settled yet.

.. DRAFT: update when the login-node method is decided.


How a tool finds a server
-------------------------

**What changed.**

* A v7 server is reached through a local socket (a ``usock://``
  address), not TCP.
* **Rendezvous files moved.** A server writes its files in its user's
  own directory, ``<tmpdir>/pmix-user.<uid>/``. A system server,
  scheduler, or system controller running as root writes its file in
  ``/run/pmix``, unless the host names a system temporary directory.
* **A tool looks in a few known places**, each only at that level:
  its own user's directory, the directories of the users that
  ``ptl_base_server_access`` names, and the system directory for the
  system roles. It no longer walks the whole temporary directory. The
  walk happens only when legacy peers are on, to find servers of
  earlier releases.
* **A tool believes a rendezvous file only if** it was written by the
  user the server runs as, and the kernel confirms that the server
  behind the socket is that user's.
* **A TCP address for a v7 server is refused** with
  ``PMIX_ERR_LOCAL_URI_REQUIRED``. Give the tool the local address
  (``--report-uri -`` prints it now) or let it find the rendezvous
  file.

**What to do.** A tool that finds its server by pid, by namespace, or
by searching needs no change. A tool that takes an address from a
person or a script needs the local one.


Who the tool is, and whom it may reach
--------------------------------------

**What changed.**

* **The kernel establishes the tool's identity.** Its user, groups and
  pid are what the kernel reports for the connection. A user or group
  the tool claims is replaced; a group it asks to be charged to stands
  only if it is one of the groups the kernel reports.
* **The tool checks the server too.** It talks to a server running as
  its own user or as root, or one that ``ptl_base_server_access``
  names.
* **A server takes tools only if its host asked for tool support.**
* **Tools of another user are refused by default.** The host must
  allow them; and a tool it allows sees only the jobs whose access
  lists include its user or one of its groups
  (``PMIX_ERR_NO_PERMISSIONS`` otherwise).
* No setting in the tool overrides the server's policy.

**What to do.** A tool that runs as the job's owner needs no change.
A tool that serves several users - a site monitor, say - needs the
host to allow it, and each job's owner to grant it access.


Names, ranks, and namespaces
----------------------------

**What changed.** A rank has one connection at a time.

* **A tool that names a rank already connected is refused** with
  ``PMIX_ERR_EXISTS``.
* **A tool started from a job script** inherits the script's namespace
  and rank. If that rank is taken, a v7 tool connects again without an
  identity and the host names it. A tool of an earlier release fails
  with ``PMIX_ERR_EXISTS`` instead.
* **The first process to connect holds the rank.** A tool that takes a
  job script's rank before the script's application connects leaves
  the application refused. Start tools from a job script without the
  script's ``PMIX_NAMESPACE`` and ``PMIX_RANK``, or with an identity of
  their own.
* **A tool may join an existing namespace** only as that namespace's
  owner, and only as a rank the host registered.
* **A tool that names itself** (``PMIX_TOOL_NSPACE``) is refused with
  ``PMIX_ERR_EXISTS`` if the host registers a job of that name while
  deciding on the tool.
* **A tool a client starts after the client initialized** does not
  inherit the client's identity: the identity variables leave the
  client's environment when it initializes.
* **A tool that** ``exec``\ **s** loses its connection. If the new
  program initializes, it is a new tool.


Launchers and output
--------------------

* **Output files are created as the job's owner.** A launcher whose
  server cannot act as that user - a DVM run by a service account -
  can write the files itself, as the user who ran it, on its own node
  (``PMIX_IOF_FILE_WRITER``).
* **A launcher acts on events, key deletions, and store updates only
  from a server of its own** - the one it connected to, or one it made
  its server with ``PMIx_tool_set_server`` - as it already did for
  output.
* **The library does not relay on a tool's behalf any more.** A
  launcher that acts as a server for other tools passes their spawn and
  query requests to its own host's up-calls. With no up-call, a query
  is refused (``PMIX_ERR_NOT_SUPPORTED``). A spawn is started by a
  launcher attached to nothing, only for a requester of its own user,
  and refused by one attached to a server.
* **stdin**: a tool forwarding stdin pauses on flow control from the
  server rather than failing.


Return codes a tool may now see
-------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Code
     - Meaning
   * - ``PMIX_ERR_EXISTS``
     - Its rank is in use, or the namespace it named was registered for
       a job.
   * - ``PMIX_ERR_LOCAL_URI_REQUIRED``
     - It was given a TCP address for a v7 server.
   * - ``PMIX_ERR_OUTDATED``
     - The server is older than v3.2.
   * - ``PMIX_ERR_NO_PERMISSIONS``
     - The job does not admit the tool's user or groups.
   * - ``PMIX_ERR_OUT_OF_RESOURCE``
     - Too many tools of its user are waiting for the host's decision.
   * - ``PMIX_ERR_BAD_PARAM``
     - A connection directive was malformed - even when the connection
       was optional.
   * - ``PMIX_ERR_WOULD_BLOCK``
     - A blocking PMIx call was made from a PMIx callback or event
       handler. It used to hang.


Earlier releases
----------------

* **A v7 tool connects to servers from PMIx v3.2 on.**
* **A tool of an earlier release connects to a v7 server only when the
  server takes legacy peers**, over loopback TCP. When it does, a v5.0
  tool finds the server's rendezvous file in ``pmix-user.<uid>/`` as
  long as it runs as the server's user: its search walks down into
  that directory. It does not find a v7 system server in
  ``/run/pmix`` unless both are pointed at the same system temporary
  directory.
* **Limitations.** A tool of an earlier release cannot connect again
  with a host-assigned name when its rank is taken (above). A v4.1
  tool cannot use a v7 server built with ``--enable-debug``.
