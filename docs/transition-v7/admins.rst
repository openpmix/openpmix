Installing and Administering PMIx v7
====================================

For people who install PMIx, and the host environment that uses it
(PRRTE, a resource manager), on a system. For what people running jobs
will notice, see :doc:`users`.

The reference pages for this role are :ref:`label-sysadmin` and its
sub-pages; each section below links to the detail.


Installing v7
-------------

**Keep one PMIx installation, the latest.** Applications do not need
to be rebuilt: a program built against PMIx v5.0 runs against the v7
library. See :doc:`/admin/releases`.

**Go from v5.0 straight to v7.0.** Skip v6.x, which was announced "do
not use".

**Upgrade the host environment at the same time.** A host environment
must be built against the PMIx release it runs with.

* PRRTE v5 requires PMIx v7, and PRRTE v3 (paired with PMIx v5)
  refuses it. There is no PRRTE release that works with both. See
  :doc:`/admin/prrte`.
* Rebuild any other host environment - a resource manager's PMIx
  plugin, say - against v7.

**Build requirements changed:**

* hwloc 2.1.0 or later (v5.0 took 1.11);
* libevent only - support for libev is gone, along with
  ``--with-libev``;
* a C11 compiler with atomics; ``--enable-c11-atomics`` and
  ``--enable-builtin-atomics`` are gone;
* from a git clone, Python 3.6 or later; flex is no longer needed;
* Solaris and Sun Studio are no longer supported.

**MUNGE is built only when asked for.** v5.0 built the ``munge``
component whenever MUNGE was installed. v7 builds it only with
``--with-munge``. If you use MUNGE authentication, add the option.

**Compression: build every node the same way.** v7 adds the ``zstd``
and ``lz4`` compression components, and prefers ``zstd`` to the zlib
ones when its development headers are installed. Compressed data
crosses nodes, and a node using ``zlib`` cannot read ``zstd`` or
``lz4`` data, so every node in a job must pick the same component.
This follows from a shared installation; it matters during a rolling
upgrade. See :ref:`label-install-optional-support-libraries`.


Containers and applications of earlier releases
-----------------------------------------------

**What changed.** v7 clients and tools connect over a local socket.
Releases before v7 connect over TCP, and a v7 server takes them only
when both of these hold:

* PMIx was configured with ``--enable-legacy-peers``. It is off by
  default.
* The support is turned on at run time: the MCA parameter
  ``ptl_base_legacy_peers=true``, or the host's
  ``PMIX_SERVER_LEGACY_PEERS`` attribute. PRRTE does not pass the
  attribute, so for PRRTE set the parameter where its daemons read it
  (a parameter file, say).

Even then, the server listens for them on loopback only.

**What users see without it.** Each process of an older application -
a container with PMIx v5 inside, for example - prints one error line
and runs as a singleton, unaware of its job. The job does not fail; it
just does not work as a job.

**What to do.** Decide whether your systems run containers or
applications with an older PMIx inside. If they do, build PMIx with
``--enable-legacy-peers`` and turn it on. If they do not, leave it off,
and tell users to update the PMIx in their images.

**The other direction needs nothing.** A v7 application runs under
servers from PMIx v3.2 on.

See :ref:`label-sysadmin-releases` and :ref:`clients of earlier releases <label-conn-earlier-clients>`.


Tools on login nodes
--------------------

**What changed.** A tool - ``prun``, ``pterm``, a debugger, a monitor
- reaches a PMIx server only on its own node. PMIx no longer takes
connections from other nodes, and the ``ssl`` authentication that
served them is gone. A server asked to take them
(``PMIX_SERVER_REMOTE_CONNECTIONS``) fails to start.

**What to do.** Users run tools on the node where the server runs.
For a launcher or a debugger, that means ssh to the node where the DVM
controller (``prte`` or ``mpirun``) is running - the approach the DDT
debugger recommends. Make sure users can reach that node.

**Coming.** Launching from a login node will remain possible. The
method is being worked out: most likely ``prun`` will send its launch
request through PRRTE's own messaging rather than through PMIx.
Debuggers are not settled yet.

.. DRAFT: update when the login-node method is decided.


Shared systems
--------------

**Access is by user and group.** A job's data, output, monitoring,
and control belong to the job's owner by default. The host widens it
with an access list for the job (``PMIX_ACCESS_PERMISSIONS``), which
users set when they launch. See :doc:`/hosts/access-control`.

**Groups come from the kernel.** A process's groups are the ones the
kernel reports for its connection (on Linux 4.13 or later, macOS, and
FreeBSD), not a lookup in the group database. Elsewhere PMIx uses the
groups the host gives it, and failing that the process's own group.
A user added to a group takes it only in processes started afterwards.

**Tools of other users are refused by default.** v5.0 took them; v7
takes, besides the server's own user and root, only the user the host
names as the owner of its socket. The host can allow others
(``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``), and must then also widen the
socket's mode to them (``PMIX_SOCKET_MODE``). How a host exposes this
is in its own documentation.

**Tools check the server too.** A tool talks to a server running as
its own user or as root. To let users' tools reach a DVM run by a
service account, name that account in ``ptl_base_server_access``
(``users=<list>,groups=<list>``) in the users' parameter file.

**Output files belong to the job's owner.** Files from ``--output
file=`` or ``dir=`` are created as the job's owner. A server that
cannot act as that user - a DVM run by a service account - has the
launcher write them instead, as the user who ran it, on the
launcher's node (``PMIX_IOF_FILE_WRITER``). An existing file owned by
anyone else is not overwritten; PMIx reports it once per job.


Temporary directories
---------------------

* **Keep** ``$TMPDIR`` **short.** The local socket is created under
  it, and a socket path is limited to about 100 characters. The
  default on macOS is too long.
* **The server checks its temporary directory at start.** It refuses a
  directory another user could change: one on a path not owned by
  root or the server's user, or one others can write without the
  sticky bit. Some container volumes are like that - a Kubernetes
  ``emptyDir``, for example. ``ptl_base_tmpdir_check`` set to
  ``warn`` or ``silent`` lets the server run there.
* **Rendezvous files moved.** Each user's servers write theirs in
  ``<tmpdir>/pmix-user.<uid>/``. A system server, scheduler, or system
  controller running as root writes its files in ``/run/pmix``, unless
  the host names a system temporary directory.
* **Cleaning up.** A rendezvous file whose server is gone is reclaimed
  automatically. Session and socket directories still accumulate after
  a crash; clean out old ``pmix*`` entries as before.

See the "Temporary files" section of :doc:`/admin/authentication`.


Scripts that read a server's address
------------------------------------

* ``--report-uri -`` and ``+`` (``PMIX_TCP_REPORT_URI``) now print the
  local socket's address, which v7 tools use.
* ``tcp-`` and ``tcp+`` print the TCP address instead, for a tool of
  an earlier release. They need legacy peers on.
* A v7 tool given a TCP address for a v7 server is refused with
  ``PMIX_ERR_LOCAL_URI_REQUIRED``, and told to use the local address.


.. _label-transition-v7-admin-settings:

Settings that now fail
----------------------

**What changed.** PMIx no longer ignores a setting it cannot honor.
Naming a component that no longer exists, a user in
``ptl_base_server_access`` the system does not know, or a relative
socket directory makes the process fail to start, with a message.

**What to do.** Check parameter files (``pmix-mca-params.conf``) and
``PMIX_MCA_*`` settings in module files and job scripts against this
list.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - v5.0 component
     - In v7
   * - ``gds/shmem2``
     - Replaced by ``gds/shmem3``.
   * - ``pstat/linux``
     - Renamed ``pstat/plinux``; ``pmacos`` and ``pfreebsd`` are new.
   * - ``ptl/client``, ``ptl/server``, ``ptl/tool``
     - Replaced by ``ptl/usock`` (local socket) and ``ptl/tcp`` (earlier
       releases only). Every available transport is used; the ``ptl``
       selection parameter is ignored.
   * - ``psec/none``, ``psec/dummy_handshake``
     - Removed. ``native`` and ``munge`` remain.
   * - ``preg/native``
     - Removed. ``raw`` and ``compress`` remain.
   * - ``plog/default``
     - Removed. ``stdfd``, ``syslog``, and ``smtp`` remain.
   * - ``bfrops/v12``, ``bfrops/v20``
     - Removed (they served releases v7 no longer talks to).
   * - the ``psquash`` framework
     - Removed.


New and changed parameters
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 15 50

   * - Parameter
     - Default
     - What it does
   * - ``ptl_base_legacy_peers``
     - ``false``
     - Take clients and tools of releases before v7 (needs
       ``--enable-legacy-peers``).
   * - ``ptl_base_server_access``
     - none
     - Servers besides one's own user's and root's that a client or
       tool will talk to.
   * - ``ptl_base_tmpdir_check``
     - ``refuse``
     - What a server does with a temporary directory another user could
       change: ``refuse``, ``warn``, or ``silent``.
   * - ``ptl_base_connect_ack_timeout``
     - 5
     - Seconds a new connection has to send its whole request.
   * - ``ptl_base_max_pending_connections``
     - 0
     - Connections still sending their request, and, separately, tools
       waiting for the host's decision. 0 means an eighth of the
       file-descriptor limit; -1 no limit. A user other than the
       server's, root's, and the host's own may have at most a quarter
       of the waiting tools.
   * - ``ptl_base_max_pending_bytes``
     - 16 MiB
     - Total buffer allocated to connection requests still arriving.
   * - ``ptl_base_max_msg_size``
     - 32
     - Largest message accepted, in megabytes. 0 means no limit.
   * - ``pcompress_base_max_size``
     - 512 MiB
     - Largest decompressed size accepted.
   * - ``plog_base_max_suppressed``
     - 1024
     - Distinct log messages remembered for duplicate suppression.

See :ref:`label-sysadmin-authentication` for the full list, and
``pmix_info --param all all`` for every parameter and its value.
