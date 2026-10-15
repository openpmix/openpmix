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
  it. A process the client forked and exec'd that calls ``PMIx_Init``
  makes such a connection. A rank whose earlier connection finalized,
  or was lost, can connect again.
* A process that calls ``PMIx_Init`` and then execs a program that
  calls it again is the same process, and keeps its rank. The server
  knows it by its pid, and its new connection takes the place of the
  old one, which the new program inherited but cannot use. **The host
  is not asked again, and is not told**: nothing it was told has
  changed. It is told the client finalized once, when the new program
  finalizes. The server knows the pid only if the client's library
  sends it: libraries from v6.1 on do, and those from v5.0 and earlier
  do not. A v5.0 or older client that execs and initializes again is
  therefore refused with ``PMIX_ERR_EXISTS``, as any second connection
  for its rank is.

.. DRAFT: today every second connection is accepted and passed to
   client_connected2, an exec'd process included.

A ``PMIX_USERID`` or ``PMIX_GRPID`` in a client's connection data is
ignored. The client's identity is the one the credential establishes.

.. DRAFT: today the server still refuses a client whose PMIX_USERID or
   PMIX_GRPID differs from the registered value.

**For a tool,** in addition:

* The uid the credential establishes replaces whatever uid the tool
  claimed. A gid the tool claims stands only if that user belongs to
  it; otherwise the verified gid replaces it.
* A tool whose uid differs from the server's is refused unless the host
  started the server with ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``.
* A tool that says it is a registered client, or that joins a namespace
  the server already knows, must have that process's uid and gid.
* A tool joining a rank that already has a connection is refused with
  ``PMIX_ERR_EXISTS``, as a client is, and a tool that execs and
  initializes again keeps its name, as a client does.
* A tool that took its namespace and rank from its environment - one
  started from a job script, which inherits the script's - and is
  refused that way connects again as a tool that needs an identity.
  **The host must then give it one**: such a tool arrives at
  ``tool_connected2`` with no ``PMIX_NSPACE``.
* **Tools are put to the host one at a time,** in the order they
  arrived, so the host never decides on two tools for one name at
  once. Once it accepts a tool, any waiting tool of the same name is
  refused without the host being asked.

.. DRAFT: today a tool joins a rank that already has a connection, a
   launched tool that is refused does not connect again, and the host
   is asked about tools as they arrive.

A connection that fails any of these checks is sent a status saying
why, and closed. The host is not told about it.

.. DRAFT: today most refusals close the connection without a status.


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
* ``PMIX_PROC_PID`` - the client's process ID on the node. The
  client's PMIx library obtains it with ``getpid()`` and sends it when
  it connects. (The kernel does not report the pid at the other end of
  a TCP connection.) A client in a container with its own process-ID
  namespace sends its pid within that namespace, which is not the pid
  the host sees;
* ``PMIX_REALUID`` and ``PMIX_REALGID``, if the client's library sent
  them.

**A tool** arrives at
:ref:`tool_connected2 <man5-pmix_server_module_t-tool_connected2>`. The host receives everything
the tool sent, with these replaced by values PMIx established:
``PMIX_USERID`` and ``PMIX_GRPID``, ``PMIX_CRED_TYPE``, and
``PMIX_VERSION_INFO``. The
rest is passed on as the tool sent it: ``PMIX_PROC_PID``,
``PMIX_REALUID`` and ``PMIX_REALGID``, which the tool's PMIx library
obtains from the operating system; ``PMIX_CMD_LINE``; and the roles the
tool asked for.

Both are also given ``PMIX_TIMEOUT``: the number of seconds the host is
asked to answer within. It is the server's
``ptl_base_handshake_wait_time`` - how long a process waits for each
reply while it connects - and is absent when that wait is unbounded.

The deprecated forms, ``client_connected`` and ``tool_connected``, are
called only when the newer form is ``NULL``. ``client_connected``
receives no information about the client beyond its process ID.

.. DRAFT: today PMIX_TIMEOUT is not passed.


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

.. DRAFT: today the server waits for the host's answer indefinitely,
   and the host is not told when a process it accepted is not
   connected.

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
* **A process that execs and initializes again** raises nothing: its
  connection continues.

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
     - Asks the server's own kernel who owns the peer's end of the
       connection. A peer the kernel cannot see directly, such as one
       in a container with its own network, proves its identity over a
       local socket instead. Serves peers on the same node only.
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
