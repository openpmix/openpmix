
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
     - Peers of earlier releases on the server's host. Its URI is
       ``<nspace>.<rank>;tcp4://<host>:<port>`` (``tcp6://`` for IPv6).
       A server listens on it, on a loopback interface, only when it
       takes legacy peers (``ptl_base_legacy_peers``,
       ``PMIX_SERVER_LEGACY_PEERS``).

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
   through any ``if_include`` / ``if_exclude`` directives, and take a
   **loopback** device. An entry that is neither an interface name nor
   a subnet, or an include list none of whose entries is on this host,
   stops the listener from opening at all. A loopback device's address
   must be a loopback address. Directives that leave no loopback device
   fail the server's init: a public interface is never taken instead. A
   host that asks for connections from other hosts
   (``PMIX_SERVER_REMOTE_CONNECTIONS`` set to true) fails its init too.
#. **Bind a listen socket.** Try each configured port (default ``0`` —
   let the kernel assign one), set ``SO_REUSEADDR`` and close-on-exec,
   ``listen`` with ``SOMAXCONN``, and make the socket non-blocking.
#. **Publish the URI.** Build a URI of the form
   ``nspace.rank;tcp4://host:port``. The base stores the server's URI -
   this one when there is a TCP listener, else the local socket's - in
   ``gds`` under both ``PMIX_MYSERVER_URI`` and (for older tools)
   ``PMIX_SERVER_URI``. If
   ``report_uri`` is set, also emit it: the local socket's URI to stdout
   (``-``) or stderr (``+``); the TCP URI to stdout (``tcp-``) or stderr
   (``tcp+``), which fail the init with no TCP listener; to an integer
   pipe fd, the TCP URI's ``<varnames>;<uri>`` string and then the local
   socket's, each with its NUL, in one write (the local socket's alone
   with no TCP listener); or to a named file. A file already at that name
   is reused only if it is the server's user's - a file of anyone
   else's, root's included unless the server is root, stops the init
   with a message naming its owner - and the file, reused or new, is
   given mode 0644 whatever the umask or an earlier run made it. A
   report file also carries the ``local:``, ``bfrops:``, ``psec:`` and
   ``auth:`` lines described below - the first three only when the
   server takes legacy peers - but no ``lock:`` line. See
   :doc:`connections` for the table.
#. **Drop rendezvous files.** Depending on the server's role and flags,
   write well-known contact files that a client or tool can later
   find (``src/mca/ptl/base/ptl_base_rndz.c``):

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
      * - ``pmix.<host>.session.<id>``
        - a server offering *session*-level tool support, for session ``<id>``
      * - ``pmix.<host>.tool.<pid>``
        - any tool-supporting server (keyed by PID)
      * - ``pmix.<host>.tool.<nspace>``
        - any tool-supporting server (keyed by nspace)

   **Where they go.** The tool files - ``pmix.<host>.session.<id>``, by PID, by
   namespace - go in ``<system tmpdir>/pmix-user.<uid>``, a directory of
   the server's user's own, whatever its session tmpdir is. So only that
   user (or root) can put a file at those names, and a tool has one
   place to look. The server makes it mode 0700 - 0711 when it takes
   tools of other users, which can then open the files at their names
   but not list them. One that is there is used only if it is a
   directory, not a link, of the server's user, that no other user can
   write; it is opened with ``O_DIRECTORY | O_NOFOLLOW`` and checked by
   ``fstat``. Anything else at the name is reported
   (``rndz-dir-not-ours``) and the files are not written: the server
   runs on, and its clients reach it by the URI they are given. The
   system roles' files - ``pmix.sys``, ``pmix.sched``, ``pmix.sysctrlr`` -
   go in the system tmpdir when the host named one, as before; otherwise
   a root server puts them in ``/run/pmix`` (``/var/run/pmix`` where there
   is no ``/run``; it must be root's, and only root may write it), and a
   non-root one, or root where neither can be used, in its own
   ``pmix-user`` directory.

   Each file contains the URI, the server's version, its PID, its
   ``uid:gid``, and a timestamp. A line tagged ``local:`` carries the
   server's host name and the local socket's URI, as
   ``local:<host>;<uri>``. Three more tagged lines follow. ``bfrops:``
   lists the wire formats (``bfrops`` components) the server accepts and
   ``psec:`` its security mechanisms, each comma-delimited in the
   server's priority order and limited by its MCA parameters.
   ``lock:flock`` says that the server holds a lock (``flock``) for as
   long as it runs on a file of its own beside this one,
   ``.<name>.lock``, mode 0600, which no search for ``pmix.*`` files
   matches; the kernel lets go of the lock however the server exits.
   Readers find every tagged line by its tag; older readers stop before
   them. A server that takes no legacy peers has no TCP listener, so its
   first line is already the local socket's URI: it writes no
   ``local:``, ``bfrops:`` or ``psec:`` line, and the handshake settles
   the wire format and security mechanism. The ``pmix_ptl_base`` state struct
   records (via its ``created_*`` flags and ``pmix_ptl_made_dir_t``
   records) exactly which files and directories this process created, so
   ``pmix_ptl_close`` can remove precisely those at shutdown and nothing
   that belongs to a peer. Each is removed through a descriptor held on
   the directory that holds it, and only if it is what was made - under
   whatever name it has there by then - so a directory renamed while the
   server runs, or one above it, is cleaned where it went. A directory
   the server made - for a rendezvous file, or as the system tmpdir - is
   removed only if it is empty once the server's own files are gone:
   anything else in it is another process's, and another server may
   share it.

The tmpdirs
~~~~~~~~~~~

Before it makes anything in them, a server checks the path of its
session and system tmpdirs (``src/mca/ptl/base/ptl_base_tmpdir.c``):
its socket's directory and its ``pmix-user`` directory go there, and
clients reach the socket by its path, so a user who could rename
entries in a tmpdir, or in a directory above it, could move them aside
and put others at their names. That would only stop the server's
clients connecting - they check the server's uid with the kernel - but
it is refused. As OpenSSH's ``StrictModes`` does, the server resolves
the path (``realpath``; a tmpdir not there yet is judged by the deepest
part that is) and walks it from ``/``:

- **every directory above the tmpdir** must be owned by root, the
  server's user, or an id from outside the server's user namespace - one
  with no entry in ``/proc/self/uid_map``, made by the host or the
  container's setup, counted as root - and writable by nobody else,
  unless it has the sticky bit or its group's only member is its owner (a
  user private group);
- **the tmpdir itself** may be anyone's - Slurm gives its root server a
  directory owned by the job's user - if nobody but its owner can write
  it, with the same group exception; or else it must pass the rule
  above, as ``/tmp`` does. Another user's sticky directory does not
  pass: its owner can still rename anything in it.

A directory that fails stops the init with a message naming it, its
owner and mode, and why. ``ptl_base_tmpdir_check`` changes that:
``refuse`` (the default), ``warn`` - say so and run on - or ``silent``,
not to check at all. The last two are for a tmpdir no other user
shares that the rules misjudge: a Kubernetes ``emptyDir`` mounted at
``/tmp``, root's and 0777 without the sticky bit, or a directory on an
NFS export with ``root_squash``, owned by ``nobody``. Either gives up
only availability: a client still refuses a server whose uid is not the
one it expects. Clients and tools do not read the parameter.

Beside the TCP listener, every server opens its **local socket** - the
``usock`` transport's, in ``src/mca/ptl/usock/ptl_usock.c``:

#. **Make a directory of its own** beneath the server's temporary
   directory: ``<tmpdir>/pmix-XXXXXX``, mode 0700, made with ``mkdirat``
   through a descriptor held on the temporary directory, which must
   already exist.
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
its directory, through the descriptors it holds; a server that is killed
leaves them behind.

The local socket's URI reaches a client in its environment, as
``PMIX_SERVER_LOCAL_URI``, and a tool on the rendezvous file's ``local:``
line. ``PMIX_SERVER_URI``, ``PMIX_MYSERVER_URI``, the report and the
first line of the rendezvous files are the TCP URI when the server
listens on TCP - they reach older peers, which cannot use the local
socket - and the local socket's otherwise.
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
* a specific server PID, nspace or session-level server
  (``PMIX_SESSION_RNDZ_ID``), whose
  file is looked for in the ``pmix-user`` directories of the users whose
  servers the tool may talk to - its own, root's, and those
  ``ptl_base_server_access`` names - each looked in, not below; or
* told none of those, the session-level servers of the tool's own user on
  the node - ``pmix.<host>.session.*`` in its own ``pmix-user``
  directory, a file whose server is gone passed over. The only one is
  taken; with several the tool is told which there are and fails
  (``too-many-sessions``), as it cannot know which was meant. A session
  server needs an ID to name its file by (``PMIX_SESSION_RNDZ_ID`` to
  ``PMIx_server_init``, its host's choice; else the scheduler's
  ``PMIX_SESSION_ID`` is used) and fails to start without one, or where a server of its session
  already runs on the node. A system role's file is looked for in the system tmpdir the
  host named, else ``/run/pmix`` and ``/var/run/pmix``, then root's and
  the tool's own ``pmix-user`` directories and those of the listed
  users. Only with legacy peers on (``ptl_base_legacy_peers``) does a
  tool also walk the whole system tmpdir, ``ptl_base_search_depth``
  levels deep, and look at the system tmpdir itself for a system role -
  where a server of an earlier release writes its files. The same goes
  for a ``PMIX_QUERY_AVAIL_SERVERS`` query. A file found this way, or at
  a fixed name, is believed only of the server that wrote it: it must be
  the file at the name, with one name, that only its owner can write
  (else it is passed over); once connected, the kernel must say the
  server runs as the file's owner (``server-not-file-owner``) and, for a
  file by PID, on the local socket, is that process
  (``server-not-file-pid``). A system role's file is taken only from
  root, the tool's own user, or a user ``ptl_base_server_access`` names.
  Such a file never vouches for a server the kernel cannot name. A file
  whose owner is not a user whose server the tool takes - the user the
  server that started it named, else root, the tool's own user, or a
  user ``ptl_base_server_access`` names - is passed over before the files
  found are counted, so another user's running server beside the tool's
  own does not fail the search. Every file left must name the same
  server; files naming different ones fail the search, saying so. A file whose server is gone
  is passed over, here and by a ``PMIX_QUERY_AVAIL_SERVERS`` query: it
  says its server holds the lock beside it, and the searching process
  can take that lock - whatever PID namespace, container or jail either
  runs in. A file with no such line, from an earlier release, is
  judged instead by the PID it records, as a server starting at its
  name judges it: it is passed over when that process is not running.
  A file is kept when it names another kind of lock or has no lock
  file; when its lock file has another name or others could open it;
  and when the searching process cannot open it - only the server's
  user and root can. One larger than any server writes is passed over. A server
  starting under a name whose lock another server holds takes the name
  as in use (``rndz-name-held``), whatever its PID says - except the
  file named for its own PID, which collides only with a server in
  another PID namespace and which it then does not write: a tool here
  that looks for this server by PID finds that one, and checks it as it
  checks any. A file at the name that another user owns - root's too,
  unless the server is root - is foreign, whatever its PID or lock
  says: it is never reclaimed nor taken as in use, and the server writes
  no file at that name, says why (``rndz-file-foreign``) and runs on.
  Otherwise it judges a file at the name by its PID, as
  before. At finalize it removes each of its files only while it is still
  the file it wrote, and its lock file only while it is the one it
  holds.

A tool started by a server has ``PMIX_SERVER_LOCAL_URI`` in its
environment, as a client does. A rendezvous or report file with a
``local:`` line for the tool's own host takes it to the local socket, and
only there. A file without one is an older server's, and the tool uses
its TCP URI. One whose ``local:`` line names another host - a report
file read elsewhere - is for a tool on that host; a tool here is left
with the TCP URI, which is not its host's, and does not dial it.

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
applied by user alone. A server the table does not show is refused. A
server at an address that is not this host's - by the interface list,
or by the kernel's own list of the addresses its interfaces hold - is
never dialled (``PMIX_ERR_UNREACH``): a process connects only to a
server on its own host. Where the platform has no table to ask, a
process connects only to a server named for it - a client's, or the one
that started it - or one whose file vouches for it - see "A server the
kernel cannot name" in :doc:`connections`.

Each connect starts afresh: a URI given to one connect by
``PMIX_SERVER_URI`` or ``PMIX_TCP_URI`` is not used by a later attach
that was given none.

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
  mechanism it uses ``munge`` if it has it, else ``native``;
* a URI given directly carries neither, and the tool uses its newest. If
  the server then closes the connection without answering - what a server
  does with a format or mechanism it lacks - the tool suggests giving it
  the server's file instead, as ``file:<path>``.

For a tool's first server the choice becomes the tool's own, as a client's
does. A server it attaches to later gets its own choice, which leaves how
the tool speaks to its other servers unchanged.

On the local socket this choice is only the tool's preference: it is
offered first, and the server decides (see :doc:`connections`). A server
that takes no legacy peers writes no ``bfrops:`` or ``psec:`` lines.

Both paths converge on ``pmix_ptl_base_make_connection``, which parses the
URI into a socket address, refuses one that is not this host's, opens the
connection (with retries), sends the
connection request - on the local socket, the offer, the server's choice
and the credential - runs the handshake, and — on success —
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
each, -1 for no limit; a user other than the server's, root and the
host's ``PMIX_USERID`` may have at most a quarter of the waiting tools,
at least one),
``max_pending_bytes`` (the most bytes allocated, between them, for
connection requests still arriving; each request's buffer starts at 4 KB
and doubles as its bytes arrive, up to the length it names, and a request
whose buffer cannot grow within the limit is dropped; default 16 MiB, 0
for no limit), and
``report_uri``. Inspect the current values
for a build with::

    pmix_info --param ptl base
