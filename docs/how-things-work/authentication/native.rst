The ``native`` Mechanism
========================

``native`` is the security mechanism PMIx uses by default. It needs no
daemon, library, or key. It establishes the user ID (uid) and group ID
(gid) of a peer through the kernel of the host the server runs on, so it
serves peers on that host only |mdash| including peers inside containers
on it. A server takes no peer from another host, whatever its mechanism.

This page describes what ``native`` checks, in what order, and what a
container needs for it to work.


How a Peer Is Authenticated
---------------------------

A peer connects either over the server's local socket |mdash| an
``AF_UNIX`` socket every server listens on, which every client and tool of
this release on the server's host uses |mdash| or over TCP, as peers of
earlier releases do. ``native`` takes these steps in order and stops at
the first that decides the matter.

#. **On the local socket, the kernel names the peer.** The kernel reports
   the uid and gid of the process at the far end of the connection
   (method 1 in :doc:`methods`), whatever the peer claimed. This works in
   a container too, as long as the server's socket is reachable from it.

#. **Over TCP, ask the kernel who owns the connection.** The server looks
   up the peer's end of the TCP connection in its own kernel's table of
   connections (method 2 in :doc:`methods`). If the peer's socket is
   there, its owner is the peer's uid. This covers every peer that shares
   the server's network namespace. The kernel records no group for a TCP
   socket, so the group is the one the peer claims, and a claim that does
   not hold up (see below) is refused.

#. **Otherwise, refuse it - or, where there is no table, take the
   peer's claim.** A TCP peer at an address of this host that the table
   does not show is refused. On a platform with no table to ask, a TCP
   peer is accepted on the uid and gid it claims, as every earlier
   release accepted it. A server opens its TCP listener only to take peers of earlier
   releases (``ptl_base_legacy_peers``), which have no other way to say
   who they are; a peer of this release that connects over TCP all the
   same is taken the same way. A table there is but that could not be
   read - the server is out of descriptors or memory - is no answer, and
   the peer is refused.

A peer that connects over TCP from an address that is not this host's
never reaches ``native``: the server refuses it before any security
component is asked (see :doc:`../connections`).


Which Identity Is Used
----------------------

Both kernel lookups report the peer's IDs as the server sees them. In a
container with a user namespace, those differ from the IDs the peer sees
for itself |mdash| for example, root inside a rootless container is an
ordinary user outside it. ``native`` therefore uses the IDs the kernel
reports, not the ones the peer claims:

* **A client the host registered** must be the user and group the host
  registered it as. If the kernel reports a different uid, or a gid that
  differs from the registration, the connection is refused.

* **A tool**, which names its own identity, is given the uid the kernel
  reports in place of the one it claimed. The group it claimed stands if
  that user belongs to it. Otherwise, on the local socket the gid the
  kernel reports replaces it; over TCP, which reports no group, the
  connection is refused.

The identity settled on here is what the server passes to its host and
what governs the peer's access to jobs and data.


Running a Peer in a Container
-----------------------------

A client or tool of this release reaches its server over the server's
local socket, which is in a directory of its own beneath the server's
temporary directory (``PMIX_SERVER_TMPDIR``, else ``TMPDIR``). For a
process in a container to reach it, that temporary directory must be
mounted into the container at the same path, and the socket must let the
container's user in - see ``PMIX_SOCKET_MODE`` in
:ref:`PMIx_server_init <man3-PMIx_server_init>`. A process that cannot
reach the socket fails its initialization rather than falling back to TCP.

A container with a user namespace of its own - rootless Podman, say -
maps the server's ids for the processes in it. A client there maps the
server's ids back to the ones the server named; for that, the server's
user and group must be mapped into the container's namespace, as they are
when the container runs as the user who started the job.


Limits
------

* ``native`` cannot validate a credential passed to
  ``PMIx_Validate_credential``: no connection comes with it, so there is
  nothing to ask the kernel about.

* A peer on another host, or in a container with a network of its own,
  connects from an address that is not this host's. The server refuses
  it before ``native`` is asked - see :doc:`../connections`.

* On a platform whose kernel keeps no table of TCP connections - any
  but Linux, macOS and FreeBSD 12 or later, and a Linux host where
  neither ``sock_diag`` nor ``/proc/net/tcp`` can be read - step 2 is
  not available, so a server that takes legacy peers takes a peer
  connecting over TCP on the identity it claims. On FreeBSD, a server that does
  not run as root confirms another user's peer only while
  ``security.bsd.see_other_uids`` is left at its default of ``1``.

* A peer that reaches the server over TCP through a relay on the server's
  host (``socat``, ``docker-proxy``, or any other process that accepts the
  peer's connection and opens its own to the server) is identified in
  step 2 as the relay's user: the connection the server sees is the
  relay's. A registered client is then refused, because that is not the
  user it was registered as, and a tool takes the relay's identity.
  Peers in containers should reach the server through its local socket
  instead.
