.. _label-host-env-connections:

Accepting Connections
=====================

Every client and tool reaches a PMIx server over a connection. This
page says what PMIx checks when a connection arrives, what it tells
the host, and what the host is expected to check in turn. **The host
always makes the final decision to accept or reject a connection.**


What PMIx checks
----------------

PMIx's checks are a first pass. They reject a connection that is
plainly invalid as early as possible, before the host spends any work
on it.

**For every connection:**

* The opening message must arrive within a time limit
  (``ptl_base_connect_ack_timeout``) and stay within a size limit.
* The peer must say what it is: a client, a tool, or a launcher.
* The peer's PMIx version must be one the server supports.
* The peer's credential is checked by the security (``psec``)
  component the peer named, which must be one the server has loaded.
  That check settles the peer's user ID (uid) and group ID (gid). How
  it does so depends on the component - see `Authentication methods`_.

**For a client,** in addition:

* Its namespace and rank must already be registered with this server
  (:ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`).
* The uid and gid the credential establishes must be the ones the host
  registered for that client.
* A rank takes one connection at a time. A second connection for a
  rank that already has one - live, or waiting on the host's answer -
  is refused with ``PMIX_ERR_EXISTS``, and the host is not asked about
  it, whatever pid it comes from. A process the client started before
  it called ``PMIx_Init`` inherits its identity, and makes such a
  connection if it calls ``PMIx_Init`` too. One it starts afterwards
  inherits none: the client library takes the variables
  ``PMIx_server_setup_fork`` set out of the environment as it
  initializes (see :ref:`PMIx_Init(3) <man3-PMIx_Init>`). A rank whose
  earlier connection finalized, or was lost, can connect again.
* **A client of this release that execs is lost**: its connection is
  close-on-exec, so it closes in the exec, and the host is sent
  ``PMIX_ERR_LOST_CONNECTION`` at once, whether the process runs on or
  not (see `When a connection ends`_). The program it execs is a new
  process to the server. A client of an earlier release leaves its
  connection open across an exec: the rank stays connected until the
  program it exec'd exits, and that program cannot connect for the
  rank meanwhile.

A ``PMIX_USERID`` or ``PMIX_GRPID`` in a client's connection data is
ignored. The client's identity is the one the credential establishes.

**For a tool,** in addition:

* The uid the credential establishes replaces whatever uid the tool
  claimed. A gid the tool claims stands only if that user belongs to
  it; otherwise the verified gid replaces it.
* A tool whose uid differs from the server's is refused unless the host
  started the server with ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``.
* A tool that says it is a registered client, or that joins a namespace
  the server already knows, must have that process's uid and gid.
* A tool joining a rank that already has a connection is refused with
  ``PMIX_ERR_EXISTS``, as a client is. A tool that execs is lost, as a
  client is - the host is sent ``PMIX_ERR_LOST_CONNECTION`` - and if its
  new program initializes, it connects as a new tool, which the host is asked about, under the same name if it
  names one.
* A tool that took its namespace and rank from its environment - one
  started from a job script, which inherits the script's - and is
  refused that way connects again as a tool that needs an identity.
  **The host must then give it one**: such a tool arrives at
  ``tool_connected2`` with no ``PMIX_NSPACE``. A tool of an earlier
  release does not connect again; its ``PMIx_tool_init`` fails with
  ``PMIX_ERR_EXISTS``.
* A tool of an earlier release with the server's address in its
  environment but no namespace and rank also arrives at
  ``tool_connected2`` with no ``PMIX_NSPACE``, and must be given an
  identity in the same way. The tool is not told the name it was given.
* **The host decides on one tool of a name at a time.** A tool that
  brings its own namespace and rank waits while the host decides on a
  tool of that name, so the host never decides on two tools for one
  name at once. Once it accepts a tool, any waiting tool of the same
  name is refused without the host being asked. Tools of other names,
  and tools that need an identity, are put to the host together: the
  host must keep the names it gives unique, and a tool it gives a name
  that a tool has taken for itself meanwhile is refused with
  ``PMIX_ERR_EXISTS`` once the host accepts it - the host is then sent
  the notice described below.

A connection that fails any of these checks is sent a status saying
why, and closed. The host is not told about it.


What the host is told
---------------------

A connection that passes reaches the host through one up-call. Until
the host answers, the process waits.

**A client** arrives at
:ref:`client_connected2 <man5-pmix_server_module_t-client_connected2>`.
The host receives:

* the client's process ID (namespace and rank), as its ``proc``
  argument;
* ``PMIX_USERID`` and ``PMIX_GRPID`` - the uid and gid PMIx established
  for the connection, which it has checked against the registration;
* ``PMIX_CRED_TYPE`` - the name of the security component that
  established them;
* ``PMIX_PROC_PID`` - the client's process ID on the node. On the
  local socket it is the pid the kernel reports for the connection, in
  the host's own process-ID namespace; where the kernel reports none,
  the host is given none. Over TCP - a client of an earlier release -
  it is the one the client's PMIx library obtains with ``getpid()`` and
  sends, which for a client in a container with its own process-ID
  namespace is its pid within that namespace, not the pid the host
  sees;
* ``PMIX_REALUID`` and ``PMIX_REALGID``, if the client's library sent
  them.

**A tool** arrives at
:ref:`tool_connected2 <man5-pmix_server_module_t-tool_connected2>`. The host receives everything
the tool sent, with these replaced by values PMIx established:
``PMIX_USERID`` and ``PMIX_GRPID``, ``PMIX_CRED_TYPE``, and
``PMIX_VERSION_INFO``; and ``PMIX_PROC_PID``, as for a client. The
rest is passed on as the tool sent it: ``PMIX_REALUID`` and
``PMIX_REALGID``, which the tool's PMIx library obtains from the
operating system; ``PMIX_CMD_LINE``; and the roles the tool asked
for.

Both are also given ``PMIX_TIMEOUT``: the number of seconds the host is
asked to answer within. It is what the process has left of the time
it waits for the reply - the server's ``ptl_base_handshake_wait_time``,
less any time it has already spent waiting for this reply, behind a
tool of its name, say - and is absent when that wait is unbounded. A
process with none left is refused with ``PMIX_ERR_TIMEOUT``, and the
host is not asked.

The deprecated forms, ``client_connected`` and ``tool_connected``, are
called only when the newer form is ``NULL``. ``client_connected``
receives no information about the client beyond its process ID.


What the host is expected to check
----------------------------------

PMIx cannot know which processes the host meant to start, or which
tools the site allows. The host does. Before accepting a connection, a
host should check at least:

**For a client:**

* that it started a process with this namespace and rank;
* that ``PMIX_PROC_PID`` is the pid of the process it started with
  that identity. The host knows the pids of the processes it launched;
  PMIx does not.

**For a tool:**

* that the tool's user may use this server at all;
* that what the tool asks to be - a launcher, a scheduler, a debugger
  attaching to a job - is something that user is allowed to be.

PRRTE, the reference host, makes all of these checks.

To reject a connection:

* return an error from ``client_connected2`` or ``tool_connected2``; or
* return ``PMIX_SUCCESS`` and later call the callback with an error.

``tool_connected2`` must return ``PMIX_SUCCESS`` to accept: the tool's
assigned identity can only come back through the callback.

**Answer within** ``PMIX_TIMEOUT``. A host that cannot decide in time
should answer no. The server waits a little longer, then refuses the
process with ``PMIX_ERR_TIMEOUT``. The host still owes its answer -
the callback's ``cbdata`` stays valid until it gives it - and until it
does, the process's name stays held. A slow answer for a tool also
delays every tool behind it.

**A yes does not always stand.** The server may be unable to complete
a connection the host accepted:

* the yes came after the server stopped waiting;
* the process went away while the host decided;
* the host deregistered the client, its job, or the tool's rank while
  it decided.

The host is then sent a ``PMIX_ERR_LOST_CONNECTION`` event naming the
process, as for any process that leaves without finalizing (see
`When a connection ends`_), and should release what it recorded for it.

A host that provides neither connection up-call accepts every client
PMIx accepts. Tools that need an identity assigned are then refused.


When a connection ends
----------------------

* **A client or tool that finalizes** reaches the host through
  :ref:`client_finalized <man5-pmix_server_module_t-client_finalized>`.
  For a tool, ``server_object`` is ``NULL``.
* **A connection that closes without a finalize** - a process that
  died, a tool that disconnected, or a tool's connection to a server
  other than the one it finalized with - raises
  ``PMIX_ERR_LOST_CONNECTION`` naming the process. It goes only to the
  event handlers the host registered in its own process with
  :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
  not to clients or tools.
* **A process the host accepted that never became connected** raises
  the same event (see `What the host is expected to check`_). It is not
  raised when another connection has the process's name by then, since
  it would name that connection.
* **A process of this release that execs** raises the same event, at
  once: its connection closes in the exec.

A client is the host's child, so the host sees it end anyway. A tool
the host did not start is not, and the lost-connection event is the
only notice that it left. **A host that accepts such tools should
register a handler for** ``PMIX_ERR_LOST_CONNECTION`` and release what
it holds for the tool when the event names it. PRRTE does.

A tool that disconnects is treated as gone: neither PMIx nor the host
can tell when it later ends, so its state is released at once. If it
connects again, it arrives as a new tool.

See :doc:`/how-things-work/connections` for everything the server does
when a connection ends.


Authentication methods
----------------------

The ``psec`` framework does the authentication. Three components exist:

.. list-table::
   :header-rows: 1
   :widths: 14 46 40

   * - Component
     - How it establishes the peer's uid and gid
     - Availability
   * - ``native``
     - Takes the uid and gid the kernel reports for a connection on the
       local socket. Over TCP, asks the server's own kernel who owns the
       peer's end of the connection. Serves peers on the same node only.
     - Always built. The default.
   * - ``munge``
     - Decodes a credential issued by the MUNGE daemon. Works across
       nodes.
     - Built only when configured ``--with-munge``. Preferred over
       ``native`` when present, and skipped at run time if the MUNGE
       daemon does not respond.
   * - ``ssl``
     - Verifies an X.509 certificate against a certificate authority.
       The certificate names a local account, whose uid must match.
       Authenticates the connection; does not encrypt it.
     - Built only when configured ``--with-openssl``. Active only when
       given a CA file or a certificate and key.

A server loads every component that is available and permitted, and
uses for each peer the one that peer names. A server with no usable
component fails to start; it never accepts unauthenticated
connections.

From v7.0, ``native`` takes the peer's uid from the kernel rather than
from the peer. For how it works, and what a container needs for it,
see :doc:`/how-things-work/authentication/native`. For a survey of
methods for authenticating a peer in a container, see
:doc:`/how-things-work/authentication/methods`.

What a system administrator can control is on
:ref:`label-sysadmin-authentication`.


Credentials requested by applications
-------------------------------------

:ref:`PMIx_Get_credential(3) <man3-PMIx_Get_credential>` and
:ref:`PMIx_Validate_credential(3) <man3-PMIx_Validate_credential>` let
an application obtain and check credentials of its own - for example,
to authenticate to another service. For a client connected to a server,
these are the host's job. PMIx passes the request to the host's
``get_credential`` or ``validate_credential`` up-call, with the
requester's identity. If the host does not provide the up-call, the
client receives ``PMIX_ERR_NOT_SUPPORTED``.
