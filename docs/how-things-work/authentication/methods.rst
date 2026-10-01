Authenticating a Containerized Peer over TCP
============================================

This page surveys the methods used in the field to authenticate the user
ID (uid) and group ID (gid) of a process running inside a container when
that process opens a TCP connection to a process running outside it. It
describes each method's mechanism, what it actually proves, and where it
breaks. It describes general techniques; :doc:`native` describes what
PMIx's default mechanism does.


The Problem
-----------

An ``AF_INET`` or ``AF_INET6`` socket carries no identity that the kernel
vouches for. The kernel reports a peer's uid and gid only for
``AF_UNIX`` sockets, through ``SO_PEERCRED``, ``SO_PEERGROUPS``,
``SO_PEERPIDFD``, ``getpeereid()``, ``LOCAL_PEERCRED``, and
``SCM_CREDENTIALS``. On Linux, ``SO_PEERCRED`` on a TCP socket succeeds
and returns the overflow uid, which identifies nobody.

Containers add three complications:

* **User namespaces.** "The peer's uid" is ambiguous: it can mean the uid
  inside the container or the uid it maps to on the host.
  Kernel-attested mechanisms report the peer's identity translated into
  the *receiver's* user namespace, which is usually what a server outside
  the container wants. An ID with no mapping comes back as the overflow
  ID (normally 65534). Rootless Podman and Docker map container root to
  an unprivileged host uid. Apptainer/Singularity, common in HPC,
  normally runs the container as the user's own uid with no remapping.

* **Network namespaces.** With Docker's default bridge networking, the
  container's ``127.0.0.1`` is not the host's, and its traffic reaches
  the host from a bridge address, often through NAT. With host
  networking (``--network=host``, and Apptainer's default), the container
  shares the host's network stack. That sharing is what makes the
  host-side socket lookups in method 2 possible.

* **Supplementary groups.** In a rootless user namespace, ``setgroups()``
  is often denied, and groups without a mapping appear as ``nogroup``.
  ``SO_PEERCRED`` reports only the primary effective gid. The full group
  list needs ``SO_PEERGROUPS`` (Linux 4.13 and later).

Before choosing a method, decide whether access policy is written in
terms of host IDs or container IDs. Kernel-attested methods yield host
IDs. Where container IDs are needed, translate through
``/proc/<pid>/uid_map`` and ``/proc/<pid>/gid_map``.

The methods fall into the families below.


1. Kernel-Attested Credentials over a Unix Socket
-------------------------------------------------

Bind-mount a Unix-domain socket from the host into the container. A
socket bound to a filesystem path works across network namespaces
because it is a filesystem object. An *abstract* Unix socket does not:
abstract names are scoped to the network namespace.

The socket can be used in two ways:

* **As the transport.** Use the Unix socket instead of TCP. The server
  reads ``SO_PEERCRED`` and ``SO_PEERGROUPS``, which are already
  translated into its own view of the IDs. Nothing can be forged, and
  there is no secret to manage.

* **As an authenticator for the TCP connection.** The client
  authenticates over the Unix socket and receives a one-time nonce or
  session key. It then proves possession of that key on the TCP
  connection, for example with an HMAC over data that is unique to that
  connection. The identity is then tied to the TCP session, not to
  whoever later obtains the nonce.

On Linux 6.5 and later, ``SO_PEERPIDFD`` returns a pidfd for the peer, a
handle that cannot be reused by a different process. Through it, the
server can inspect ``/proc/<pid>/status``, ``uid_map``, ``gid_map``, and
``cgroup`` without a pid-reuse race. That tells it which container the
peer is in, and how the peer's IDs map.

Limitations:

* The peer must be on the same host.
* The socket has to be mounted into the container when it is launched.
* ``SO_PEERCRED`` reports the credentials of the process that called
  ``connect()``, not of whoever holds the descriptor now. A descriptor
  can be passed to another process.

systemd and D-Bus, the Docker and containerd APIs, the SPIRE agent, and
the MUNGE daemon all use this pattern.


2. Host-Side Lookup of the TCP Socket's Owner
---------------------------------------------

For a loopback connection on the same host, the server takes the
connection's address/port 4-tuple and asks its own kernel which socket
holds the other end:

* A ``NETLINK_SOCK_DIAG`` (``inet_diag``) query returns the socket's
  owning uid (``idiag_uid``) and its inode. ``/proc/net/tcp`` has the
  same information but is slower to search. The uid is reported in the
  querier's user namespace.
* There is no gid in the reply. Getting one means mapping the inode to a
  process by scanning ``/proc/*/fd`` and then reading
  ``/proc/<pid>/status``. That scan is expensive and racy.

PostgreSQL's ``ident`` authentication, ``oidentd``, and the RFC 1413
Identification Protocol use this lookup. Across a network, RFC 1413 is
worthless, because the remote host is only asserting its own answer.
Locally, the answer comes from the server's own kernel and can be
trusted.

Limitations:

* The server can see the socket only if the container uses host
  networking. With a separate network namespace, the server would have to
  ``setns()`` into it, which needs ``CAP_SYS_ADMIN``, and NAT rewrites
  the 4-tuple the lookup depends on.
* Linux only.
* The uid is that of the process that created the socket, not of whoever
  holds it now. The owner can also change between the lookup and the
  use of the answer.


3. A Local Credential Service Verified Remotely
-----------------------------------------------

A trusted daemon on the client's host authenticates the caller over a
Unix socket (method 1) and issues a signed or encrypted credential. The
client sends the credential over TCP, and the server verifies it.

* **MUNGE** is the HPC standard, used by Slurm and older Torque releases.
  The credential carries the uid and gid as ``munged`` sees them. If the
  daemon's socket is bind-mounted into the container, that is the host's
  view. Because the key is shared across the cluster, the credential can
  be verified on another node. Credentials expire after a set lifetime,
  and the daemon keeps a replay cache.
* **Slurm's ``auth/slurm`` with ``sackd``** issues a newer signed token
  on a similar model.
* **SPIFFE/SPIRE** is the cloud-native equivalent. The SPIRE agent's
  Workload API identifies the caller with ``SO_PEERCRED`` and inspection
  of ``/proc`` and the caller's cgroup. Its ``unix`` attestor yields the
  uid, gid, and binary path; its Docker and Kubernetes attestors identify
  the container or pod. The agent then issues an X.509 or JWT SVID
  (SPIFFE Verifiable Identity Document), which the client uses for
  mutual TLS over TCP.
* **Kerberos/GSSAPI** authenticates a *principal*, not a uid. The
  principal is then mapped to a uid through NSS. Getting a ticket cache
  or keytab into the container is the usual difficulty.

Limitation: most of these credentials are bearer tokens. Without
*channel binding*, any process that obtains one can replay it on its own
connection until it expires. Channel binding means including a
server-chosen nonce, or a TLS exporter value (RFC 9266), inside the
signed credential.


4. Proving Access to a Protected File
-------------------------------------

The server writes a random secret to a file readable only by uid ``U``
(mode ``0400``), or only by group ``G`` (mode ``0440``). The file sits
on a path visible inside the container. The client proves it could read
the file by returning an HMAC of a server-chosen nonce, keyed with the
secret. The secret itself never crosses the wire.

X11's ``MIT-MAGIC-COOKIE-1``, Jupyter's token file, and many
rendezvous-file schemes rely on this idea. It is portable, needs no
daemon, and works on any operating system.

Limitations:

* It proves only that the client can read the file. That equals holding
  the uid or gid only while the file's permissions are enforced. In a
  container without a user namespace, container root is host root and
  can read any file.
* In a user namespace, whether the client can read the file depends on
  the ID mapping and on any ID-mapped mounts. Check each container
  runtime separately.
* Proving group membership works only if the group is mapped into the
  container.


5. Recording Identity at ``connect()`` with eBPF
------------------------------------------------

A BPF program attached to the cgroup ``connect4``/``connect6`` hook, or
to ``sockops``, runs when the client connects. It records
``bpf_get_current_uid_gid()`` (host-namespace IDs) and the cgroup ID in
a BPF map, keyed by the 4-tuple or by the socket cookie. The server then
looks up the connecting peer in that map.

Cilium and Tetragon use this technique for identity-aware networking.
It works across separate network namespaces if the server also consults
connection tracking (conntrack) to undo NAT.

Limitations:

* Loading the program requires privilege, and it adds operational
  weight.
* Linux only.
* Only the primary gid is recorded unless the program does more work.


6. Identity Assigned by the Launcher
------------------------------------

The launcher created the container, so it already knows the uid and gid
the container runs as. At launch, it gives the container a credential
issued for that container: a token in a secrets mount, a client
certificate, or a key passed in an inherited file descriptor. The server
trusts "holds the credential issued for container X" and looks up X's
uid and gid in the launcher's records.

Kubernetes projected service-account tokens, checked through
``TokenReview``, and Slurm job credentials work this way.

This is often the most robust option across nodes. What it checks,
though, is what the launcher recorded, not the process's current
credentials: if a process in the container changes its uid, the server
will not notice.


7. Labeled Networking
---------------------

SELinux labeled IPsec, or NetLabel/CIPSO, lets ``SO_PEERSEC`` return the
peer's security context over a TCP connection, and that context can be
mapped to a user. The kernel vouches for the result, but the method is
in practice found only on systems that run multi-level security (MLS).


Methods Too Weak to Rely On Alone
---------------------------------

* **A uid and gid the client reports about itself in the handshake.**
  This is NFS ``AUTH_SYS`` (ONC RPC ``AUTH_UNIX``), the classic
  counter-example and the reason NFS moved to Kerberos. Anyone can forge
  it. In a user namespace, it also reports the uid *inside* the
  container rather than the host uid.
* **Mapping the source IP address to a container and then to a uid.**
  This is only as strong as the guarantee that a container cannot change
  its address. A container with ``CAP_NET_ADMIN``, or one sharing a
  network namespace with others, defeats it.
* **A privileged source port** (below 1024), the rlogin "trusted port"
  convention. Root inside a container's own network namespace can bind
  low ports.


Comparison
----------

.. list-table::
   :header-rows: 1
   :widths: 24 13 18 12 18 15

   * - Method
     - Kernel-attested
     - Separate network namespace
     - Cross-node
     - Supplementary groups
     - Platforms
   * - Unix socket ``SO_PEERCRED`` / ``SO_PEERGROUPS``
     - Yes
     - Yes (bind mount)
     - No
     - Yes (Linux)
     - Most POSIX systems
   * - ``sock_diag`` / ident lookup
     - Yes
     - Only with host networking
     - No
     - Through ``/proc``, racy
     - Linux
   * - MUNGE, SPIRE, Kerberos
     - Yes, at issue
     - Yes
     - Yes
     - MUNGE: primary gid only; SPIRE: yes
     - Broad
   * - File-permission challenge
     - Indirectly (file permissions)
     - Yes (shared mount)
     - With a shared filesystem
     - One file per group
     - All
   * - eBPF ``connect()`` hook
     - Yes
     - Yes, with conntrack
     - No
     - Primary gid only
     - Linux, privileged
   * - Launcher-issued credential
     - No (trusts the launcher)
     - Yes
     - Yes
     - From the launcher's records
     - All


Recommendations for HPC Environments
------------------------------------

#. **Same host:** use a bind-mounted Unix socket with ``SO_PEERCRED``
   and ``SO_PEERGROUPS`` (plus ``SO_PEERPIDFD`` where available), either
   as the transport itself or to authenticate the TCP connection through
   a nonce tied to that connection. It is the only option that the
   kernel vouches for, needs no secret, and handles the user-namespace
   mapping correctly with no extra work.
#. **Across nodes, or where a bind mount is not possible:** use MUNGE,
   or a SPIFFE-style issuer, with the daemon's socket mounted into the
   container. Add channel binding so that a captured credential cannot
   be replayed on another connection.
#. **Portable fallback:** use the file-permission HMAC challenge, and
   document that it does not hold against root in a container without a
   user namespace.
#. **In every case:** state whether policy is written in host IDs or
   container IDs, and translate through the peer's ``uid_map`` and
   ``gid_map`` when the two differ.
