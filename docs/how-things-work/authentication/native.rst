The ``native`` Mechanism
========================

``native`` is the security mechanism PMIx uses by default. It needs no
daemon, library, or key. It establishes the user ID (uid) and group ID
(gid) of a peer through the kernel of the host the server runs on, so it
serves peers on that host only |mdash| including peers inside containers
on it. A peer on another host authenticates with ``munge`` or ``ssl``.

This page describes what ``native`` checks, in what order, and what a
container needs for it to work.


How a Peer Is Authenticated
---------------------------

All PMIx connections use TCP. When a peer connects, ``native`` takes
these steps in order and stops at the first that decides the matter.

#. **Ask the kernel who owns the connection.** The server looks up the
   peer's end of the TCP connection in its own kernel's table of
   connections (method 2 in :doc:`methods`). If the peer's socket is
   there, its owner is the peer's uid. This covers every peer that shares
   the server's network namespace: ordinary processes, and containers run
   with host networking. The kernel records no group for a socket, so
   the group is the one the peer claims; if that claim does not hold up
   (see below), the server moves on to step 2 to have the group
   attested.

#. **Otherwise, ask the peer to prove it over a local socket.** A peer in
   a container with a network of its own is not in the server's table.
   The server then creates an ``AF_UNIX`` socket and sends the peer, over
   the TCP connection, its path and a random value. The peer connects to
   that socket and returns the value. The kernel reports the uid and gid
   of the process that connected (method 1 in :doc:`methods`), and the
   returned value ties that process to the TCP connection. The server
   then closes and removes the socket. All further traffic stays on TCP.

#. **Otherwise, refuse.** A peer whose credential shows it runs on
   another host's kernel is refused outright: no local socket can reach
   it.

A peer built against a PMIx release that predates step 2 cannot take
part in it. If step 1 cannot confirm such a peer, it is accepted on the
uid and gid it claims, as those releases always were, unless
``psec_native_legacy_auth`` is set to ``false``.


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
  that user belongs to it. Otherwise:

  * after step 2, the gid the kernel reports replaces it;
  * after step 1, which reports no group, the server goes on to step 2 |mdash|
    or, for a peer from an older release, refuses the connection.

The identity settled on here is what the server passes to its host and
what governs the peer's access to jobs and data.


Running a Peer in a Container
-----------------------------

Step 2 works only if the peer can reach the server's socket. The server
creates a directory of its own for these sockets, named
``pmix-native.XXXXXX``, beneath a base directory:

* the value of ``psec_native_socket_dir``, if set on the server;
* otherwise, the server's temporary directory (``PMIX_SERVER_TMPDIR``,
  else ``TMPDIR``).

Each server creates its own directory, so several servers can share a
base directory. The directory is created the first time a peer needs
it, and removed when the server finalizes.

To let a container's processes authenticate:

* **Mount the base directory into the container.** A socket bound to a
  path works across network namespaces, because it is a filesystem
  object.

* **If it is mounted at a different path**, set
  ``psec_native_socket_dir`` (``PMIX_MCA_psec_native_socket_dir`` in the
  environment) in the container to the path where it appears there. The
  peer combines that path with the name of the server's own directory.

A container that uses host networking needs neither: step 1 finds its
connection.


MCA Parameters
--------------

``psec_native_socket_dir`` (string)
   On a server: the base directory beneath which it creates the
   directory for its sockets. On a peer: where that same base directory
   appears to it. Default: the server's temporary directory.

``psec_native_legacy_auth`` (bool)
   Whether to accept a peer from a release that predates the
   local-socket check on the identity it claims, when the kernel cannot
   confirm it. Default: ``true``.

``ptl_base_connect_ack_timeout`` (int)
   Bounds how long a server waits for a peer to complete the
   local-socket check, from start to finish, as for any security
   handshake. Default: 5 seconds.


Limits
------

* ``native`` cannot validate a credential passed to
  ``PMIx_Validate_credential``: no connection comes with it, so there is
  nothing to ask the kernel about.

* A peer on another host is refused unless it uses an older release and
  ``psec_native_legacy_auth`` is ``true``. Use ``munge`` or ``ssl``
  across hosts.

* On platforms other than Linux and macOS, step 1 is not available and
  every peer goes through step 2. Those platforms also cannot tell a
  remote peer from a local one by its kernel, so a remote peer from a
  current release is asked for step 2, which then fails.

* In a container with host networking and a user namespace, step 1
  reports the uid but not the gid, so step 2 is used to settle the
  group. The socket directory must then be mounted into the container
  as well.
