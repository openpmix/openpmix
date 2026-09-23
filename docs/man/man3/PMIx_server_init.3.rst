.. _man3-PMIx_server_init:

PMIx_server_init
================

.. include_body

``PMIx_server_init`` |mdash| Initialize the PMIx server support library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_init(pmix_server_module_t *module,
                                  pmix_info_t info[], size_t ninfo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_SERVER_NSPACE,
             'value': "SERVER", 'val_type': PMIX_STRING}]
  # map is a dictionary of server-module-function keys to Python
  # callback implementations (e.g. {'clientconnected': myconnfn, ...})
  rc = foo.init(pydirs, map)


INPUT PARAMETERS
----------------

* ``module``: Pointer to a :ref:`pmix_server_module_t(5)
  <man5-pmix_server_module_t>` structure containing the
  host environment's callback functions |mdash| the "function-shipped" server
  module through which client requests are relayed to the resource manager.
  Any function the host does not support is indicated by leaving its function
  pointer ``NULL``; client calls to an unsupported operation return a
  "not supported" error. Passing ``NULL`` for ``module`` (or an empty
  structure) is permitted and indicates that the host will not support
  multi-node operations such as :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`, but
  still intends to give local clients access to job information.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify server initialization (see
  `DIRECTIVES`_). A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


DESCRIPTION
-----------

Initialize the PMIx server support library and register the host environment's
callback module. ``PMIx_server_init`` is called by the process that hosts the
PMIx server |mdash| typically a resource manager daemon, launcher, or tool
that will accept client and/or tool connections.

The PMIx server provides a *function-shipping* approach to the server side of
the protocol: each client-visible PMIx operation is mirrored by an entry in the
``pmix_server_module_t`` structure. When a client request arrives, the PMIx
server library invokes the corresponding host function so that resource
managers can implement server behavior without being burdened with PMIx
internal details. For performance and scalability, host module functions are
required to return quickly and to execute their work asynchronously, completing
each operation by invoking the callback function supplied to them.

Ownership rules for the module functions are important: all data passed *to* a
host server function is owned by the PMIx server library and must not be freed
by the host, while data returned by the host via a module callback is owned by
the host and may be released once the callback returns.

The ``info`` array supplies additional information the server may need when
initializing |mdash| for example, the namespace and rank to assign to the
server itself, the temporary directory in which to place the rendezvous socket,
or the ``PMIX_SERVER_TOOL_SUPPORT`` directive announcing that the daemon is
willing to accept connection requests from tools (see `DIRECTIVES`_).

The PMIx server library is reference counted. Repeated calls to
``PMIx_server_init`` are permitted and simply increment the reference count;
if a ``module`` is provided on a later call and none had been set previously,
the library adopts it. Each successful call must be balanced by a matching call
to :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`.

``PMIx_server_init`` is a blocking call that completes synchronously; it does
not take a callback.


DIRECTIVES
----------

The following attributes are relevant to this operation. Support for any given
attribute is optional and depends on how the PMIx implementation was built and
on the capabilities of the host environment. All are passed in the ``info``
array.

Server identity and directories
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_SERVER_NSPACE`` (char\*) |mdash| namespace to assign to this PMIx
  server.
* ``PMIX_SERVER_RANK`` (pmix_rank_t) |mdash| rank to assign to this PMIx server.
* ``PMIX_SERVER_TMPDIR`` (char\*) |mdash| temporary directory in which the
  server is to place its session-level files, including the rendezvous files
  tools use to find it by PID or namespace. If not given, the
  ``PMIX_SERVER_TMPDIR`` environment variable is used, then ``TMPDIR``,
  ``TEMP`` and ``TMP``, and finally ``/tmp``.
* ``PMIX_SYSTEM_TMPDIR`` (char\*) |mdash| temporary directory in which the
  server is to place its system-level rendezvous files (system server,
  scheduler, system controller). If not given, the ``PMIX_SYSTEM_TMPDIR``
  environment variable is used, then the same fallbacks as for
  ``PMIX_SERVER_TMPDIR``.

Role and support attributes
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_SERVER_TOOL_SUPPORT`` (bool) |mdash| the host is willing to accept
  connection requests from tools, and rendezvous files naming the server by PID
  and by namespace are written (see `Rendezvous files`_). The files are readable
  only by the host's user ID, and tools running under other user IDs are
  refused, unless ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` is also given.
* ``PMIX_SERVER_SYSTEM_SUPPORT`` (bool) |mdash| the server is the node's
  system server: tools asking for the system server find it through a
  system-level rendezvous file. Takes effect only together with
  ``PMIX_SERVER_TOOL_SUPPORT``. As for that attribute, the file is readable
  only by the host's user ID unless ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` is
  given.
* ``PMIX_SERVER_SESSION_SUPPORT`` (bool) |mdash| the server is the session
  server: tools asking for one find it through a session-level rendezvous file.
  Takes effect only together with ``PMIX_SERVER_TOOL_SUPPORT``.
* ``PMIX_SERVER_GATEWAY`` (bool) |mdash| the server is acting as a gateway that
  can relay operations (e.g., logging) requiring routing to other nodes.
* ``PMIX_SERVER_SCHEDULER`` (bool) |mdash| the server is hosting the system
  scheduler (workload manager) and expects to receive scheduler-related
  requests.
* ``PMIX_SERVER_SYS_CONTROLLER`` (bool) |mdash| the server is hosting the
  system controller.
* ``PMIX_SERVER_REMOTE_CONNECTIONS`` (bool) |mdash| allow (or disable)
  connections from remote tools. When allowed, the server listens on every
  non-loopback interface left by ``PMIX_TCP_IF_INCLUDE`` /
  ``PMIX_TCP_IF_EXCLUDE``. Its URI names the first; the others are stored as
  ``PMIX_MYSERVER_ALT_URIS`` and listed in its contact files, where tools
  finding the server by file, PID or namespace try them if the URI's address
  cannot be reached.
* ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` (bool) |mdash| mark the rendezvous
  files as readable by all users and allow tools running under user IDs other
  than that of the server to connect. Defaults to ``false``: without it, a
  connection request from a tool under another user ID is refused. The host
  retains ultimate authority over such connections and may restrict what
  foreign tools are permitted to do (for example, limiting them to queries) as
  dictated by host policy. The permissions of an existing directory the files
  are placed in are never changed, so a directory other users cannot search
  still keeps their tools out (see `Rendezvous files`_).
* ``PMIX_ALLOW_CLIENT_CLONES`` (bool) |mdash| allow connections from clones
  (forks) of a registered client process.

Connection attributes
^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_LAUNCHER_RENDEZVOUS_FILE`` (char\*) |mdash| full pathname of an
  additional rendezvous file in which the server is to write its contact
  information |mdash| for example, so that a debugger can find a launcher.
  Missing directories on the path are created.
* ``PMIX_TCP_REPORT_URI`` (char\*) |mdash| report the server's URI: ``-``
  writes it to stdout, ``+`` to stderr, an integer writes it to that open file
  descriptor (which is then closed), and anything else is taken as the name of
  a file to write it to.
* ``PMIX_TCP_IF_INCLUDE`` (char\*) |mdash| comma-delimited list of devices
  and/or CIDR notation to listen on. Cannot be combined with
  ``PMIX_TCP_IF_EXCLUDE``.
* ``PMIX_TCP_IF_EXCLUDE`` (char\*) |mdash| comma-delimited list of devices
  and/or CIDR notation not to listen on.
* ``PMIX_TCP_IPV4_PORT`` (int or char\*) |mdash| IPv4 port to listen on, or
  (as a string) a comma-delimited list of ports and ranges to try in order.
  ``0`` or ``-1`` lets the kernel choose.
* ``PMIX_TCP_IPV6_PORT`` (int or char\*) |mdash| as ``PMIX_TCP_IPV4_PORT``,
  for IPv6.
* ``PMIX_TCP_DISABLE_IPV4`` (bool) |mdash| do not listen on IPv4 addresses.
* ``PMIX_TCP_DISABLE_IPV6`` (bool) |mdash| do not listen on IPv6 addresses.

Topology and behavior attributes
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_TOPOLOGY2`` (pmix_topology_t) |mdash| pointer to an
  implementation-specific description of the local node topology.
* ``PMIX_SERVER_SHARE_TOPOLOGY`` (bool) |mdash| the server is to scalably
  expose the node topology to its local clients (for example, via shared-memory
  backing stores), cleaning up any artifacts at finalize.
* ``PMIX_HOMOGENEOUS_SYSTEM`` (bool) |mdash| the nodes in the system are
  topologically identical, so the server need not compute or exchange
  per-node topology descriptions.
* ``PMIX_SERVER_ENABLE_MONITORING`` (bool) |mdash| enable the internal
  monitoring capabilities of the PMIx server.
* ``PMIX_EXTERNAL_PROGRESS`` (bool) |mdash| the host will progress the PMIx
  library as needed via calls to ``PMIx_Progress`` rather than PMIx spawning
  its own internal progress thread.
* ``PMIX_SINGLETON`` (char\*) |mdash| the server is operating in support of a
  singleton process; the value is the ``nspace.rank`` string of that singleton.
* ``PMIX_IOF_LOCAL_OUTPUT`` (bool) |mdash| write output streams to the server's
  own stdout/stderr.
* ``PMIX_BIND_PROGRESS_THREAD`` (char\*) |mdash| comma-delimited ranges of CPUs to
  which the internal PMIx progress thread is to be bound.
* ``PMIX_BIND_REQUIRED`` (bool) |mdash| return an error if the internal PMIx
  progress thread cannot be bound as requested.
* ``PMIX_EXTERNAL_AUX_EVENT_BASE`` (void\*) |mdash| pointer to an ``event_base``
  the library is to use for auxiliary functions (e.g., capturing signals) that
  would otherwise interfere with the host.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding
to a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx server library could not be initialized
  (for example, a required transport was explicitly disabled, or a tight race
  between concurrent ``PMIx_server_init`` calls left the library in an unusable
  state).
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| a provided directive requested behavior the
  implementation does not support.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a directive or an environmental value carried
  something the implementation cannot use |mdash| for example a
  ``PMIX_SINGLETON`` that is not of the form ``"nspace.rank"``, or a
  ``PMIX_SERVER_RANK`` environment variable that is not a simple non-negative
  number.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.

A failed ``PMIx_server_init`` leaves nothing behind. Anything the call had
already brought up is torn down before the error is returned, so the caller may
correct whatever was rejected and call ``PMIx_server_init`` again. It must
**not** call :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`, which
answers ``PMIX_ERR_INIT`` because no initialization completed. The one
exception is a failure of the underlying runtime itself, which cannot be
unwound; there the library reports the error and is unusable for the life of
the process.


NOTES
-----

``PMIx_server_init`` is intended for use only by processes acting as a PMIx
*server* |mdash| processes that host the server support library on behalf of a
resource manager or launcher. Client processes call
:ref:`PMIx_Init(3) <man3-PMIx_Init>` and tools call
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` instead.

After a successful ``PMIx_server_init``, the host is expected to register each
participating namespace with
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>` and
each local client with
:ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>` before
launching that client, then set up the client's environment with
``PMIx_server_setup_fork``.


Rendezvous files
^^^^^^^^^^^^^^^^

A tool finds a server through a *rendezvous file*: a small file holding the
server's URI, version and PID. Which ones are written depends on the
directives:

.. list-table::
   :header-rows: 1

   * - Directive
     - File
     - Directory
   * - ``PMIX_LAUNCHER_RENDEZVOUS_FILE``
     - the name given
     - as given
   * - ``PMIX_SERVER_SCHEDULER``
     - ``pmix.sched.<hostname>``
     - ``PMIX_SYSTEM_TMPDIR``
   * - ``PMIX_SERVER_SYS_CONTROLLER``
     - ``pmix.sysctrlr.<hostname>``
     - ``PMIX_SYSTEM_TMPDIR``
   * - ``PMIX_SERVER_TOOL_SUPPORT`` with ``PMIX_SERVER_SYSTEM_SUPPORT``
     - ``pmix.sys.<hostname>``
     - ``PMIX_SYSTEM_TMPDIR``
   * - ``PMIX_SERVER_TOOL_SUPPORT`` with ``PMIX_SERVER_SESSION_SUPPORT``
     - ``pmix.<hostname>.tool``
     - ``PMIX_SERVER_TMPDIR``
   * - ``PMIX_SERVER_TOOL_SUPPORT``
     - ``pmix.<hostname>.tool.<pid>`` and ``pmix.<hostname>.tool.<nspace>``
     - ``PMIX_SERVER_TMPDIR``

A scheduler or system controller writes only its own file; the files under
``PMIX_SERVER_TOOL_SUPPORT`` are written by any other server that gives it.

By default each file is readable only by the server's user ID. With
``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` it is also readable by all users. Either
way, the mode is subject to the process umask.

A missing directory is created with mode 0700 (0755 with
``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``), again subject to the umask, and is
removed at finalize. A directory that already exists |mdash| normally the
temporary directory the server was given, or ``$TMPDIR`` |mdash| is used as
found. Its permissions are never changed. So if other users cannot search it,
their tools cannot find the server however the file itself is marked. A host
that wants foreign tools to connect must give the server a directory they can
search.

A file at the same name whose recorded PID is no longer running is removed and
replaced. One whose PID is still running belongs to another server, and
``PMIx_server_init`` fails rather than take it over.

A file named by ``PMIX_TCP_REPORT_URI`` is not a rendezvous file and none of
the above applies to it: it is written with default permissions, subject to the
umask, and removed at finalize.


.. seealso::
   :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
