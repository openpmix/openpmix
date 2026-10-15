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
     - ``true``
     - When ``false``, ``native`` refuses a peer from a release older
       than v7.0 whose identity the kernel cannot confirm, instead of
       accepting the identity the peer gives.
   * - ``psec_native_socket_dir``
     - server's temporary directory
     - Where ``native`` creates the local socket a containerized peer
       uses to prove its identity. The directory must be visible inside
       the container.
   * - ``ptl_base_if_include`` / ``ptl_base_if_exclude``
     - none
     - Network interfaces the server may or may not listen on. Keep
       loopback included.
   * - ``ptl_base_connect_ack_timeout``
     - 5
     - Seconds a server allows for a new connection's opening message
       and each handshake step.
   * - ``ptl_base_max_pending_connections``
     - 0
     - How many connections may wait to be accepted. 0 means a quarter
       of the process's file-descriptor limit; -1 means no limit.
   * - ``ptl_base_max_msg_size``
     - 0
     - Largest message accepted, in megabytes. 0 means no limit.
   * - ``psec_ssl_ca_file``, ``psec_ssl_cert_file``,
       ``psec_ssl_key_file``, ``psec_ssl_crl_file``,
       ``psec_ssl_max_skew``
     - (none), 300
     - Certificates and limits for ``ssl``. ``ssl`` is not used unless
       a CA file, or a certificate and key, is given.

To see every parameter and its current value:

.. code-block:: sh

   shell$ pmix_info --param psec all
   shell$ pmix_info --param ptl all

Some policies are set by the host environment rather than by MCA
parameters, because they depend on how the host is deployed:

* **Whether tools of other users may connect.** By default a server
  refuses a tool whose user is not the server's own. A host can allow
  them (``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``).
* **Whether processes on other nodes may connect.** By default a
  server listens only on loopback. A host can listen on other
  interfaces (``PMIX_SERVER_REMOTE_CONNECTIONS``), which also requires
  ``munge`` or ``ssl``. Restricting ``psec`` to ``native`` keeps every
  peer on the same node, since ``native`` refuses peers on other nodes.
* **Clients are always checked** against the user and group the host
  registered them with. No setting changes that.

Consult your host environment's documentation for how it exposes these
choices.


Temporary files
---------------

A PMIx server writes rendezvous files and session directories under
its temporary directory (``$TMPDIR``, or ``/tmp``). Tools find servers
through those files. A server that is killed can leave its files
behind; PMIx reclaims a rendezvous file whose server is gone, but
session directories accumulate. Clean out old ``pmix*`` entries as you
would other stale temporary files. Keep the temporary directory's path
short: it forms part of a socket path, which is limited to about 100
characters.

See :doc:`/how-things-work/session_dirs`.
