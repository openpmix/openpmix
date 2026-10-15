.. _connection-lifecycle-label:

Connection Lifecycle: Clients and Tools
=======================================

This page describes the life of a connection between a PMIx server and
one of its clients or tools, from the server opening its listener to
the connection ending. It covers:

#. how a server sets up its listener;
#. how a server tells the clients it starts where to connect;
#. how a client or tool opens a connection;
#. how the server accepts it: the handshake, the security checks, and
   the call to the host;
#. how a connection ends: by finalize, by disconnect, by a process
   dying, and by the host.

Two related subjects are covered elsewhere:

* **How a tool finds a server** - rendezvous files, connection-order
  directives, the tmpdir search - is in :doc:`ptl` for now.
* **Messages on an open connection** - framing, tags, the send and
  receive handlers - are in :doc:`ptl`.

The security mechanisms themselves are in :doc:`authentication/index`.
What a host may do with the identity it is given is in
:doc:`/hosts/authorization`.


The pieces
----------

.. list-table::
   :header-rows: 1
   :widths: 28 34 38

   * - Piece
     - Where
     - Role
   * - Listener
     - ``src/mca/ptl/base/ptl_base_listener.c``, ``src/mca/ptl/tcp/ptl_tcp.c``,
       ``src/mca/ptl/usock/ptl_usock.c``
     - Has each transport open its listening socket - the local socket
       (``ptl_usock.c``) and TCP (``ptl_tcp.c``) - then publishes the
       URIs, writes the rendezvous files and accepts connections.
   * - Connection handler
     - ``src/mca/ptl/base/ptl_base_connection_hdlr.c``
     - Reads and checks each new connection's request, runs the checks,
       calls the host, replies.
   * - Connecting side
     - ``src/mca/ptl/base/ptl_base_connect.c``,
       ``ptl_base_fns.c``, ``src/mca/ptl/base/ptl_base_client.c``
     - Builds and sends the connection request, reads the replies.
   * - Security (``psec``)
     - ``src/mca/psec/{native,munge,ssl}``
     - Creates a credential on the connecting side; checks it, and runs
       any further exchange, on the server.
   * - Host up-calls
     - ``client_connected2``, ``tool_connected2``,
       ``client_finalized`` in
       :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`
     - The host's decision to accept a connection, and its notice that
       a process has finished.
   * - Departure handling
     - ``lost_connection`` in ``ptl_base_sendrecv.c``; the
       ``PMIX_FINALIZE_CMD`` case in ``pmix_server_switchyard.c``;
       ``pmix_server_registration.c``
     - Unwinds a connection and the server state that depends on it.

There is no listener thread. Accepting, reading a connection request,
and every check after it run on the PMIx progress thread.


Rules the connection path follows
---------------------------------

Every phase below is written to these rules. They are what makes the
path predictable for the peer and for the host.

#. **One identity, settled before anyone acts on it.** The peer's user
   and group are settled - its credential checked and any further
   exchange complete - before any policy check is made and before the
   host is told anything.
#. **A connection changes nothing until it is accepted.** Checks run
   against a scratch copy of the peer's identity. Nothing the host
   registered, and nothing another connection relies on, is modified by
   a connection that may yet be refused.
#. **Every check that can refuse runs before the host is asked.** Once
   the host accepts a connection, only the final replies remain.
#. **The progress thread is never held.** No step waits on a peer
   without a time limit, and a slow peer does not stop the server
   serving everyone else.
#. **A refused peer is told why.** Every refusal sends a status before
   closing, so the peer can report something more useful than "server
   unreachable".
#. **Every failure undoes exactly what was done.** Nothing is left
   behind, and nothing is released twice.
#. **Clients and tools are checked alike.** The same checks run in the
   same order on both paths, except where a check only makes sense for
   one of them.
#. **The host is told where each item came from:** the identity the
   security component settled, the registration the host made, or what
   the peer's PMIx library supplied.
#. **Once the host has accepted a connection, it can learn how it
   ends:** a finalize through ``client_finalized``, and a lost
   connection through ``PMIX_ERR_LOST_CONNECTION`` delivered to the
   host's own event handlers. A client is the host's child, so the host
   also sees it end directly.
#. **A departure is handled the same way however it is noticed** - by
   the socket closing without a finalize, or by the host deregistering
   the process. A finalize alone is not a departure: the process may
   initialize again.

.. DRAFT: rules 1-10 are the design. The code does not yet meet all of
   them; the fixes for this area bring it into line.


1. Setting up the listener
--------------------------

A server sets up its listener during ``PMIx_server_init``. A tool sets
one up only if it is a launcher or a scheduler, after it has connected
to its own server.

Setting up happens in two steps:

#. ``pmix_ptl_base_create_listener`` opens the sockets and publishes how
   to reach them. Nothing is accepted yet.
#. ``pmix_ptl_base_start_listening``, at the very end of init, starts
   accepting.

Between the two, the server finishes initializing. A peer that connects
in that window waits in the kernel's backlog until the server starts
accepting.

A server listens on its **local socket**, where every client and tool of
this release on its host connects. It listens on **TCP** only for those
who use it: clients and tools of earlier releases, when it takes them
(``ptl_base_legacy_peers``, or the host's ``PMIX_SERVER_LEGACY_PEERS``;
off by default), and tools on other hosts, when its host asks
(``PMIX_SERVER_REMOTE_CONNECTIONS``). A server that cannot open a socket
it needs fails ``PMIx_server_init``.

The local socket
^^^^^^^^^^^^^^^^

The local socket is an ``AF_UNIX`` socket. The kernel identifies the
process at the far end of each connection to it - its uid, gid and pid -
whatever that process claims.

* It is ``pmix-<pid>``, in a directory of its own that the server makes
  beneath its temporary directory: ``<tmpdir>/pmix-XXXXXX``.
* The directory is mode 0700 and the socket 0600: only the server's own
  user can connect. A host widens that by passing ``PMIX_SOCKET_MODE``,
  and ``PMIX_USERID`` and ``PMIX_GRPID`` for the socket's owner and
  group, to ``PMIx_server_init``. The socket takes its mode and owner
  while the directory is still closed; the directory, which stays the
  server's, is then opened to whom the mode lets in - search permission,
  never read. The server holds the directory open from the moment it
  makes it, and makes every change in it through that descriptor.
* The path must fit an ``AF_UNIX`` address, about 100 characters. A
  temporary directory too long for it fails init; there is no fallback.
  The default ``$TMPDIR`` on macOS is too long - pass a shorter
  ``PMIX_SERVER_TMPDIR``.
* At finalize the server removes the socket and its directory. A server
  that is killed leaves them behind.

Choosing an interface
^^^^^^^^^^^^^^^^^^^^^

For TCP, the server considers each network interface that has an IPv4 address
(or IPv6, if enabled), skipping any the ``if_include`` / ``if_exclude``
settings rule out. Then:

* **By default it listens on loopback only.** Only processes on the
  same node can reach it.
* **With ``PMIX_SERVER_REMOTE_CONNECTIONS``,** it listens on the first
  public interface, and on every other public interface as well. The
  first gives the URI; the others are *alternate* addresses. Only a
  PMIx configured ``--enable-remote-tools`` does this. In one without
  it, a host that asks fails its init. In one with it, a host that asks
  fails its init when neither ``munge`` nor ``ssl`` can validate a
  peer's credential.
* **With no loopback interface,** a PMIx configured
  ``--enable-remote-tools`` uses a public interface - unless
  ``PMIX_SERVER_REMOTE_CONNECTIONS`` was explicitly set to false, in
  which case it fails. One without that option fails.

The port is the first free one from ``ptl_base_ipv4_ports`` (or the
``PMIX_TCP_IPV4_PORT`` directive), and by default is chosen by the
kernel. Each socket is close-on-exec and non-blocking.

Publishing the address
^^^^^^^^^^^^^^^^^^^^^^

Each address is a URI:

.. code-block:: text

   <nspace>.<rank>;usock://<path>
   <nspace>.<rank>;tcp4://<address>:<port>

(``tcp6://`` for IPv6). The server's URI is its TCP one when it listens
on TCP, and its local socket's when it does not. The server stores it
under ``PMIX_MYSERVER_URI`` and ``PMIX_SERVER_URI``, reports it, and
puts it first in its rendezvous files; any alternates go, comma-separated,
under ``PMIX_MYSERVER_ALT_URIS``. A TCP URI reaches processes on other
hosts and older peers, which cannot use the local socket. When the
server has one, the local socket's URI is given beside it where a
process on the host reads it: the environment of the processes the
server starts (see below) and the rendezvous files.

The server can also report the URI on request (``ptl_base_report_uri``
or the ``PMIX_TCP_REPORT_URI`` directive): to stdout (``-``), stderr
(``+``), an open file descriptor (a number), or a file.

Rendezvous files
^^^^^^^^^^^^^^^^

A server that supports tools writes rendezvous files, through which
tools find it. Which files it writes, and where, is described in
:doc:`ptl`; that part is being redesigned. Each file holds the URI, the
server's version, pid, uid and gid, the local socket's URI, and the
wire formats and security mechanisms it accepts. The server removes the
files it wrote when it finalizes.

Accepting
^^^^^^^^^

Once listening, the progress thread runs the accept handler whenever a
connection arrives:

#. ``accept`` the connection and make it non-blocking and close-on-exec.
#. Add it to the list of *pending* connections - accepted, but not yet
   delivered their connection request.
#. Start a timer of ``ptl_base_connect_ack_timeout`` seconds (default
   5). A connection that has not delivered its whole request in that
   time is closed.
#. If ``ptl_base_max_pending_connections`` connections are already
   pending (default: an eighth of the process's file-descriptor limit),
   close the oldest of them. Tools waiting for the host's decision are
   not pending connections and are never the ones closed. They have a
   limit of their own, the same number: once that many tools are
   waiting, a further tool is refused with ``PMIX_ERR_OUT_OF_RESOURCE``.

If ``accept`` itself fails, the handler retries per-connection errors
at once. When the process is out of descriptors or memory, it pauses
accepting for a second at a time until the condition clears.

At finalize the server closes its listening sockets, closes any pending
connections, and removes its rendezvous and URI files.


2. Telling a client where to connect
------------------------------------

A client never searches for its server. The host tells it, through the
environment, before the client starts. The host:

#. registers the job (``PMIx_server_register_nspace``);
#. registers the client, with the uid and gid it will run as
   (``PMIx_server_register_client``);
#. calls :ref:`PMIx_server_setup_fork(3) <man3-PMIx_server_setup_fork>`
   to add the connection information to the client's environment;
#. starts the client with that environment.

A client is local to its server by definition: the host registers it
and prepares its environment on the server's node. It connects to the
local socket. ``PMIX_SERVER_REMOTE_CONNECTIONS`` lets tools on other
nodes reach the server.

``PMIx_server_setup_fork`` sets:

.. list-table::
   :header-rows: 1
   :widths: 30 35 35

   * - Variable
     - Value
     - Used by the client to
   * - ``PMIX_NAMESPACE``, ``PMIX_RANK``
     - the client's identity
     - name itself to the server
   * - ``PMIX_SERVER_LOCAL_URI``
     - the local socket's URI, then ``;<uid>:<gid>`` - the effective
       uid and gid the server runs as
     - connect to the local socket, and check that the server there is
       the one that started it. Clients of earlier releases ignore it
   * - ``PMIX_CONNECT_TOKEN``
     - ``<server nspace>.<server rank>;`` then 128 random bits, in hex,
       that that server made for this rank and keeps
     - present them to the server of that name, however it reaches it,
       and to no other. Clients of earlier releases ignore it
   * - ``PMIX_SERVER_URI<NN>``, one per wire format the server accepts
       (for example ``PMIX_SERVER_URI61``, ``PMIX_SERVER_URI51``)
     - the server's TCP URI when it takes clients of earlier releases,
       else its local socket's URI
     - choose the newest wire format both sides share: the first
       variable the client recognizes wins. A client of an earlier
       release also connects to this URI - and, given the local
       socket's, which it cannot use, initializes as a singleton and
       returns ``PMIX_ERR_UNREACH`` (a v5.0 client also prints a
       ``PMIX_ERR_BAD_PARAM`` error line as it does)
   * - ``PMIX_VERSION``
     - the server's version
     - know which features the server has
   * - ``PMIX_SECURITY_MODE``
     - the security mechanisms the server accepts
     - choose its own mechanism from that list
   * - ``PMIX_BFROP_BUFFER_TYPE``
     - the buffer type
     - format its messages as the server expects
   * - ``PMIX_GDS_MODULE``
     - the datastores the server offers
     - choose how to receive job data
   * - ``PMIX_SERVER_TMPDIR``, ``PMIX_SYSTEM_TMPDIR``
     - the server's temporary directories
     - place its own temporary files
   * - ``PMIX_HOSTNAME``
     - the server's name for this node
     - use the same node name as the server

A client takes these out of its environment as ``PMIx_Init`` begins -
all but ``PMIX_SERVER_TMPDIR``, ``PMIX_SYSTEM_TMPDIR`` and
``PMIX_HOSTNAME`` - and keeps them for its own process, where a later
``PMIx_Init`` of the same process reads them. A tool does the same at
``PMIx_tool_init``. Both also take ``PMIX_LAUNCHER_RNDZ_URI``,
``PMIX_SERVER_NSPACE`` and ``PMIX_SERVER_RANK``, which a launcher sets
for a process it starts itself. The token is overwritten where the
environment held it. A process the client or tool starts once it has
initialized - forked, or forked and exec'd - and a program it execs
inherit none of them, and a process forked from it does not find the
ones it kept. Only the C library's environment is changed: a language
runtime's own copy of it, such as Python's ``os.environ``, still holds
them (the PMIx Python bindings take them out of that copy too).

A server takes none of them out: the host's environment is the host's.
The server adds the variables above to each client's environment one
client at a time, and does not pass its own environment on. The one
place the library does - a process it starts itself, with no host to
start it (``PMIx_Spawn`` from a launcher, or a server whose host has no
``spawn`` up-call) - it passes its environment on less these variables,
and gives the process its own.

A process started with none of these is a **singleton**. It looks for a
system-level server on its node (the ``pmix.sys.<host>`` rendezvous
file) and, if it finds one, connects to it; otherwise it runs without a
server.


3. Opening a connection
-----------------------

A client connects inside ``PMIx_Init``, and a tool inside
``PMIx_tool_init``, on the caller's thread. Each blocks until the
connection is complete or has failed. A tool attaching to another
server later (``PMIx_tool_attach_to_server``) connects from the
progress thread, without blocking it.

A client takes the local socket's URI from its environment (or a URI
from a ``PMIX_SERVER_URI`` directive to ``PMIx_Init``). A tool finds its
server as described in :doc:`ptl`; a rendezvous file of this release
gives it the local socket. From there the two do the same:

#. **Open a connection** to the URI's address. For TCP, try each
   alternate address if that fails. Each attempt is bounded by
   ``ptl_base_handshake_wait_time`` (default 60 seconds). A process given
   the local socket connects there and nowhere else: if it cannot reach
   the socket, it fails to initialize, and says why.
#. **On the local socket, check the server** (below).
#. **Send the connection request.**
#. **Read the replies**, each also bounded by
   ``ptl_base_handshake_wait_time``.
#. **Arm the connection** for normal traffic.

Checking the server
^^^^^^^^^^^^^^^^^^^

On the local socket the kernel names the server as well: the uid and
gid of the process listening there. A process checks them before it
sends its request:

* **A process the server started** was told who the server is, in
  ``PMIX_SERVER_LOCAL_URI``. The kernel's uid and gid must equal those.
  A process in a user namespace of its own - a rootless container, say -
  is shown the server's ids as its namespace maps them; it maps them back
  to its parent namespace's, through ``/proc/self/uid_map`` and
  ``gid_map``, before comparing. A server whose ids its namespace does
  not map cannot be checked, and is refused.
* **Any other process** - a tool that found the server through a
  rendezvous file, say - talks to a server running as its own user or as
  root, or as a user or group the ``ptl_base_server_access`` parameter
  names (``users=<list>,groups=<list>``). The gid checked is the
  server's effective gid.

A server that fails the check is not talked to. The process reports the
uid and gid it found and what it expected.

Over TCP, the kernel names the owner of the server's end of the
connection when the server is on the same host, from its table of TCP
connections - a user, but no group. A process applies the same rule by
user alone: a server running as its own user, as root, or as a user
``ptl_base_server_access`` names. A server at an address of this host
that the table does not show is refused. One at any other address - on
another host, or in another network namespace - is not checked, nor is
any on a platform with no table to ask.

The connection request
^^^^^^^^^^^^^^^^^^^^^^

The request is a message header followed by these fields, in this
order. **The order is a wire-format contract**: a server and its peers
can be different PMIx releases, so fields are only ever appended.

.. list-table::
   :header-rows: 1
   :widths: 6 30 64

   * - #
     - Field
     - Content
   * - 1
     - security mechanism
     - name of the ``psec`` component the peer uses
   * - 2
     - credential length
     -
   * - 3
     - credential
     - created by that component (see below)
   * - 4
     - kind of peer
     - one byte: see the table below
   * - 5
     - identity
     - depends on the kind: some or all of uid, gid, namespace, rank
   * - 6
     - version
     - the peer's PMIx version
   * - 7
     - wire format
     - name of the ``bfrops`` component the peer uses
   * - 8
     - buffer type
     -
   * - 9
     - datastore
     - name of the ``gds`` component the peer uses
   * - 10
     - information
     - packed ``pmix_info_t`` array; everything to the end of the message

The kinds of peer:

.. list-table::
   :header-rows: 1
   :widths: 6 26 36 32

   * - Value
     - Kind
     - Sent by
     - Identity sent
   * - 0
     - simple client
     - a client started by the server
     - namespace, rank
   * - 1, 2
     - legacy tool, legacy launcher
     - tools and launchers of old releases only
     - uid, gid
   * - 3
     - tool needing an identifier
     - a tool that has no namespace or rank of its own
     - uid, gid
   * - 4
     - tool given an identifier
     - a tool given ``PMIX_TOOL_NSPACE`` and ``PMIX_TOOL_RANK``; also a
       server connecting to another server
     - uid, gid, namespace, rank
   * - 5
     - tool started by a server
     - a tool whose environment names it (``PMIX_NAMESPACE``,
       ``PMIX_RANK``)
     - uid, gid, namespace, rank
   * - 6, 7, 8
     - launcher needing / given / started with an identifier
     - as 3, 4, 5, for a launcher
     - as 3, 4, 5
   * - 9
     - singleton
     - a client with no server of its own, connecting to a system
       server
     - uid, gid, namespace, rank
   * - 10
     - scheduler
     - a scheduler
     - uid, gid, namespace, rank

The information field carries, from a client, its pid
(``PMIX_PROC_PID``, which the client's PMIx library obtains with
``getpid()``) and command line (``PMIX_CMD_LINE``). From a tool it
carries its pid, real and effective uid and gid, its roles
(``PMIX_LAUNCHER``, ``PMIX_SERVER_SCHEDULER``,
``PMIX_SERVER_SYS_CONTROLLER``), its command line, and any directives it
was given that are meant for the server.

The credential
^^^^^^^^^^^^^^

Each ``psec`` component makes its own credential:

* **native** sends the peer's effective uid and gid. The server does
  not take them on trust: see :doc:`authentication/native`.
* **munge** sends a credential encoded by the MUNGE daemon.
* **ssl** sends the peer's uid, gid, a timestamp, a random value, its
  certificate chain, and a signature over all of them.

The replies
^^^^^^^^^^^

A **client** reads, in order:

#. a status: success, or a request to continue with a security
   exchange;
#. if requested, the client side of that exchange (no component of
   this release requests one);
#. its index in the server's table of peers.

A **tool** reads, in order:

#. if requested, the security exchange;
#. a status - the host's decision;
#. if it asked for one, its assigned namespace and rank;
#. the server's namespace and rank;
#. a final status.

Any status other than success ends the connection, and the peer
reports it. A connection the server closes without sending a status is
reported as ``PMIX_ERR_UNREACH``.

Once the replies are in, the peer marks itself connected, switches the
socket to non-blocking, and arms it for normal traffic. A client then
asks the server for its job information, completing ``PMIx_Init``.


4. Accepting a connection
-------------------------

Everything in this section runs on the server's progress thread.

Reading the request
^^^^^^^^^^^^^^^^^^^

The connection handler reads the request as it arrives, without
waiting: each time the socket is readable it takes what is there and
keeps its place. A request larger than ``PMIX_MAX_CRED_SIZE`` (128 KB)
is refused. Once the whole request is in, the connection leaves the
pending list and its timer stops.

The handler then reads the fields in order. Two checks apply to every
peer:

* **Kind of peer.** An unknown value is refused.
* **Version.** A peer from PMIx v1 or v2.0 is refused with
  ``PMIX_ERR_OUTDATED``. See :ref:`label-version-numbers` for the
  supported floors. A peer of any release before v7.0 is refused with
  ``PMIX_ERR_NOT_SUPPORTED`` unless the server takes legacy peers, and on
  the local socket whether it does or not: no earlier release has that
  transport.

Before the security component is asked, a peer that connected over TCP
from an address that is not this host's - not a loopback address, and
not the address of one of its interfaces - must name a component that
validates a credential made on another host: ``munge`` or ``ssl``. One
that names ``native`` is refused with ``PMIX_ERR_NO_PERMISSIONS``, as is
any such peer in a PMIx configured without ``--enable-remote-tools``.

The rest depends on whether the peer is a client (kinds 0 and 9) or a
tool (all the others).

Checks for a client
^^^^^^^^^^^^^^^^^^^

#. **The client must be registered.** Its namespace, and its rank
   within it, must have been registered by the host
   (``PMIx_server_register_client``). An unregistered client is
   refused.
#. **Its components must be available.** The security, wire-format,
   and datastore components it names must be ones this server has
   loaded.
#. **Its identity must be the registered one.** The security component
   settles the client's uid and gid (see `Settling the identity`_),
   and they must equal the uid and gid the host registered for that
   rank.
#. **It must present its rank's token.** A rank the host registers
   with ``PMIx_server_register_client`` gets a token nobody is handed;
   preparing a process for it with ``PMIx_server_setup_fork`` replaces it
   with the one that process is given, and preparing it again replaces
   that. The server keeps it until the rank or its job is deregistered. A
   client of this release that names the rank must present it, on
   whichever socket it comes, or is refused with
   ``PMIX_ERR_INVALID_CRED`` before the host is asked - so until the
   rank is prepared, no client is taken for it. A rank neither
   registered nor prepared has none, and nothing is asked of it; nor is
   anything asked of the singleton a server was started for
   (``PMIX_SINGLETON``), which no host prepared. A client of an earlier
   release presents none, and is not asked for one: with legacy peers, a
   connection over TCP that declares an earlier release is taken without
   a token.
#. **Its wire format must match its job's.** All clients of one job use
   one wire format, set by the first to connect.

A ``PMIX_USERID`` or ``PMIX_GRPID`` in a client's information field is
ignored. The identity the security component settles is the only one
the server uses.

Checks for a tool
^^^^^^^^^^^^^^^^^

#. **Its security component must be available.**
#. **Its identity is settled** by the security component (see
   `Settling the identity`_). The settled uid and gid replace whatever
   the tool claimed.
#. **A tool of another user is refused** unless the host started the
   server with ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``.
#. **A tool that names an identity must be entitled to it:**

   - a tool started by a server (kind 5 or 8) whose rank is already
     known must have that rank's uid and gid;
   - a tool of this release that names a rank with a token must present
     it, as a client must;
   - a tool joining a namespace the server already knows must have
     that namespace's uid and gid, and the namespace must belong to a
     tool, or to a process this server started.

#. **Its wire format must be one this server accepts.**

A tool the host has not given an identity - a self-started tool with no
namespace - gets a new namespace, created by the server and named by
the host.

Settling the identity
^^^^^^^^^^^^^^^^^^^^^

The security component the peer named settles its uid and gid. How
depends on the component:

* **native**, on the local socket, takes the uid and gid the kernel
  reports for the connection. Over TCP it asks the server's own kernel
  who owns the peer's end of the connection, which gives the uid; a peer
  at an address of this host that the table does not show is refused. A
  peer on another node never reaches it (see above). On a platform with
  no table to ask, a TCP peer is accepted on its claim only while
  ``psec_native_legacy_auth`` is ``true``. See
  :doc:`authentication/native`.
* **munge** decodes the credential through the MUNGE daemon. The
  decoded uid and gid must equal the peer's registered (client) or
  claimed (tool) identity.
* **ssl** verifies the certificate chain against the configured
  certificate authority, checks the signature and the timestamp, and
  checks that the certificate names a local account with the claimed
  uid, which holds the claimed gid. The random value may not be reused.

The result is one uid and one gid. They are what the host is given, and
what every later access decision for this peer uses.

Where settling the identity takes an exchange with the peer, the server
drives it as a small state machine per connection: each step runs when
the peer's socket becomes readable, and a time limit ends the exchange.
While one connection waits, the server serves everyone else.

Calling the host
^^^^^^^^^^^^^^^^

When every check has passed, the server asks the host. The peer waits.

**A client** goes to
:ref:`client_connected2 <man5-pmix_server_module_t-client_connected2>`
(or, if that is ``NULL``, the deprecated ``client_connected``). The host
receives:

.. list-table::
   :header-rows: 1
   :widths: 28 40 32

   * - Item
     - Content
     - Comes from
   * - ``proc`` argument
     - the client's namespace and rank
     - the host's registration; the client's identity was checked
       against it
   * - ``server_object`` argument
     - the pointer the host gave when it registered the client
     - the host
   * - ``PMIX_USERID``, ``PMIX_GRPID``
     - the settled uid and gid
     - the security component
   * - ``PMIX_CRED_TYPE``
     - the name of the security component that settled them
     - the server
   * - ``PMIX_PROC_PID``
     - the client's process ID
     - the client's PMIx library, which calls ``getpid()``. The kernel
       does not report the pid of a TCP peer. A client in a container
       with its own process-ID namespace sends its pid within that
       namespace, which is not the pid the host sees.
   * - ``PMIX_REALUID``, ``PMIX_REALGID``
     - the client's real uid and gid, if it sent them
     - the client's PMIx library
   * - ``PMIX_TIMEOUT``
     - the seconds the host is asked to answer within
     - the server's ``ptl_base_handshake_wait_time``, less what the peer
       has waited for this reply already; absent when that is 0

**A tool** goes to
:ref:`tool_connected2 <man5-pmix_server_module_t-tool_connected2>` (or
the deprecated ``tool_connected``). The host receives the tool's whole
information field, with these set by the server:

.. list-table::
   :header-rows: 1
   :widths: 28 40 32

   * - Item
     - Content
     - Comes from
   * - ``PMIX_USERID``, ``PMIX_GRPID``
     - the settled uid and gid
     - the security component
   * - ``PMIX_CRED_TYPE``
     - the name of the security component that settled them
     - the server
   * - ``PMIX_NSPACE``, ``PMIX_RANK``
     - the identity the tool gave, if any
     - the tool; checked as above where the server already knows it
   * - ``PMIX_VERSION_INFO``
     - the tool's PMIx version
     - the tool's PMIx library, in its connection request
   * - ``PMIX_TIMEOUT``
     - the seconds the host is asked to answer within
     - the server's ``ptl_base_handshake_wait_time``, less what the peer
       has waited for this reply already; absent when that is 0

The rest is passed on as the tool sent it: ``PMIX_PROC_PID``,
``PMIX_REALUID`` and ``PMIX_REALGID``, which the tool's PMIx library
obtains from the operating system; ``PMIX_CMD_LINE``; and the roles the
tool asked for.

If the host provides neither connection up-call, a client is accepted,
and so is a tool that brought its own identity. A tool that needs an
identity assigned is refused.

**The host decides on one tool of a name at a time.** A tool that
brings its own namespace and rank waits in a queue, in the order the
tools' identities were settled, while the host decides on a tool of
that name - or while one of that name is not yet done with after a late
answer (see `When the host's answer does not stand`_). So the host never
decides on two tools for one name at once:

* where a tool belongs - the namespace and rank it joins, and whether
  that rank already has a connection - is checked when its turn comes,
  so it sees every tool accepted ahead of it;
* when the host accepts a tool, any queued tool that names the same
  namespace and rank is refused with ``PMIX_ERR_EXISTS`` at once, and
  any that names the same namespace as another user is refused with
  ``PMIX_ERR_NOT_SUPPORTED``. The host is not asked about either;
* a queued tool that has closed its connection by its turn - its own
  connect timed out, say - is dropped without asking the host;
* when the server finalizes, the tools still queued are refused with
  ``PMIX_ERR_UNREACH``.

Tools of other names, and tools that need an identity - which have no
name until the host gives them one - are put to the host together. The
host keeps the names it gives unique; it cannot know of a name a tool
took for itself until it is asked. A tool the host names while a tool
that took that name is still being decided on is checked again when it
is accepted: whichever is accepted second is refused with
``PMIX_ERR_EXISTS``, and a tool given a rank in a namespace that
appeared while the host decided - another tool's, accepted meanwhile -
joins it only if every process in it is the tool's user.

A slow answer therefore delays the tools of that name behind it, and
their time in the queue counts against their own wait.

What the host must do
^^^^^^^^^^^^^^^^^^^^^

* **Decide.** Check that the client is a process you started, with the
  pid given in ``PMIX_PROC_PID``; check that the tool's user may use
  this server, in the roles it asked for. See :doc:`/hosts/connections`.
* **Record the identity.** Keep the ``PMIX_USERID`` and ``PMIX_GRPID``
  you were given. They are the identity for every later request from
  this peer, and the library passes them again on every up-call it
  makes for it. See :doc:`/hosts/authorization`.
* **Answer exactly once.** Either return an error, or return
  ``PMIX_SUCCESS`` and later call the callback, once, from any thread.
  Never both. ``client_connected2`` may instead return
  ``PMIX_OPERATION_SUCCEEDED`` to accept at once without calling the
  callback. ``tool_connected2`` may not, because the tool's assigned
  identity can only travel through the callback.
* **Answer within** ``PMIX_TIMEOUT``. The peer is waiting, bounded by
  its own ``ptl_base_handshake_wait_time``, and tools of its name behind
  it in the queue wait too. ``PMIX_TIMEOUT`` is what the peer has left
  of that wait; a peer with none left is refused with
  ``PMIX_ERR_TIMEOUT`` without the host being asked. The up-call itself runs on the progress thread, so it
  must not block. A host that cannot decide in time should answer no.
* **Handle** ``PMIX_ERR_LOST_CONNECTION`` **for a process it accepted.**
  See `When the host's answer does not stand`_.

When the host's answer does not stand
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The server waits for the host's answer for ``PMIX_TIMEOUT`` and two
seconds more. Then it refuses the peer with ``PMIX_ERR_TIMEOUT``, and
keeps only what it needs for the answer, which the host still owes: the
host holds the callback's ``cbdata``. Until the answer comes, the peer's
name stays held - a client's rank is refused to any other connection,
and a tool of the same name stays in the queue - so that what follows
cannot name a process accepted since.

A yes the server cannot act on is followed by a
``PMIX_ERR_LOST_CONNECTION`` event naming the process, delivered to the
event handlers the host registered in its own process, as for any
process that leaves without finalizing. That happens when:

* the yes comes after the server stopped waiting;
* the peer has gone by the time the server replies to it;
* the host deregistered the client, its job, or the tool's rank while it
  decided;
* the peer's wire format no longer fits its job;
* the server could not record the connection.

The name stays held until the event has been given to the host's
handlers, as it is while the server waits for the answer, so nothing
accepted under the name meanwhile can be the process the event names.
The event is not raised when a live connection has the name by then:
it would name that connection. A host with no handler for the event is
not told, as for any lost connection.

A no, early or late, is followed by nothing.

Replying to the peer
^^^^^^^^^^^^^^^^^^^^

When the host accepts, the server:

#. adds the peer to its table of peers;
#. sends the remaining replies (see `The replies`_);
#. makes the socket non-blocking and arms it for normal traffic;
#. sends the peer any event notifications cached for it.

When the host refuses, the server sends the host's status to the peer
and closes the connection.

One connection per rank
^^^^^^^^^^^^^^^^^^^^^^^

A rank has at most one connection at a time. A process a client starts
once it has initialized inherits none of its identity (see `2. Telling
a client where to connect`_), and calls ``PMIx_Init`` as a singleton. One
that does have it - started before the client initialized, or given it
by the host or a job script - and connects as the same rank is a
*clone*, in the Standard's terms. The server does not accept clones.

* A client connection for a rank that already has a connection - live
  (not finalized, socket open) or waiting on the host's answer - is
  refused with ``PMIX_ERR_EXISTS``, before the host is asked.
* A tool joining a rank that already has a connection is refused the
  same way - checked before its wire format, so a tool is told its rank
  is taken rather than that its format does not fit the namespace. A
  tool that took its namespace and rank from its environment - one
  started from a job script, say, which inherits the script's - then
  connects again as a tool that needs an identity, and the host gives it
  a namespace of its own; if that connection fails too, the tool is told
  ``PMIX_ERR_EXISTS``. Tools of earlier releases do not connect again:
  their ``PMIx_tool_init`` returns ``PMIX_ERR_EXISTS``. A tool connects
  again only while ``PMIx_tool_init`` runs: a
  tool that attaches to a server later keeps the identity it was
  initialized with, and so does a tool whose caller gave it one
  (``PMIX_TOOL_NSPACE`` and ``PMIX_TOOL_RANK``).
* The first process to connect holds the rank. A tool that takes a job
  script's rank before the script's application connects leaves the
  application refused: the server cannot tell such a tool from one that
  is the script's only PMIx process, so the script must start its tools
  without the script's ``PMIX_NAMESPACE`` and ``PMIX_RANK``, or with an
  identity of their own.
* A tool with the server's address in its environment but no namespace
  and rank asks the host for an identity. A tool of an earlier release -
  v7.0.0rc1 among them - in that position says instead that it is a tool
  on a client's rank, and sends an empty name: the server takes it as a
  tool that needs an identity, and the host names it, but the name is
  not sent to the tool, which reads none. Any other connection that names
  itself with an empty namespace or an invalid rank is refused with
  ``PMIX_ERR_BAD_PARAM``: an empty namespace would match every namespace
  where names are compared.
* A rank whose earlier connection finalized, or was lost, can connect
  again.

Every socket the library creates or accepts is close-on-exec, so a
process's connection closes when it execs - on the local socket and over
TCP alike, for a client and a tool. The server handles that as it handles
any connection that ends without a finalize: the rank is lost at once
(see `When a connection is lost`_), whether the process runs on or not.
The program the process execs is a new process to the server.

The server never compares a new connection's pid with the pid of the
rank's live connection: a second connection for a rank that has one is
refused with ``PMIX_ERR_EXISTS`` whatever pid it comes from.

Libraries of earlier releases leave their connection open across an
exec. The program such a process execs inherits the socket, and cannot
use it: the rank keeps its connection until that program exits, and if
the program calls ``PMIx_Init`` or ``PMIx_tool_init`` for the rank, it
is refused with ``PMIX_ERR_EXISTS`` meanwhile. A process of this release
whose server is of an earlier release closes its connection when it
execs, as it does with any server, and that server handles the
connection as lost.

A rank's process that died can connect again - restarted by the host,
say - before the server has read its old connection's close. Before
refusing, the server looks at the old socket: one that has closed in
order or been reset is handled as a lost connection there and then, and
the new connection goes ahead. One with unread data on it, or one that
reports any other error, is live.

``PMIX_ALL_CLONES_PARTICIPATE``, which the Standard defines for
collectives, has no effect: a rank has one process.


5. Ending a connection
----------------------

A connection ends in one of five ways. The server must handle each one
completely: release the peer, account for it in every operation it was
part of, and let the host know.

When a client finalizes
^^^^^^^^^^^^^^^^^^^^^^^

On the client, ``PMIx_Finalize``:

#. returns ``PMIX_ERR_WOULD_BLOCK`` if called from the progress thread;
#. if the client was started with ``PMIX_EMBED_BARRIER``, joins a fence
   of its job;
#. sends ``PMIX_FINALIZE_CMD`` to the server and waits for the reply, at
   most ``pmix_server_client_fintime`` seconds (the
   ``finalize_timeout`` MCA parameter, default 10);
#. stops its progress thread, closes the connection, and releases its
   state.

On the server, ``PMIX_FINALIZE_CMD``:

#. removes the client's event registrations and output-forwarding
   requests, fails requests waiting on its data, and drops cached
   notifications addressed to it;
#. calls the host's
   :ref:`client_finalized <man5-pmix_server_module_t-client_finalized>`;
#. once the host calls back, marks the peer finalized and replies.

When the client's connection then closes, the server releases the
connection. It keeps the rank's entry - a *tombstone* - so that the
same rank can initialize again later and be recognized. See
:doc:`init-finalize`.

A finalized rank remains a member of its job, because the process may
call ``PMIx_Init`` again and rejoin. An MPI Sessions program does this
once per session, and a fast rank can open the next session's fence
before a slow one has finished the last. So a collective that names a
finalized rank keeps waiting for it to come back and contribute. See
:doc:`collectives/tracking_spec`, "A graceful finalize is not a
departure".

The server cannot tell a rank that will return from one that will not.
The host can: deregistering the client (see `When the host ends it`_)
tells the server the process is gone for good. Where the host does not
deregister, a collective whose participant finalized and never returns
waits until its ``PMIX_TIMEOUT``, or indefinitely if it has none.

What the host must do:

* release what it holds for that process;
* call the callback, which releases the client. The client waits only
  up to its own timeout;
* not deregister the client from inside the up-call before calling the
  callback.

When a tool finalizes or disconnects
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``PMIx_tool_finalize`` sends ``PMIX_FINALIZE_CMD`` to the tool's
primary server, waits for the reply, and closes all its connections. The
primary server handles a tool's finalize as it does a client's, and
calls ``client_finalized`` for it too; ``server_object`` is then
``NULL``. (The capability flag ``PMIX_CAP_TOOL_FINALIZED`` says the
library does this.) Any other server sees its connection close, and
handles it as a lost connection (see `When a connection is lost`_). That
is deliberate: for a tool, whether it exited or crashed makes no
difference to a server.

``PMIx_tool_disconnect`` ends the connection to one server, without a
message. It is not a finalize, but the server cannot tell it from one
that failed, and handles it as a lost connection. Requests the tool
still had outstanding with that server complete with
``PMIX_ERR_UNREACH``.

The server treats a disconnected tool as gone, because once a tool has
disconnected neither the server nor the host can tell when it later
ends: if its state were kept for a return, it would never be released.
So a namespace the server created for a self-started tool is removed
when the last process using it has gone, whether by finalize,
disconnect or failure. A tool that connects again arrives as a new
tool.

.. DRAFT: not done today - a self-started tool's namespace is never
   removed.

When a connection is lost
^^^^^^^^^^^^^^^^^^^^^^^^^

A process that exits, or is killed, without finalizing closes its
connection without notice; so does a tool that disconnects. The server
notices when a read or write on the socket fails - a process that
execs closes its connection the same way (see `One connection per
rank`_). The server:

#. closes the connection and stops its events;
#. accounts for the process in every collective it was part of: a fence,
   connect, disconnect, or group construct still waiting for it
   completes with ``PMIX_ERR_PARTIAL_SUCCESS`` if others remain, or
   ``PMIX_ERR_LOST_CONNECTION`` if not;
#. removes its event registrations and output requests, fails requests
   waiting on its data with ``PMIX_ERR_LOST_CONNECTION``, and stops any
   monitoring of it;
#. raises ``PMIX_ERR_LOST_CONNECTION``, naming the process, with range
   ``PMIX_RANGE_PROC_LOCAL``. It goes to the event handlers the host
   registered in its own process with ``PMIx_Register_event_handler``,
   and to no client or tool.

For a client this is a second notice: the client is the host's child,
and the host sees it end. For a tool the host did not start, the event
is the only notice that it left without finalizing.

A process that hangs without closing its connection is not detected by
the server. The host's own process monitoring covers that case.

What the host must do: register a handler for
``PMIX_ERR_LOST_CONNECTION`` if it accepts tools it did not start, and
release what it holds for such a tool when the event names it (PRRTE
does this); notice a client's death through its own monitoring;
generate the termination events applications expect
(``PMIX_PROC_TERMINATED``, and ``PMIX_ERR_PROC_TERM_WO_SYNC`` for
processes connected to it), and deregister the process when it no
longer needs it.

When the server goes away
^^^^^^^^^^^^^^^^^^^^^^^^^

When a client's or tool's connection to its server fails:

#. every request waiting on that server completes with
   ``PMIX_ERR_UNREACH``;
#. the process raises ``PMIX_ERR_LOST_CONNECTION``, if the server was its
   primary one and it has not begun to finalize;
#. from then on, calls that need the server return
   ``PMIX_ERR_UNREACH``.

There is no automatic reconnection. A tool can attach to another
server.

When the host ends it
^^^^^^^^^^^^^^^^^^^^^

**Deregistering a client**
(:ref:`PMIx_server_deregister_client(3) <man3-PMIx_server_deregister_client>`)
tells the server that a process has ended and will not connect again.
It is the only way the server learns that a finalized rank is gone for
good: finalizing alone leaves the rank a member of its job (see `When a
client finalizes`_).

A host need not call it. Without it, the rank stays a member until its
job is deregistered, and:

* a collective that names a finalized rank that never returns waits
  until its ``PMIX_TIMEOUT``, or indefinitely;
* a request for data that rank never posted, made after it finalized,
  waits instead of being answered.

When to call it: once the process has terminated. Not from
``client_finalized`` - the process may still initialize again. PRRTE
calls it for each local process when the process has terminated: its
exit has been collected and its output drained. Its
``client_finalized`` only records that the process finalized.

What the server does:

#. if the client is still connected, closes the connection and handles
   it as a lost connection, without calling the host back. The host
   deregisters a live client only when it is about to kill it or
   believes it already dead, so the connection may fail on its own while
   this runs; the departure is still handled once;
#. releases the network and GPU resources assigned to the process,
   stops monitoring it, and runs any epilogs registered for it;
#. sends any reply still queued for it - typically the reply to its
   finalize;
#. removes the rank's entry. A later connection by that rank is
   refused, because it is no longer registered;
#. records the rank as departed, so a later request for its data is
   answered ``PMIX_ERR_NOT_FOUND`` at once;
#. accounts for its departure in every active collective that names it
   and has not received its contribution, as for a lost connection;
#. once every local process of the job has finalized or been
   deregistered, tells the network and GPU support that the job's
   local processes are done.

The departure is counted because the rank can never contribute now,
and because a collective's ``PMIX_TIMEOUT`` is optional: without this
step, a job whose rank finalized and exited would hang.

.. DRAFT: step 6 is not done today - deregistration does not reach the
   collective trackers, for a finalized client or a live one.

**Deregistering a job**
(:ref:`PMIx_server_deregister_nspace(3) <man3-PMIx_server_deregister_nspace>`)
deregisters every client in it, removes the job's data, events, and
group memberships, and fails every collective that names it.

**Finalizing the server**
(:ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`) closes every
connection. Clients and tools see their server go away.


Host obligations, in one place
------------------------------

* Register a job and each of its clients before starting them, and call
  ``PMIx_server_setup_fork`` for each client.
* Decide every connection in ``client_connected2`` or
  ``tool_connected2``: check that a client is a process you started,
  with the pid given in ``PMIX_PROC_PID``, and that a tool's user may
  use the server in the roles it asked for.
* Record the ``PMIX_USERID`` and ``PMIX_GRPID`` you are given. They are
  the peer's identity for every later authorization.
* Answer each up-call exactly once, promptly, without blocking - a
  connection up-call within the ``PMIX_TIMEOUT`` it carries.
* Be ready to give an identity to a tool you launched: one whose
  namespace and rank are already in use connects again without one.
* In ``client_finalized``, release your state for the process and call
  the callback. Do not deregister the process there: it may initialize
  again.
* Watch your processes. Generate termination events when they end.
* Register a handler for ``PMIX_ERR_LOST_CONNECTION``: it is how you
  learn that a tool you did not start has gone without finalizing, and
  that a process you accepted never became connected.
* Optionally, deregister each client once its process has terminated.
  Until you do, a rank that finalized is still expected in its job's
  collectives.

A peer that vanishes while the server writes to it costs the server an
error on that connection, never a ``SIGPIPE``: no write PMIx makes to a
connection's socket can raise it, nor the write of its URI to a pipe the
host names (``PMIX_SERVER_REPORT_URI``). A host need not change its own
handling of that signal for connections. (Output forwarding writes pipes
of its own, and is described with it.)
