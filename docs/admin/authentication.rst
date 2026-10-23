.. _label-sysadmin-authentication:

Controlling Authentication
==========================

This page covers how PMIx authenticates connections on a system, and
the controls a system administrator has over it.


How PMIx authenticates connections
----------------------------------

When a process connects to a PMIx server, a security (``psec``)
component establishes the process's user and group. Three exist:

* ``native`` - asks the kernel; serves processes on the same node only.
  Always built, and the default.
* ``munge`` - uses the MUNGE daemon; works across nodes.
* ``ssl`` - uses X.509 certificates; works across nodes.

What each one checks is described in :doc:`/hosts/connections`. The host
environment then makes its own checks before accepting the connection.

**The settings that count are the server's.** A server accepts only
the components it has loaded, and passes its list to the processes it
starts. A process cannot use a method the server does not offer. So
apply the controls below where the host environment's daemons read
them.

You control which methods are available in four ways, from the most
permanent to the least.

1. Do not build the component
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``munge`` and ``ssl`` are built only when you ask for them:

* ``--with-munge[=DIR]`` builds ``munge``.
* ``--with-openssl[=DIR]`` builds ``ssl``.

To leave out a component that would otherwise build, name it in
``--enable-mca-no-build``:

.. code-block:: sh

   shell$ ./configure --enable-mca-no-build=psec-munge ...

Keep at least one component. A server with none fails to start; it
never accepts connections without authentication.

See :ref:`label-building-pmix-cli-options-optional-support-libraries`
and :ref:`label-building-installation-cli-options`.

2. Restrict the components in a parameter file
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``psec`` MCA parameter lists the components that may be used, or,
with a leading ``^``, those that may not:

.. code-block:: sh

   # use only munge
   psec = munge

   # use anything except ssl
   psec = ^ssl

Put the setting in a parameter file. A parameter file belongs to one
PMIx installation: each PMIx library reads the files under its own
installation prefix when it initializes, and no other. Put the setting
in the installation the host environment's server runs from - its
settings are the ones that count. A system with more than one PMIx
installation, or applications that bring their own library (in a
container, say), has a set of files for each:

* ``$prefix/etc/pmix-mca-params.conf`` - the installation's default. A
  user's ``$HOME/.pmix/mca-params.conf`` and the user's environment can
  override it.
* ``$prefix/etc/pmix-mca-params-override.conf`` - settings here take
  precedence over the environment and over every other parameter file.
  Use this file for settings that users, and the host environment,
  should not change.

A process reads these files because its environment does not say
otherwise. The files themselves are named by MCA parameters
(``mca_base_param_files``, ``mca_base_override_param_file``), so a
process whose environment sets those - or sets
``mca_base_param_files`` to ``none`` - reads other files, or none. A
parameter file is therefore the policy for the processes started
without such settings: the host's servers, when the administrator
starts them or controls their environment. To make a component
unavailable to every process of an installation, build the
installation without it, or remove its plugin (below).

A process running as root reads a parameter file only if root owns it
and no other user or group can write to it.

See :doc:`/mca` for the file format and :ref:`label-running-selecting-framework-components`
for the include and exclude syntax.

3. Remove the component's plugin
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If PMIx was configured with ``--enable-mca-dso``, each component is a
separate plugin file under ``$prefix/lib/pmix``. Deleting a plugin
makes that component unavailable - for example, a component that was
built by mistake:

.. code-block:: sh

   shell$ rm $prefix/lib/pmix/pmix_mca_psec_ssl.so

Components built into the library itself (the default) cannot be
removed this way. Use the parameter file instead.

``pmix_info`` lists the components an installation can load:

.. code-block:: sh

   shell$ pmix_info | grep psec

4. Tune the connection checks
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

These MCA parameters adjust the checks PMIx makes on connections:

.. list-table::
   :header-rows: 1
   :widths: 38 12 50

   * - Parameter
     - Default
     - Effect
   * - ``psec_native_legacy_auth``
     - ``false``
     - When ``true``, ``native`` accepts a peer connecting over TCP on
       the identity it gives, on a platform whose kernel has no table of
       TCP connections to confirm it with (any but Linux, macOS and
       FreeBSD),
       instead of refusing it.
   * - ``ptl_base_legacy_peers``
     - ``false``
     - When ``true``, a server takes clients and tools of PMIx releases
       before v7.0, which connect over TCP; a host's
       ``PMIX_SERVER_LEGACY_PEERS`` overrides it. Older clients need it -
       and, on a platform other than Linux, macOS or FreeBSD,
       ``psec_native_legacy_auth`` as well. A PMIx configured
       ``--disable-legacy-peers`` refuses it.
   * - ``ptl_base_if_include`` / ``ptl_base_if_exclude``
     - none
     - Network interfaces the server may or may not listen on. Keep
       loopback included.
   * - ``ptl_base_connect_ack_timeout``
     - 5
     - Seconds a server allows for a new connection's opening message.
   * - ``ptl_base_max_pending_connections``
     - 0
     - How many connections may still be sending their request, and,
       separately, how many tools may wait for the host's decision. 0
       means an eighth of the process's file-descriptor limit, for each;
       -1 means no limit.
   * - ``ptl_base_max_msg_size``
     - 0
     - Largest message accepted, in megabytes. 0 means no limit.
   * - ``ptl_base_server_access``
     - none
     - Servers a client or tool will talk to on its host besides one
       running as its own user or as root, when no server started it:
       ``users=<list>,groups=<list>``, each list user or group names or
       numbers separated by ``:``. Over TCP only ``users`` can match: the
       kernel records no group for a TCP connection. A name the system
       does not know, an empty list, or a directive given twice makes
       the process fail to initialize.
   * - ``psec_ssl_ca_file``, ``psec_ssl_cert_file``,
       ``psec_ssl_key_file``, ``psec_ssl_crl_file``,
       ``psec_ssl_max_skew``
     - (none), 300
     - Certificates and limits for ``ssl``. ``ssl`` is not used unless
       a CA file, or a certificate and key, is given. A process and a
       server on different hosts each need all three: a certificate and
       key to prove who they are, and the CA the other's certificate is
       issued by. A server's certificate must allow server
       authentication, and a tool's client authentication.

To see every parameter and its current value:

.. code-block:: sh

   shell$ pmix_info --param psec all
   shell$ pmix_info --param ptl all

Some policies are set by the host environment rather than by MCA
parameters, because they depend on how the host is deployed:

* **Whether tools of other users may connect.** By default a server
  refuses a tool whose user is not the server's own. A host can allow
  them (``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``), and must then also widen
  the server's local socket to them (``PMIX_SOCKET_MODE``, with
  ``PMIX_GRPID`` for a group): by default only the server's user can
  connect to it.
* **Whether processes on other nodes may connect.** By default a
  server takes no TCP connection at all, and when it takes clients and
  tools of earlier releases it listens for them on loopback only. In a
  PMIx configured
  ``--enable-remote-tools`` - which also requires ``--with-munge`` or
  ``--with-openssl`` - a host can listen on other interfaces
  (``PMIX_SERVER_REMOTE_CONNECTIONS``); without that option, a host that
  asks fails its init. A process on another node and the server
  authenticate each other with ``munge`` or ``ssl``, the server first:
  the server proves who it is before the process sends its connection
  request, and each credential is good for that one connection. With
  ``ssl``, the user a server's certificate names must exist on the
  process's host, with the same uid, and hold the server's group there,
  as a process's must on the server's host. The process refuses
  a server that does not prove who it is, or proves it is a user the
  process may not talk to (its own, root, or one
  ``ptl_base_server_access`` names). The server refuses a process on
  another node that does not take part - one of an earlier release, or
  one with only ``native`` - and a server asked for remote connections
  that cannot take part fails its init.
* **Clients are always checked** against the user and group the host
  registered them with. No setting changes that.

Consult your host environment's documentation for how it exposes these
choices.


Temporary files
---------------

A PMIx server writes rendezvous files, session directories and its
local socket under its temporary directory (``$TMPDIR``, or ``/tmp``).
Tools find servers through those files. The local socket is in a
directory of its own, ``pmix-XXXXXX``. A server that is killed can leave
its files behind; PMIx reclaims a rendezvous file whose server is gone,
but session directories and socket directories accumulate. Clean out old
``pmix*`` entries as you would other stale temporary files.

Keep the temporary directory's path short: the local socket's path is
built under it, and is limited to about 100 characters. A server whose
path is too long fails to start. The default ``$TMPDIR`` on macOS is too
long - start the host environment with a shorter one.

See :doc:`/how-things-work/session_dirs`.
