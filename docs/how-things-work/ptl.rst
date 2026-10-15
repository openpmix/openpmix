
Transport Layer (Client/Server/Tool Connections)
=================================================

This document describes how PMIx connects a process to the PMIx server it
talks to, and how messages then flow between them: how a server publishes
where it can be reached, how a client or tool discovers and connects to
it, the identity/security handshake both sides run, and how framed
messages are sent and received over the socket in steady state. All of
this is the job of the MCA ``ptl`` (PMIx Transport Layer) framework.

The life of a connection, from listener to departure, is described in
:doc:`connections`. This page covers the framework itself, how a tool
finds its server, and how messages travel once connected.

For a code-oriented orientation aimed at contributors working *inside*
the framework, each ``ptl`` directory also carries an ``AGENTS.md`` (with
a ``CLAUDE.md`` symlink): ``src/mca/ptl/AGENTS.md`` for the framework as a
whole, plus one each in ``client/``, ``server/``, and ``tool/``. This
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


An Unusual Framework Shape
--------------------------

``ptl`` is a **single-select** framework — exactly one component is active
in a given process — but it is structured differently from most PMIx
frameworks, and understanding that difference is the key to reading the
code.

In a typical framework the component contains the implementation and
``base/`` is just plumbing. In ``ptl`` it is inverted: **the base contains
essentially all of the code** — TCP sockets, message framing, the
handshake, the listener, interface discovery, rendezvous files,
send/receive, and teardown — while the three components (``client``,
``server``, ``tool``) are thin *role selectors*. Each component's
``component_query`` inspects the local process type and claims the process
only if it matches:

.. list-table::
   :header-rows: 1
   :widths: 15 15 70

   * - Component
     - Priority
     - Selected when the local peer is…
   * - ``client``
     - 50
     - a client **and not** a tool (a "pure" client)
   * - ``tool``
     - 40
     - any tool — debugger, launcher, or client-tool
   * - ``server``
     - 30
     - a server **and not** a tool (a "pure" server)

These conditions are mutually exclusive, so at most one component ever
returns a module and the priorities never actually break a tie. A
**launcher** is ``tool+server`` and therefore uses the ``tool`` component
— which is why a tool, not just a server, is able to set up a listener.

There is only one transport today: TCP, identified on the wire as
``PMIX_PROTOCOL_V2``. The older Unix-domain-socket transport
(``PMIX_PROTOCOL_V1``, "usock") has been removed. Handshake version
differences are handled *inside* the base by inspecting the peer's version
string, not by swapping components.


Publishing a Server: the Listener
---------------------------------

A server (or launcher, or any tool that will spawn children) makes itself
reachable in ``pmix_ptl_base_setup_listener``
(``src/mca/ptl/base/ptl_base_listener.c``):

#. **Choose an interface.** Enumerate the node's interfaces, filter them
   through any ``if_include`` / ``if_exclude`` directives, and default to
   a **loopback** device. Only if remote or tool connections were
   requested does it fall back to a public interface. When remote
   connections are accepted, *every* public interface the directives leave
   is used: the first carries the URI, and each of the others gets a
   listener of its own (on the same port where it is free), so a remote
   tool on any of the host's networks can reach the server.
#. **Bind a listen socket.** Try each configured port (default ``0`` —
   let the kernel assign one), set ``SO_REUSEADDR`` and close-on-exec,
   ``listen`` with ``SOMAXCONN``, and make the socket non-blocking.
#. **Publish the URI.** Build a URI of the form
   ``nspace.rank;tcp4://host:port`` and store it in ``gds`` under both
   ``PMIX_MYSERVER_URI`` and (for older tools) ``PMIX_SERVER_URI``. If
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
   others. Two more tagged lines follow: ``bfrops:`` lists the wire
   formats (``bfrops`` components) the server accepts and ``psec:`` its
   security mechanisms, each comma-delimited in the server's priority
   order and limited by its MCA parameters. Readers find every tagged
   line by its tag; older readers stop before them. The ``pmix_ptl_base`` state struct
   records (via its ``created_*`` flags) exactly which files and
   directories this process created, so ``pmix_ptl_close`` can remove
   precisely those at shutdown and nothing that belongs to a peer.

``pmix_ptl_base_start_listening`` then registers the listen socket's
accept event on the progress thread.


Finding and Connecting to a Server
----------------------------------

The outbound side is driven by ``pmix_ptl.connect_to_peer``, called
(blocking) during ``PMIx_Init`` or ``PMIx_tool_init``. There are two
implementations, chosen by the selected component.

Clients: the short path
~~~~~~~~~~~~~~~~~~~~~~~~~

A pure client uses the ``client`` component's own ``connect_to_peer``
(``src/mca/ptl/client/ptl_client.c``). A client already knows its server,
so the logic is short:

#. use an explicit ``PMIX_SERVER_URI`` from the info array if present;
#. otherwise consult the environment via ``pmix_ptl_base_set_peer``, which
   walks the active ``bfrops`` modules and looks for the corresponding
   ``PMIX_SERVER_URIvNN`` variable — finding one yields both the URI and
   the server's version;
#. if neither exists, the process was not launched under a server: mark it
   a **singleton**, probe for a system server via ``pmix.sys.<host>``, and
   fall back to standalone operation if none is found.

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
* a directory search of the session tmpdir, taking the first
  ``pmix.<host>.tool*`` server that answers.

``PMIX_TOOL_CONNECT_OPTIONAL`` decides whether failing to find a server is
an error or simply leaves the tool unconnected.

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
  handshake as it arrives and never waits for it. Only the replies and
  any security handshake that follow are blocking exchanges, each read
  bounded by ``ptl_base_connect_ack_timeout``.
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
``connect_ack_timeout`` (how long a server gives an incoming connection to
deliver its whole connection request, and to answer each step of any
security handshake after it; default 5 seconds, 0 for no limit),
``max_pending_connections`` (how many incoming connections may be waiting
to deliver their connection request at once; the oldest is dropped at the
limit; default a quarter of the descriptor limit, -1 for no limit), and
``report_uri``. Inspect the current values
for a build with::

    pmix_info --param ptl base
