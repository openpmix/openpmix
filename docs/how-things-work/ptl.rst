
Transport Layer (Client/Server/Tool Connections)
=================================================

This document describes how PMIx connects a process to the PMIx server it
talks to, and how messages then flow between them: how a server publishes
where it can be reached, how a client or tool discovers and connects to
it, how the server establishes who connected, and how framed
messages are sent and received over the socket in steady state. All of
this is the job of the MCA ``ptl`` (PMIx Transport Layer) framework.

The life of a connection, from listener to departure, is described in
:doc:`connections`. This page covers the framework itself, how a tool
finds its server, and how messages travel once connected.

For a code-oriented orientation aimed at contributors working *inside*
the framework, each ``ptl`` directory also carries an ``AGENTS.md`` (with
a ``CLAUDE.md`` symlink): ``src/mca/ptl/AGENTS.md`` for the framework as a
whole, plus one each in ``base/``, ``tcp/``, and ``usock/``. This
document explains how the transport layer fits into the wider library;
those explain each piece's internals in detail.


The Problem
-----------

Every PMIx message — a ``PMIx_Get``, a fence, an event notification,
forwarded I/O — ultimately travels over a socket between a process and
its server. Before any of that can happen, three things must be arranged:

* the server must make itself **findable** — a client that was launched
  under it, and a tool that shows up later, both need a way to learn the
  server's address;
* the two ends must complete a **handshake** that establishes identity,
  negotiates the serialization (``bfrops``) and datastore (``gds``)
  modules they will use, and validates a security credential; and
* once connected, messages must be **framed, matched, and delivered**
  reliably on PMIx's single progress thread without blocking it.

The ``ptl`` framework owns all three. Everything above the socket —
serializing the payload (``bfrops``), the security credential itself
(``psec``), where the data is stored (``gds``) — belongs to other
frameworks; ``ptl`` is the pipe and the rules for setting it up.


Roles, Not Components
---------------------

In a typical framework the component contains the implementation and
``base/`` is just plumbing. In ``ptl`` **the base contains essentially all
of the code** — message framing, the handshake, the listener, rendezvous
files, send/receive, and teardown. What a process uses depends on its
*role*, which is its own process type. When the framework is selected,
``pmix_ptl_base_select`` fills the ``pmix_ptl`` table with the role's
functions:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Role
     - The local peer is…
   * - client
     - a client **and not** a tool. It only connects; it neither listens
       nor forks PMIx children of its own.
   * - tool
     - any tool — debugger, launcher, or client-tool. It connects, and it
       listens.
   * - server
     - a server (and not a tool). It listens, and can connect to another
       server.

They are tested in that order. A **launcher** is ``tool+server`` and is
therefore a tool — which is why a tool, not just a server, is able to set
up a listener.

The transports are components. ``ptl`` is multi-select: every transport
that can run is active, and each opens its own listening socket:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Transport
     - Used by
   * - ``usock``
     - Every client and tool of this release on the server's host. An
       ``AF_UNIX`` socket, whose far end the kernel identifies - its uid,
       gid and pid. Its URI is ``<nspace>.<rank>;usock://<path>``.
   * - ``tcp``
     - Peers of earlier releases, and tools on other hosts. Its URI is
       ``<nspace>.<rank>;tcp4://<host>:<port>`` (``tcp6://`` for IPv6).
       A server listens on it only when it takes legacy peers
       (``ptl_base_legacy_peers``, ``PMIX_SERVER_LEGACY_PEERS``) or
       remote tools (``PMIX_SERVER_REMOTE_CONNECTIONS``).

Both are always built: ``configure`` stops with an error if either is
excluded. Which transport a URI belongs to is the transport that
recognizes its scheme. ``PMIX_MCA_ptl`` selects nothing - it is removed
from the environment at init, and a ``ptl`` selection given any other way
is ignored. Both transports speak the same messages, on the wire as
``PMIX_PROTOCOL_V2``. (``PMIX_PROTOCOL_V1`` was the original Unix-domain
transport of the v1 releases, whose peers can no longer connect.)
Handshake version differences are handled *inside* the base by inspecting
the peer's version string.


Publishing a Server: the Listener
---------------------------------

A server (or launcher, or any tool that will spawn children) makes itself
reachable in ``pmix_ptl_base_setup_listener``
(``src/mca/ptl/base/ptl_base_listener.c``). It has the transport open the
socket - the first two steps below are the ``tcp`` transport's, in
``src/mca/ptl/tcp/ptl_tcp.c`` - and does the rest itself:

#. **Choose an interface.** Enumerate the node's interfaces, filter them
   through any ``if_include`` / ``if_exclude`` directives, and default to
   a **loopback** device. An entry that is neither an interface name nor
   a subnet, or an include list none of whose entries is on this host,
   stops the listener from opening at all. A loopback device's address
   must be a loopback address. A PMIx configured ``--enable-remote-tools``
   falls back to a public interface when the directives leave no loopback
   one and the host did not say no remote connections; one without it
   never does. When remote
   connections are accepted, *every* public interface the directives leave
   is used: the first carries the URI, and each of the others gets a
   listener of its own (on the same port where it is free), so a remote
   tool on any of the host's networks can reach the server.
#. **Bind a listen socket.** Try each configured port (default ``0`` —
   let the kernel assign one), set ``SO_REUSEADDR`` and close-on-exec,
   ``listen`` with ``SOMAXCONN``, and make the socket non-blocking.
#. **Publish the URI.** Build a URI of the form
   ``nspace.rank;tcp4://host:port``. The base stores the server's URI -
   this one when there is a TCP listener, else the local socket's - in
   ``gds`` under both ``PMIX_MYSERVER_URI`` and (for older tools)
   ``PMIX_SERVER_URI``. If
   ``report_uri`` is set, also emit it to stdout (``-``), stderr (``+``),
   an integer pipe fd, or a named file. The other addresses, if any, are
   stored as ``PMIX_MYSERVER_ALT_URIS`` - a comma-delimited list of
   ``tcp4://host:port`` / ``tcp6://host:port`` - and follow the version
   in a report file on a line tagged ``alturis:``. A report file also
   carries the ``bfrops:`` and ``psec:`` lines described below.
#. **Drop rendezvous files.** Depending on the server's role and flags,
   write well-known contact files that a client or tool can later
   discover in the tmpdir tree:

   .. list-table::
      :header-rows: 1
      :widths: 45 55

      * - File
        - Written by
      * - ``pmix.sched.<host>``
        - a scheduler
      * - ``pmix.sysctrlr.<host>``
        - a system controller
      * - ``pmix.sys.<host>``
        - a server offering *system*-level tool support
      * - ``pmix.<host>.tool``
        - a server offering *session*-level tool support
      * - ``pmix.<host>.tool.<pid>``
        - any tool-supporting server (keyed by PID)
      * - ``pmix.<host>.tool.<nspace>``
        - any tool-supporting server (keyed by nspace)

   Each file contains the URI, the server's version, its PID, its
   ``uid:gid``, and a timestamp, and then - only when the server listens
   on more than one address - a line tagged ``alturis:`` listing the
   others. A line tagged ``local:`` carries the server's host name and
   the local socket's URI, as ``local:<host>;<uri>``. Two
   more tagged lines follow: ``bfrops:`` lists the wire
   formats (``bfrops`` components) the server accepts and ``psec:`` its
   security mechanisms, each comma-delimited in the server's priority
   order and limited by its MCA parameters. A line ``lock:flock`` says
   that the server holds a lock (``flock``) for as long as it runs on a
   file of its own beside this one, ``.<name>.lock``, mode 0600, which
   no search for ``pmix.*`` files matches; the kernel lets go of the
   lock however the server exits.
   Readers find every tagged
   line by its tag; older readers stop before them. The ``pmix_ptl_base`` state struct
   records (via its ``created_*`` flags) exactly which files and
   directories this process created, so ``pmix_ptl_close`` can remove
   precisely those at shutdown and nothing that belongs to a peer.

Beside the TCP listener, every server opens its **local socket** - the
``usock`` transport's, in ``src/mca/ptl/usock/ptl_usock.c``:

#. **Make a directory of its own** with ``mkdtemp`` beneath the server's
   temporary directory: ``<tmpdir>/pmix-XXXXXX``, mode 0700.
#. **Bind** ``pmix-<pid>`` inside it, and give the socket its mode -
   0600 unless the host passed ``PMIX_SOCKET_MODE`` - and its owner and
   group (``PMIX_USERID``, ``PMIX_GRPID``; the group is the server's
   effective group unless named) while the directory is still 0700. Only
   then is the directory opened to whom the mode lets connect: group or
   others get search permission, never read. The directory itself stays
   the server's. It is held open from the moment it is made, and every
   change in it is made through that descriptor.
#. **Listen**, non-blocking and close-on-exec.

A server that cannot open its local socket fails ``PMIx_server_init``.
The socket's path must fit an ``AF_UNIX`` address (about 100 characters):
a long temporary directory - the default ``$TMPDIR`` on macOS is one - is
an error, not a reason to fall back. Pass a shorter directory
(``PMIX_SERVER_TMPDIR``). At finalize the server removes the socket and
its directory; a server that is killed leaves them behind.

The local socket's URI reaches a client in its environment, as
``PMIX_SERVER_LOCAL_URI``, and a tool on the rendezvous file's ``local:``
line. ``PMIX_SERVER_URI``, ``PMIX_MYSERVER_URI``, the report and the
first line of the rendezvous files are the TCP URI when the server
listens on TCP - they reach processes on other hosts, and older peers,
which cannot use the local socket - and the local socket's otherwise.
The ``PMIX_SERVER_URIxx`` variables are the TCP URI only when the server
takes legacy peers, which are the processes that read them alone.

``pmix_ptl_base_start_listening`` then registers the listen sockets'
accept events on the progress thread.


Finding and Connecting to a Server
----------------------------------

The outbound side is driven by ``pmix_ptl.connect_to_peer``, called
(blocking) during ``PMIx_Init`` or ``PMIx_tool_init``. There are two
implementations, chosen by the process's role.

Clients: the short path
~~~~~~~~~~~~~~~~~~~~~~~~~

A pure client uses its own ``connect_to_peer``
(``src/mca/ptl/base/ptl_base_client.c``). A client already knows its server,
so the logic is short:

#. use an explicit ``PMIX_SERVER_URI`` from the info array if present;
#. otherwise consult the environment via ``pmix_ptl_base_set_peer``, which
   walks the active ``bfrops`` modules and looks for the corresponding
   ``PMIX_SERVER_URIvNN`` variable — finding one yields both the URI and
   the server's version. If the server also set ``PMIX_SERVER_LOCAL_URI``
   - the local socket, and the uid and gid the server runs as - naming
   the same server, the client connects there and nowhere else: if the
   socket cannot be reached, ``PMIx_Init`` fails and says why. A
   ``PMIX_SERVER_LOCAL_URI`` naming another server is one a server of an
   earlier release left in place, and is not used;
#. if neither exists, the process was not launched under a server: mark it
   a **singleton** and probe for a system server via ``pmix.sys.<host>`` -
   reaching it on its local socket - falling back to standalone operation
   if none is found.

Tools and servers: the discovery matrix
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Tools (and servers connecting upward) use
``pmix_ptl_base_connect_to_peer`` (``src/mca/ptl/base/ptl_base_connect.c``),
which searches for the right server among many possibilities, roughly from
most specific to least:

* an explicit ``PMIX_SERVER_URI`` / ``PMIX_TCP_URI``;
* a rendezvous / ``PMIX_TOOL_ATTACHMENT_FILE``;
* a caller-specified **connection order** — ``PMIX_CONNECT_SYSTEM_FIRST``,
  ``PMIX_CONNECT_TO_SYSTEM``, ``PMIX_CONNECT_TO_SCHEDULER``,
  ``PMIX_CONNECT_TO_SYS_CONTROLLER`` — each mapping to a well-known
  rendezvous filename;
* a specific server PID or nspace; or
* a directory search of the system tmpdir for ``pmix.<host>.tool*``
  files. Every file found must name the same server; files naming
  different ones fail the search, saying so. A file whose server is gone
  is passed over, here and by a ``PMIX_QUERY_AVAIL_SERVERS`` query: it
  says its server holds the lock beside it, and the searching process
  can take that lock - whatever PID namespace, container or jail either
  runs in. A file is kept when it has no such line (from an earlier
  release), names another kind of lock, or has no lock file; when its
  lock file has another name or others could open it; and when the
  searching process cannot open it - only the server's user and root
  can. One larger than any server writes is passed over. A server
  starting under a name whose lock another server holds takes the name
  as in use (``rndz-name-held``), whatever its PID says - except the
  file named for its own PID, which collides only with a server in
  another PID namespace and which it then does not write: a tool here
  that looks for this server by PID finds that one, and checks it as it
  checks any. Otherwise it judges a file at the name by its PID, as
  before. At finalize it removes each of its files only while it is still
  the file it wrote, and its lock file only while it is the one it
  holds.

A tool started by a server has ``PMIX_SERVER_LOCAL_URI`` in its
environment, as a client does. A rendezvous or report file with a
``local:`` line for the tool's own host takes it to the local socket, and
only there. A file without one is an older server's, and one whose
``local:`` line names another host - a report file read elsewhere - is
for a tool on that host: the tool uses the file's TCP URI.

A client's or tool's connection to its server is close-on-exec whichever
transport it is on: a process that execs leaves its connection behind,
and the server handles it as lost.

``PMIX_TOOL_CONNECT_OPTIONAL`` decides whether failing to find a server is
an error or simply leaves the tool unconnected.

Checking the server
~~~~~~~~~~~~~~~~~~~

On the local socket the kernel names the server too, and a client or
tool checks it before it sends anything that matters:

* a process the server started was told, in ``PMIX_SERVER_LOCAL_URI``,
  the uid and gid the server runs as. The kernel's must match both;
* any other process accepts a server running as its own user, as root,
  or as a user or group ``ptl_base_server_access`` names.

A server that fails the check is not talked to, and the process says
whom it found. Over TCP, the kernel's table of TCP connections names the
user - no group - of a server on the same host, and the same rule is
applied by user alone. A server at an address of this host that the table
does not show is refused; one at any other address is on another host,
and proves who it is instead - see "A server on another host" in
:doc:`connections`. A process other than a client has one on its own
host prove who it is the same way when the platform has no table to
ask, unless it knows the server is of an earlier release - see "A server
the kernel cannot name" there.

Choosing what to speak
~~~~~~~~~~~~~~~~~~~~~~

The connect request names one wire format and one security mechanism, and
the server either accepts them or drops the connection; it never says what
it would accept instead. A client is told by the variable its server set
(``PMIX_SERVER_URIvNN``). A tool is started by no server, so it learns from
the server's file:

* if the file has ``bfrops:`` and ``psec:`` lines, the tool uses the
  highest of its own wire formats that the server lists, and keeps its
  security mechanism if the server lists it, or else takes the highest of
  its own that the server does. With nothing in common it does not try to
  connect, and says why;
* a file from a server too old to write those lines has only the server's
  version. The tool then uses the highest of its wire formats named for a
  release no later than the server's. That assumes the server runs every
  format up to its own, which an MCA parameter on the server can make
  untrue, so it is a fallback for older servers only. For the security
  mechanism it uses ``munge`` if it has it, else ``native`` - never
  ``ssl``, which no such server has;
* a URI given directly carries neither, and the tool uses its newest. If
  the server then closes the connection without answering - what a server
  does with a format or mechanism it lacks - the tool suggests giving it
  the server's file instead, as ``file:<path>``.

For a tool's first server the choice becomes the tool's own, as a client's
does. A server it attaches to later gets its own choice, which leaves how
the tool speaks to its other servers unchanged.

A server may be reachable at more than one address: the rendezvous file's
``alturis:`` line, or a ``PMIX_SERVER_ALT_URIS`` directive given with the
URI, lists the rest. If the URI's address cannot be connected to, the tool
tries each of those in order; a failed handshake, by contrast, is the
server's answer and is not retried elsewhere.

Why the other addresses are not simply in the URI
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Clients and tools of one release must be able to connect to servers of
another, and every released parser refuses a URI that carries more than
one address - a client refused that way even runs as a singleton rather
than reporting an error. So the URI keeps exactly its historical form
everywhere it appears (stored values, environment variables, and the first
line of every contact file), and the additional addresses travel only where
no older reader looks: after the five lines a contact file has always held,
which every release reads by position, and in keys older libraries do not
know. An older tool therefore connects exactly as it always has, and a
newer tool facing an older server simply finds no alternates.

Both paths converge on ``pmix_ptl_base_make_connection``, which parses the
URI into a socket address, opens the connection (with retries), sends the
connect-ack, runs the handshake, and — on success —
``pmix_ptl_base_complete_connection`` records the server and arms the
steady-state send/receive events.


The Handshake
-------------

The connection request, the checks the server makes, the security
exchange, the call to the host, and the replies are described in
:doc:`connections`.


Messages in Steady State
------------------------

Once connected, all traffic is framed and runs on the progress thread
(``src/mca/ptl/base/ptl_base_sendrecv.c``).

Framing
~~~~~~~

Every message is a fixed header followed by its payload::

    struct pmix_ptl_hdr_t {
        int32_t  pindex;   /* sender's peer-array index          */
        uint32_t tag;      /* matches a send to its posted recv  */
        uint32_t nbytes;   /* payload length after the header    */
    };

Header integers travel in network byte order — this is the one place
``ptl`` must be endian-correct on the wire; the payload itself is handled
by ``bfrops``.

Tags route a message to the right handler. Low reserved values name
persistent channels (``PMIX_PTL_TAG_NOTIFY`` = 0,
``PMIX_PTL_TAG_HEARTBEAT`` = 1, ``PMIX_PTL_TAG_IOF`` = 2); dynamic tags
start at ``PMIX_PTL_TAG_DYNAMIC`` (100) and are handed out one per
request/response exchange. To keep the two ends of a bidirectional socket
from ever choosing the same dynamic tag, the dynamic-tag space is **split
in half**: the side that accepted the connection uses the upper half, the
side that initiated it uses the lower half. ``UINT_MAX`` is a
wildcard/"any tag" recv.

Sending and receiving
~~~~~~~~~~~~~~~~~~~~~~~

Back-end code never calls ``send()``/``recv()`` directly. It uses macros
from ``ptl_types.h`` that each allocate a small caddy, retain the peer,
and thread-shift onto the progress thread:

* ``PMIX_PTL_SEND_RECV`` — a two-way exchange: posts a one-shot recv on the
  next dynamic tag, sends the request, and delivers the reply to a
  callback;
* ``PMIX_PTL_SEND_ONEWAY`` — fire-and-forget on a fixed tag;
* ``PMIX_SERVER_QUEUE_REPLY`` — the server's reply path, queued directly
  from within the progress thread.

On the wire side, ``pmix_ptl_base_send_handler`` ``writev``\ s the header
and payload together when the socket is writable, resuming across partial
writes and ``EAGAIN``; ``pmix_ptl_base_recv_handler`` reads the header,
bounds-checks ``nbytes`` against the ``max_msg_size`` MCA parameter,
allocates and reads the payload, then posts the completed message to
``pmix_ptl_base_process_msg``. That function walks the posted-recv list,
matches on ``(peer, tag)``, and fires the recv's callback; a dynamic-tag
recv is one-shot and is removed after it fires. A message that matches no
posted recv is, by design, an error — this subsystem never receives
anything it did not previously ask for.

A send to oneself (``peer == pmix_globals.mypeer``) skips the socket
entirely and is matched locally — this is how a server delivers messages
to itself.

Losing a connection
~~~~~~~~~~~~~~~~~~~~~

How a connection ends - by finalize, disconnect, a process dying, or
the server going away - is described in :doc:`connections`.


Threading Model
---------------

The regimes that coexist:

* **Init-time connection is synchronous.** ``connect_to_peer`` runs in the
  caller's thread during ``PMIx_Init`` / ``PMIx_tool_init``, performs
  *blocking* socket I/O, and returns only when the handshake is complete.
  Each ``connect()`` attempt and each reply is bounded by
  ``ptl_base_handshake_wait_time``.
* **Connecting from the progress thread is event-driven.**
  ``PMIx_tool_attach_to_server`` - and a tool or server attaching to
  another server at init - does its work in a thread-shift handler, on the
  progress thread. There it uses ``connect_to_peer_nb``: the server is
  located as before, but the connection is a non-blocking ``connect()``
  followed by the same handshake, each reply collected as it arrives. The
  progress thread keeps serving everything else in between, and one timer,
  ``ptl_base_handshake_wait_time``, bounds the whole attach.
* **The server's side of the handshake does not wait either.** It runs
  on the server's progress thread, whenever anyone connects, while that thread
  is serving every other client. So the server reads an incoming
  handshake as it arrives and never waits for it. The replies that follow
  are a few words written to the peer; the server reads nothing more
  from it until the connection is accepted.
* **Steady-state I/O is entirely on the progress thread.** Every
  ``PMIX_PTL_SEND_*`` macro thread-shifts; the send/receive handlers,
  message matching, the accept handler, and the connection handler all run
  on ``pmix_globals.evbase``. Shared state — the posted-recv list, a
  peer's send/receive queues — is touched only there.

The various caddies (``pmix_ptl_sr_t``, ``pmix_ptl_queue_t``,
``pmix_pending_connection_t``, and the connection handler's own object)
each carry the mandatory ``pmix_event_t`` and retain the peer so it
outlives the asynchronous hop, exactly as PMIx's thread-safety rules
require.


Configuration
-------------

The framework's behavior is tunable through MCA parameters registered in
``ptl_base_frame.c`` (all under the ``pmix_ptl_base_`` prefix, most with
deprecated ``pmix_ptl_tcp_`` synonyms): ``max_msg_size``,
``if_include`` / ``if_exclude``, ``ipv4_ports`` / ``ipv6_ports``,
``disable_ipv4_family`` / ``disable_ipv6_family``,
``connection_wait_time`` / ``max_retries`` (how long and how often to wait
for a server's rendezvous file to appear), ``handshake_wait_time`` /
``handshake_max_retries`` (how long a client or tool waits on a server
while connecting to it, and how often it retries a handshake the server
asks it to retry; the wait defaults to 60 seconds, and 0 removes the
bound),
``server_access`` (servers a client or tool will
talk to over the local socket besides one of its own user or root, as
``users=<list>,groups=<list>``, each list user or group names or numbers
separated by ``:``; a name the system does not know, an empty list, or
a directive given twice fails init),
``connect_ack_timeout`` (how long a server gives an incoming connection to
deliver its whole connection request; default 5 seconds, 0 for no
limit),
``max_pending_connections`` (how many incoming connections may be waiting
to deliver their connection request at once, and, separately, how many
tools may wait for the host's decision; at the first limit the oldest
connection still delivering its request is dropped, and at the second a
further tool is refused; default an eighth of the descriptor limit for
each, -1 for no limit), and
``report_uri``. Inspect the current values
for a build with::

    pmix_info --param ptl base
