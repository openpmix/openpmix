Host Environments and PMIx v7
=============================

For developers of resource managers, launchers, schedulers, and other
programs that embed the PMIx server library. PRRTE v5 already does
everything listed here, and serves as an example; the contract applies
to any host.

This page summarizes. The detail is in :ref:`label-host-env`, which
each section links to. A system scheduler should also read
:doc:`schedulers`.


The shape of the change
-----------------------

* **The division of work is written down.** PMIx authenticates each
  connection and makes the inexpensive first checks. **The host is the
  final arbiter** of every connection and every operation passed up to
  it. See :doc:`/hosts/partnership`.
* **PMIx tells the host who is asking** on every up-call made for a
  client or tool, and leaves the decision to the host.
* **A host that has not been updated fails safe.** The identity in an
  up-call is always that of the process connected to the server, as
  the kernel reported it. A host that authorizes on it judges the
  process actually there; a relayed claim arrives beside it, not in
  its place.


Building
--------

* **Build the host against the PMIx major release it runs with.** The
  library is given only a pointer to the host's
  ``pmix_server_module_t``, and cannot tell which release's definition
  of it the host was built with.
* **New module entries since v5.0:** ``tool_connected2``, ``log2``,
  ``abort2``, and ``resource_block``. The older ``tool_connected``,
  ``log``, and ``abort`` are called only when the new ones are
  ``NULL``. See :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`.
* **Capability flags** in ``pmix_version.h`` say what this library
  does, for a host's ``configure`` to test: for example
  ``PMIX_CAP_REQUESTER_ID``, ``PMIX_CAP_ORIGIN_ID``,
  ``PMIX_CAP_DMODEX_REQUEST2``, ``PMIX_CAP_ACCESS_CHECK``, and
  ``PMIX_CAP_DEREG_DEPARTS``.
* **A host that uses PMIx's internal headers**, as PRRTE does, has no
  compatibility guarantee across release series (:doc:`/admin/prrte`).


Starting the server
-------------------

* **Ask for tool support** (``PMIX_SERVER_TOOL_SUPPORT``) if tools are
  to connect. Without it, none can.
* **Decide on clients and tools of earlier releases.**
  ``PMIX_SERVER_LEGACY_PEERS`` takes them, if PMIx was built
  ``--enable-legacy-peers``; the site's ``ptl_base_legacy_peers`` does
  the same when the host does not say. Without either, they are
  refused, and an older client runs as a singleton.
* **Remote connections are gone.** ``PMIX_SERVER_REMOTE_CONNECTIONS``
  set to true fails init.
* **The local socket is owner-only.** To take tools of other users,
  pass ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` *and* widen the socket
  (``PMIX_SOCKET_MODE``, with ``PMIX_USERID`` and ``PMIX_GRPID`` naming
  its owner and group). A tool of the user named as ``PMIX_USERID`` is
  taken without ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS``, as root's and the
  server's own are.
* **Use a short temporary directory.** The server refuses one another
  user could change, and a socket path is limited to about 100
  characters.
* **A session-level server must name its session**
  (``PMIX_SESSION_RNDZ_ID`` or ``PMIX_SESSION_ID``).
* **Reporting the address** (``PMIX_TCP_REPORT_URI``): ``-`` and ``+``
  now give the local socket's address; ``tcp-`` and ``tcp+`` the TCP
  address. A pipe gets both when the server listens on TCP, TCP first.
* **After a failed init**, do not call ``PMIx_server_finalize``. The
  server library does not change the host's own environment.


Registering jobs and processes
------------------------------

* **Register each job's owner** (``PMIX_USERID``, ``PMIX_GRPID``) with
  ``PMIx_server_register_nspace``, on every node that hosts part of the
  job. A host running as a service account must: otherwise the owner
  falls back to the clients' user, and on a node with none of the
  job's processes to the server's own user.
* **Register an access list** (``PMIX_ACCESS_PERMISSIONS``) to widen
  access beyond the owner, and accept one in spawn requests.
* **Give users' groups where the kernel cannot.** A process's groups
  come from the kernel for the local socket. For a request relayed
  from another node, a TCP peer, or a platform where the kernel gives
  no list, PMIx uses the groups the host gives with
  ``PMIX_USER_GRPIDS`` - at ``register_nspace`` for the job's owner, or
  ``PMIx_server_register_resources`` for another user. PMIx never
  looks groups up itself.
* **Prepare every local process before starting it**, with
  ``PMIx_server_register_client`` and ``PMIx_server_setup_fork``. The
  environment ``setup_fork`` produces carries the local socket's
  address and a connection token for that process; the token is bound
  to the first process that connects with it.
* **Re-registering a client** updates it and returns ``PMIX_UPDATED``.
  It used to return ``PMIX_ERR_DUPLICATE_KEY``.
* **Sessions** can be registered and deregistered on their own
  (``PMIx_server_register_session``,
  ``PMIx_server_deregister_session``). Registering a known session
  again updates it.
* **A rank the host deregisters leaves its job's collectives**, and is
  counted back in if the host restarts it under the same rank
  (``PMIX_CAP_DEREG_DEPARTS``).


Accepting connections
---------------------

* **What the host is told.** ``client_connected2`` and
  ``tool_connected2`` receive the verified user and group, the
  authentication method, the kernel's pid for the process, and
  ``PMIX_TIMEOUT``.
* **What the host checks.** What PMIx cannot: that the host started
  this client, with this pid; that this tool's user may do what it
  asks.
* **Answer once, within the timeout.**
* **A tool may arrive with no namespace.** Assign one, and keep
  assigned names unique. Do not register a job under the name a tool
  gave itself.
* **Handle** ``PMIX_ERR_LOST_CONNECTION``. It is raised for a process
  that leaves without finalizing, one accepted that never connected,
  and one that execs.
* **Client clones are gone** (``PMIX_ALLOW_CLIENT_CLONES``): one
  connection per rank.

See :doc:`/hosts/connections`.


Authorizing operations
----------------------

* **Every up-call for a client or tool carries** ``PMIX_USERID`` and
  ``PMIX_GRPID`` - always the connected peer's verified ids - and
  ``PMIX_REQUESTOR_PID``.
* **A relayed request** carries the original requester's claimed
  identity in separate attributes (``PMIX_ORIGIN_*``), and a group the
  requester asks to be charged to as ``PMIX_REQUESTED_GRPID``. The host
  decides whether to believe them.
* **The library no longer relays on its own.** A spawn or query that
  a peer sends to a server embedded in a launcher goes to the host's
  up-call. With no up-call, a query is refused; a spawn is started by a
  launcher attached to nothing, only for a requester of its own user.
* **Direct modex**: use ``PMIx_server_dmodex_request2``, which carries
  the requester; the server that holds the data applies the access
  rule.
* **The access rule as a function.** A host that wants exactly the
  library's rule for its own operations calls
  ``pmix_server_access_check()``.
* **Events raised by clients** reach the host with the requester's
  identity. An event at namespace range with no targets goes only to
  the source's own job.

See :doc:`/hosts/authorization` and :doc:`/hosts/operations`.


Data, output, and shared memory
-------------------------------

* **PMIx applies the access rule to what it answers itself**: job and
  process data, queries and resolves, monitoring of local processes,
  and output it forwards. The host applies it to its up-calls.
* **Output files are created as the job's owner.** PMIx does the file
  work as that user. A file owned by anyone else is not overwritten,
  and is reported once per job. A server that cannot act as the owner
  leaves the files to the launcher (``PMIX_IOF_FILE_WRITER``).
* **Shared-memory job data** is read-only to clients and owned by the
  job's owner. A process the file permissions do not admit fetches
  from the server instead.
* **A commit carrying a reserved** ``pmix`` **key** (other than
  ``PMIX_QUALIFIED_VALUE``) is refused whole.
* ``PMIx_server_deregister_resources`` honors its qualifiers.

See :doc:`/hosts/access-control`.


Up-call return codes and groups
-------------------------------

* **Returning** ``PMIX_OPERATION_SUCCEEDED`` from ``spawn``,
  ``lookup``, ``direct_modex``, ``get_credential``, or
  ``validate_credential`` is treated as an error.
* **A group ID** must be unique, non-empty, and may not name a job the
  server knows. A group controller should refuse a duplicate with
  ``PMIX_ERR_EXISTS``.
* **Joining a group construct** that is still collecting must be the
  same operation, and - when the first call listed every participant -
  by one of them.
* **Participant checks** apply to clients of every release.
* **A monitor's alert** may raise only its own code, a monitor code, or
  a user code; it now carries the monitor's ID and settings and the
  heartbeats or checks missed.
