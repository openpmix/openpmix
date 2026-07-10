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
             'value': {'value': "SERVER", 'val_type': PMIX_STRING}}]
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
  server is to place client rendezvous points and contact information.
* ``PMIX_SYSTEM_TMPDIR`` (char\*) |mdash| temporary directory in which the
  server is to place tool rendezvous points and system-level contact
  information.

Role and support attributes
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_SERVER_TOOL_SUPPORT`` (bool) |mdash| the host is willing to accept
  connection requests from tools.
* ``PMIX_SERVER_SYSTEM_SUPPORT`` (bool) |mdash| the host is willing to accept
  connections from system-level tools (a system-level rendezvous point is
  established).
* ``PMIX_SERVER_SESSION_SUPPORT`` (bool) |mdash| the host is willing to accept
  connections from session-level tools.
* ``PMIX_SERVER_GATEWAY`` (bool) |mdash| the server is acting as a gateway that
  can relay operations (e.g., logging) requiring routing to other nodes.
* ``PMIX_SERVER_SCHEDULER`` (bool) |mdash| the server is hosting the system
  scheduler (workload manager) and expects to receive scheduler-related
  requests.
* ``PMIX_SERVER_SYS_CONTROLLER`` (bool) |mdash| the server is hosting the
  system controller.
* ``PMIX_SERVER_REMOTE_CONNECTIONS`` (bool) |mdash| allow (or disable)
  connections from remote tools.

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

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


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


.. seealso::
   :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_register_client(3) <man3-PMIx_server_register_client>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
