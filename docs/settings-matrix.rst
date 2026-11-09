.. _label-settings-matrix:

Settings and What They Enable
=============================

Most of what a PMIx server does is switched on in more than one place,
and each place belongs to a different person. This page puts them side
by side, so that whoever builds PMIx, whoever runs it, and whoever
writes the program that hosts it can see what the others must also do.

.. list-table::
   :header-rows: 1
   :widths: 24 26 50

   * - Layer
     - Set by
     - What it decides
   * - ``configure`` options
     - whoever builds PMIx
     - What is in the library at all.
   * - MCA parameters
     - the system administrator, in a parameter file or the environment
       (see :doc:`/mca`)
     - Site policy, read by each process as it initializes.
   * - Attributes passed to
       :ref:`PMIx_server_init(3) <man3-PMIx_server_init>` - or to
       :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` by a launcher or
       scheduler
     - the developer of the host environment (PRRTE, a resource
       manager, a scheduler), in its code
     - What this server is for: whether it serves tools, which role it
       plays, who may reach its socket.

Four rules hold throughout:

#. **A behavior happens only when every layer it needs says yes.** A
   run-time setting cannot add what the build left out, and a host
   cannot use what the site turned off.
#. **Asking for something the build left out is not always ignored.**
   Some requests make the process fail to initialize; others quietly do
   nothing. The tables below say which.
#. **Where an attribute and an MCA parameter control the same thing,
   the attribute wins.** A host that passes an attribute overrides the
   site; a host that passes nothing leaves the decision to the site.
#. **The settings that count are those of the process that reads
   them.** Server policy belongs in the parameter files the host's
   daemons read. Two settings are read by clients and tools instead -
   ``ptl_base_server_access`` and ``ptl_base_legacy_servers`` - and
   belong where those processes read them: the users' parameter files
   or environment.

``pmix_info`` shows what an installation was built with and the value
of every parameter:

.. code-block:: sh

   shell$ pmix_info                    # build options and components
   shell$ pmix_info --param ptl all    # connection parameters and values


.. _label-settings-matrix-surprises:

Common surprises
----------------

Each line is a combination that does not do what it looks as though it
should. The sections after this one give the full tables.

.. list-table::
   :header-rows: 1
   :widths: 40 35 25

   * - If...
     - then...
     - To get the behavior
   * - PMIx is configured ``--enable-legacy-clients`` (or
       ``--enable-legacy-peers``), but neither
       ``ptl_base_legacy_clients`` nor the host's
       ``PMIX_SERVER_LEGACY_PEERS`` is set
     - the server has no TCP listener, and its environment and
       rendezvous files name only its local socket. A client of a
       release before v7.0 cannot use that address and runs as a
       singleton; a tool of such a release cannot connect.
     - Set ``ptl_base_legacy_clients = true`` where the host's daemons
       read it, or have the host pass ``PMIX_SERVER_LEGACY_PEERS``.
   * - PMIx is configured *without* ``--enable-legacy-clients`` (the
       default), and ``ptl_base_legacy_clients`` or
       ``PMIX_SERVER_LEGACY_PEERS`` is set true
     - every server, and every launcher or scheduler (``prte``,
       ``prun``), that reads the setting **fails to initialize**,
       saying the support was not built.
     - Rebuild with ``--enable-legacy-clients``, or remove the setting.
       To turn on whatever compatibility the build has, set
       ``ptl_base_legacy_peers`` instead: it asks for nothing the build
       lacks.
   * - PMIx is configured *without* ``--enable-legacy-servers`` (the
       default), and ``ptl_base_legacy_servers`` is set true
     - every client and tool (``prte``, ``prun`` among them) that reads
       the setting **fails to initialize**, saying the support was not
       built.
     - Rebuild with ``--enable-legacy-servers``, or remove the setting -
       or set ``ptl_base_legacy_peers``, as above.
   * - The site sets ``ptl_base_legacy_clients = true`` and the host
       passes ``PMIX_SERVER_LEGACY_PEERS`` set false
     - legacy clients are off.
     - The host's attribute wins; a host should pass it only when it
       means to override the site.
   * - ``ptl_base_legacy_servers = true`` is set on the server's node
     - the server still takes no older clients. The parameter only
       widens where a process told of no server searches for one.
     - To take older clients, see the first row.
   * - The host does not pass ``PMIX_SERVER_TOOL_SUPPORT``
     - the server writes none of the files tools find it by, and refuses
       **every** tool with ``PMIX_ERR_NOT_SUPPORTED`` - even one started
       from a job script, which has the server's address in its
       environment. Clients are not affected.
     - The host passes ``PMIX_SERVER_TOOL_SUPPORT``. A launcher passes it
       to ``PMIx_tool_init`` the same way.
   * - The host passes ``PMIX_SERVER_SYSTEM_SUPPORT`` or
       ``PMIX_SERVER_SESSION_SUPPORT`` without
       ``PMIX_SERVER_TOOL_SUPPORT``
     - no system or session file is written, and tools are refused.
     - Pass ``PMIX_SERVER_TOOL_SUPPORT`` as well.
   * - The host passes ``PMIX_LAUNCHER_RENDEZVOUS_FILE`` without
       ``PMIX_SERVER_TOOL_SUPPORT``
     - the file is written, and a debugger that reads it is refused.
     - Pass ``PMIX_SERVER_TOOL_SUPPORT`` as well.
   * - The host passes ``PMIX_SERVER_SESSION_SUPPORT`` with neither
       ``PMIX_SESSION_RNDZ_ID`` nor ``PMIX_SESSION_ID``
     - the server fails to initialize, whether or not it supports tools.
     - Pass one of the two IDs.
   * - The host passes ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` and leaves the
       socket at its default mode
     - tools of other users can read the rendezvous files, but the
       kernel refuses their connection to the socket (mode 0600).
     - Widen the socket too: ``PMIX_SOCKET_MODE``, with ``PMIX_GRPID``
       for a group.
   * - The host widens the socket (``PMIX_SOCKET_MODE``) but does not
       pass ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``
     - tools of other users reach the socket and are refused by PMIx -
       except root's and those of the user named as ``PMIX_USERID``.
     - Pass ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` as well.
   * - The server runs as a service account, not as root or as the
       users
     - users' tools will not talk to it: a tool talks only to a server
       running as its own user or root, or one that started it.
     - Name the account in ``ptl_base_server_access``
       (``users=<account>``) in the users' parameter file.
   * - Something other than the server's own directory is at
       ``<system tmpdir>/pmix-user.<uid>``
     - the server reports it and runs on, writing its tool files in
       ``$XDG_RUNTIME_DIR/pmix`` if it can, else not at all. Clients are
       not affected; tools of other users cannot find it.
     - Remove what is at the name.
   * - A directory on the path to the server's temporary directory can
       be changed by another user (a Kubernetes ``emptyDir``, say)
     - the server fails to initialize.
     - Use another directory, or set ``ptl_base_tmpdir_check`` to
       ``warn`` or ``silent``.
   * - The temporary directory's path is long (macOS's default
       ``$TMPDIR``, for one)
     - the server fails to initialize: the socket's path is limited to
       about 100 characters.
     - Start the host with a short ``TMPDIR`` or ``PMIX_SERVER_TMPDIR``.
   * - The host passes ``PMIX_TCP_REPORT_URI`` set to ``tcp-`` or
       ``tcp+`` and legacy clients are off
     - the server fails to initialize: it has no TCP address to report.
     - Turn legacy clients on, or report ``-`` or ``+``.
   * - The host passes ``PMIX_SERVER_REMOTE_CONNECTIONS`` set true
     - the server fails to initialize. A server takes connections from
       its own host only.
     - Remove the attribute.
   * - PMIx is configured *without* ``--enable-legacy-servers`` (the
       default) and an application runs under a resource manager of a
       release before v7.0 - a container brought to an older system,
       say
     - its ``PMIx_Init`` fails: it does not fall back to running as a
       singleton.
     - Configure ``--enable-legacy-servers`` (or
       ``--enable-legacy-peers``) for the PMIx the application uses.
   * - MUNGE is installed, but PMIx is not configured ``--with-munge``
     - the ``munge`` component is not built, and connections are
       authenticated by ``native``.
     - Configure ``--with-munge``.
   * - Nodes of one job are built with different compression libraries
     - a node whose component is ``zlib`` cannot read data a node
       compressed with ``zstd`` or ``lz4``.
     - Build every node the same way.


.. _label-settings-matrix-legacy:

Processes of releases before v7.0
---------------------------------

Processes of this release reach a server on their host over its local
socket. Processes of earlier releases reach a server over TCP, and the
two directions - a server taking older clients, and a client reaching
an older server - are built and controlled separately. See
:ref:`clients of earlier releases <label-conn-earlier-clients>`.

A server taking clients and tools of earlier releases
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 25 30 45

   * - Built with
     - At run time
     - The server
   * - default (no ``--enable-legacy-clients``)
     - nothing set
     - Has no TCP listener. Clients of earlier releases run as
       singletons; tools of earlier releases cannot connect.
   * - default
     - ``ptl_base_legacy_clients`` or ``PMIX_SERVER_LEGACY_PEERS`` true
     - **Fails to initialize.** So does a launcher or scheduler tool
       that reads the setting.
   * - default
     - ``ptl_base_legacy_peers`` true
     - As in the first row. The parameter turns on only the directions
       the build has, and this build has neither.
   * - ``--enable-legacy-clients`` or ``--enable-legacy-peers``
     - nothing set; or the host passes ``PMIX_SERVER_LEGACY_PEERS``
       false
     - As in the first row: the support is in the library and nothing
       turns it on.
   * - ``--enable-legacy-clients`` or ``--enable-legacy-peers``
     - ``ptl_base_legacy_clients`` or ``ptl_base_legacy_peers`` true,
       and the host passes no ``PMIX_SERVER_LEGACY_PEERS``
     - Listens on TCP, on a loopback interface only, besides its local
       socket. The address processes of earlier releases read - in the
       environment of the processes it starts, first in its rendezvous
       files, and from ``PMIX_TCP_REPORT_URI`` ``tcp-``/``tcp+`` - is the
       TCP one; the local socket's is given beside it. Processes of this
       release still use the local socket.
   * - ``--enable-legacy-clients`` or ``--enable-legacy-peers``
     - the host passes ``PMIX_SERVER_LEGACY_PEERS`` true
     - As in the row above, whatever the parameters say.

While a server takes legacy clients, a connection over TCP that
declares a release before v7.0 is not checked for a connection token.
On a platform whose kernel keeps no table of TCP connections (any but
Linux, macOS and FreeBSD 12 or later), ``native`` takes such a peer's
user and group as it gives them, as every earlier release did. See
:doc:`/hosts/connections`.

A client or tool reaching a server of an earlier release
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 25 30 45

   * - Built with
     - At run time
     - A client or tool of this release
   * - default (no ``--enable-legacy-servers``)
     - nothing set, or ``ptl_base_legacy_peers`` true
     - Refuses a server of an earlier release, and any server address
       over TCP, naming the reason. A client's ``PMIx_Init`` fails rather
       than running as a singleton.
   * - default (no ``--enable-legacy-servers``)
     - ``ptl_base_legacy_servers`` true
     - ``PMIx_Init`` and ``PMIx_tool_init`` fail, saying the support was
       not built.
   * - ``--enable-legacy-servers`` or ``--enable-legacy-peers``
     - nothing set
     - Connects to a server of v3.2 through v6.x that started it or that
       it is told of, over TCP. Refuses a server older than v3.2
       (``PMIX_ERR_OUTDATED``).
   * - ``--enable-legacy-servers`` or ``--enable-legacy-peers``
     - ``ptl_base_legacy_servers`` or ``ptl_base_legacy_peers`` true,
       where the client or tool reads it
     - Also, when told of no server, searches the whole system temporary
       directory, where earlier releases write their rendezvous files.

With neither ``--enable-legacy-clients`` nor ``--enable-legacy-servers``
- the default - the library has no TCP transport, and no wire format
but this release's.


.. _label-settings-matrix-tools:

Tools, and the files they find a server by
------------------------------------------

A client is told where its server is, in the environment the server
gave it. A tool - a debugger, ``prun``, a monitor, a runtime attaching
to a scheduler - finds a server through a *rendezvous file*. Which
files a server writes, and whether it accepts the tool that reads one,
depends only on the attributes its host passes.

.. list-table::
   :header-rows: 1
   :widths: 32 33 35

   * - The host passes
     - Files written
     - Tools
   * - none of the attributes below
     - none
     - All refused, ``PMIX_ERR_NOT_SUPPORTED``.
   * - ``PMIX_SERVER_TOOL_SUPPORT``
     - ``pmix.<host>.tool.<pid>`` and ``pmix.<host>.tool.<nspace>``, in
       the server's user directory
     - Accepted from the server's user, root, and the user named as
       ``PMIX_USERID`` - subject to the host's own decision.
   * - ``PMIX_SERVER_TOOL_SUPPORT`` and ``PMIX_SERVER_SYSTEM_SUPPORT``
     - also ``pmix.sys.<host>``, in the system directory
     - As above. This is the server a tool asking for the system server
       finds.
   * - ``PMIX_SERVER_SYSTEM_SUPPORT`` alone
     - none
     - All refused.
   * - ``PMIX_SERVER_TOOL_SUPPORT``, ``PMIX_SERVER_SESSION_SUPPORT`` and
       ``PMIX_SESSION_RNDZ_ID`` (or ``PMIX_SESSION_ID``)
     - also ``pmix.<host>.session.<id>``, in the server's user directory
     - As above. A second server given the same ID on the node fails to
       initialize.
   * - ``PMIX_SERVER_SESSION_SUPPORT`` with an ID, without
       ``PMIX_SERVER_TOOL_SUPPORT``
     - none
     - All refused.
   * - ``PMIX_SERVER_SESSION_SUPPORT`` with no ID
     - \-
     - The server fails to initialize.
   * - ``PMIX_SERVER_SCHEDULER``
     - ``pmix.sched.<host>``, in the system directory, and no other
     - Accepted as for ``PMIX_SERVER_TOOL_SUPPORT``, which the role
       implies: the runtimes a scheduler serves connect as tools.
       ``PMIX_SERVER_TOOL_SUPPORT`` passed false fails the init.
   * - ``PMIX_SERVER_SYS_CONTROLLER``
     - ``pmix.sysctrlr.<host>``, in the system directory, and no other
     - As for ``PMIX_SERVER_SCHEDULER``.
   * - ``PMIX_LAUNCHER_RENDEZVOUS_FILE``
     - the file named, at the path given
     - Refused unless ``PMIX_SERVER_TOOL_SUPPORT`` is passed too.
   * - ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``, with
       ``PMIX_SERVER_TOOL_SUPPORT``
     - the same files, readable by all users; the user directory, if
       the server makes it, mode 0711
     - Also accepted from other users - who must be able to reach the
       socket as well (next section).

**Where the files go.** The *user directory* is
``<system tmpdir>/pmix-user.<uid>``; when something other than the
server's own directory is at that name, ``$XDG_RUNTIME_DIR/pmix``, if
that is a directory of the server's user that nobody else can write;
else the files are not written and the server runs on. The *system
directory* is ``PMIX_SYSTEM_TMPDIR`` when the host names one, else
``/run/pmix`` (``/var/run/pmix``) for a server running as root, else
the server's user directory.

**What a tool believes.** A tool reads a file only if its owner is the
tool's own user, root, or a user ``ptl_base_server_access`` names, and
talks only to a server that the kernel says runs as that user. A server
whose files tools of another user are to find runs as root, or as that
user, or is named in those users' ``ptl_base_server_access``.

A server whose namespace, system role or session ID is already held by
a running server on the node fails to initialize, rather than take over
that server's file. A file whose server is gone is
removed and replaced.

The rest is in :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`
("Rendezvous files") and :doc:`/how-things-work/ptl`.


.. _label-settings-matrix-socket:

Who can reach the socket
------------------------

A process of another user needs two things to connect: the kernel must
let it open the server's local socket, and PMIx must accept its user.
The host controls both, separately.

.. list-table::
   :header-rows: 1
   :widths: 26 37 37

   * - The process's user
     - The socket must admit it
     - PMIx accepts a tool of that user (given
       ``PMIX_SERVER_TOOL_SUPPORT``)
   * - the server's own
     - by default (mode 0600)
     - by default
   * - root
     - always
     - by default
   * - the user named as ``PMIX_USERID``
     - by default: ``PMIX_USERID`` makes that user the socket's owner
     - by default
   * - a member of the group named as ``PMIX_GRPID``
     - with ``PMIX_SOCKET_MODE`` giving the group write permission
       (0660, say)
     - only with ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``
   * - anyone else
     - with ``PMIX_SOCKET_MODE`` giving others write permission (0666,
       say)
     - only with ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``

The same socket rule applies to a client the host starts under another
user: a server running as root for a job's user names that user as
``PMIX_USERID``. A client is always checked against the user and group
the host registered it with; no setting changes that.

A tool also checks the server, from its own side: see *What a tool
believes*, above. And the host has the last word on every tool PMIx
accepts - see :doc:`/hosts/connections`.


.. _label-settings-matrix-auth:

Authentication
--------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Setting
     - Effect
   * - default build
     - ``native`` only: the kernel names each peer.
   * - ``--with-munge``
     - Builds ``munge``, which is then preferred to ``native`` for every
       connection while the MUNGE daemon responds. Not built without the
       option, even where MUNGE is installed.
   * - ``--enable-mca-no-build=psec-munge``
     - Leaves ``munge`` out.
   * - ``psec`` MCA parameter (``munge``, ``^munge``, ...)
     - Restricts the components a server loads. A server left with none
       fails to initialize.

A process can use only the methods its server offers, so the server's
settings decide. See :ref:`label-sysadmin-authentication`.


.. _label-settings-matrix-other:

Other build options that change behavior
----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Option
     - Effect
   * - ``--with-zstd``, ``--with-lz4``, ``--with-zlib``,
       ``--with-zlibng``
     - The component used is the first of ``zstd``, ``zlibng``, ``lz4``
       and ``zlib`` that the node's build found, unless the ``pcompress``
       parameter names one. Compressed data crosses nodes, so every node
       of a job must make the same choice. See
       :ref:`label-install-optional-support-libraries`.
   * - (no option) the shared-memory datastore, ``gds/shmem3``
     - Built on 64-bit platforms other than macOS, and used at run time
       where ``/proc/self/maps`` exists; elsewhere clients hold job data
       in their own memory. See :doc:`/how-things-work/shmem3`.
   * - ``--disable-per-user-config-files``
     - ``$HOME/.pmix/mca-params.conf`` is not read. Settings that belong
       to users - ``ptl_base_server_access``, ``ptl_base_legacy_servers``
       - must then be made in the environment or the installation's
       parameter file.
   * - ``--enable-mca-dso``
     - Each component is a separate plugin, which an administrator can
       remove to make it unavailable.
   * - ``--enable-test-build``
     - Compiles every optional component against stand-in headers. The
       result does not work; never install it.


.. _label-settings-matrix-roles:

What each person must do
------------------------

**Whoever builds PMIx**

* Decide on clients of earlier releases. If containers or applications
  with an older PMIx inside run on the system, configure
  ``--enable-legacy-clients``; it does nothing until turned on at run
  time.
* Decide on servers of earlier releases. If applications built against
  this PMIx - in a container image, say - may run under a resource
  manager of an earlier release, configure ``--enable-legacy-servers``;
  without it their ``PMIx_Init`` fails there.
* Configure ``--with-munge`` if the site authenticates with MUNGE.
* Build every node with the same compression libraries.

**The system administrator**

* Turn legacy clients on with ``ptl_base_legacy_clients = true`` in the
  parameter file the host's daemons read - PRRTE does not pass
  ``PMIX_SERVER_LEGACY_PEERS``, so for PRRTE the parameter is the only
  way. Do not set it, or ``ptl_base_legacy_peers``, in a build without
  ``--enable-legacy-clients``.
* Keep the temporary directory short, and on a path only root and the
  server's user can change.
* Where a DVM or scheduler runs as a service account, name it in the
  users' ``ptl_base_server_access``.

**The host environment's developer**

* Pass ``PMIX_SERVER_TOOL_SUPPORT`` if any tool is to connect -
  including from a scheduler, a system server, a session server, and a
  launcher, which passes it to ``PMIx_tool_init``.
* To take tools of other users, pass
  ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` *and* widen the socket.
* Give a session server its ID.
* Leave ``PMIX_SERVER_LEGACY_PEERS`` unset unless the host means to
  override the site, and expose the choice to the site if it does.

What each of these attributes does is in
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. What the host is
expected to do once it is running is in :ref:`label-host-env`, and
:ref:`label-host-env-starting` sets out, goal by goal, what a host
passes to get each behavior.
