Access control by user and group
================================

.. note:: This is a **plan**. It describes the access model PMIx is
          moving to, and the order in which the pieces are being
          implemented. The `Status`_ section at the end says which
          parts are in place today. Anything not yet implemented
          behaves as it did before this work began.

This page is for two audiences:

* **Users and administrators**, who need to know who can see a job's
  data and act on a job, and how to widen that.
* **Developers** of PMIx and of host environments, who need to know
  where each check is made and what a host has to do.

Summary
-------

* Access to a job - reading its data, pulling its output, monitoring
  or signalling its processes - is decided by **who is asking** (their
  user ID and groups), not by which job the requester belongs to.
* By default, **only the job's owner** can access it. The owner can
  widen that when the job is started, by naming other users and/or
  groups with ``PMIX_ACCESS_PERMISSIONS``.
* Crossing jobs is normal and supported. A debugger or tool that asks
  about another job succeeds whenever its user is allowed.
* These rules govern requests that **clients and tools** make of a PMIx
  server. They do not restrict the **host environment** itself - see
  below.

Who is governed: the host versus clients and tools
--------------------------------------------------

A PMIx server lives inside a *host environment*: a resource manager,
launcher, or other system daemon that calls ``PMIx_server_init`` and
supplies the ``pmix_server_module_t`` up-calls. Many hosts run as a
privileged service account that is **not** root.

* **The host is not restricted.** The library does not limit what the
  host can do through the server APIs (registering jobs, delivering
  output, answering data requests, and so on), and does not second-guess
  what the host does with a request passed up to it. The host is the
  authority for its own operations.
* **Clients and tools are governed.** A request a client or tool sends
  to a server is checked against the target job's permissions:

  - by the **server library**, for data and operations it handles
    itself;
  - by the **host**, for operations the library passes up to it. For
    these the library tells the host exactly who is asking - the
    requester's process ID, ``PMIX_USERID`` and ``PMIX_GRPID`` (see
    :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`) - and
    the host applies the same rule.

The requester's identity is the one established when it connected to
the server (the connection handshake verifies it against the host's
registration). It is not something the request itself can claim.

Terms
-----

Owner
   The user (and primary group) a job belongs to - normally the user
   who started it.

Requester
   The client or tool making a request, identified by the user ID and
   group ID recorded when it connected.

Access list
   Additional users and groups the owner has allowed, given with
   ``PMIX_ACCESS_PERMISSIONS``.

The rule
--------

A requester may access a job if **any** of the following holds:

#. the requester's user ID is 0 (root);
#. the requester's user ID is the server's own effective user ID - the
   account the host runs as, which can already inspect the server
   process itself;
#. the requester's user ID is the job owner's;
#. the requester's user ID is in the job's access list;
#. the requester belongs to a group in the job's access list - its
   primary group or any supplementary group.

Otherwise the request fails with ``PMIX_ERR_NO_PERMISSIONS``.

Establishing a job's permissions
--------------------------------

**The owner.** The host gives the job's owner when it registers the
job, as ``PMIX_USERID`` and ``PMIX_GRPID`` in the info passed to
``PMIx_server_register_nspace``. Either may be a number or a user or
group name; names are resolved when the job is registered. If the host
does not give them:

* the owner is the user and group the job's clients were registered
  with (``PMIx_server_register_client``);
* failing that, the owner is the server's own user.

A tool's owner is the user and group it presented when it connected.

**The access list.** A user widens access when starting a job, by
passing ``PMIX_ACCESS_PERMISSIONS`` in the job-level directives of
``PMIx_Spawn``. Its value is a data array of ``pmix_info_t`` holding
either or both of:

* ``PMIX_ACCESS_USERIDS`` - a data array of user IDs (numbers or names);
* ``PMIX_ACCESS_GRPIDS`` - a data array of group IDs (numbers or names).

The host passes the list on when it registers the job, on every node
that hosts a part of it. The same attributes already govern access to
published data (``PMIx_Publish``), so users meet one vocabulary.

**Group membership** is looked up once per user and cached, rather than
on every request. The server keeps one table, keyed by user ID, for
every requester - connected or on another node (see `Data held on
another node`_). An entry is made the first time a check needs that
user's groups, and only when the job's access list names groups. It is
refreshed after the interval set by the MCA parameter
``pmix_server_access_group_timeout`` (in seconds; default 300; 0 keeps
entries for the life of the server), so a change to a user's groups
takes effect within that time.

What is covered
---------------

.. list-table::
   :header-rows: 1
   :widths: 28 42 30

   * - Area
     - Examples
     - Checked by
   * - Job-level data
     - ``PMIx_Get`` of another job's data (rank ``PMIX_RANK_WILDCARD``)
     - server library
   * - Process data on this node
     - ``PMIx_Get`` of a process's committed (modex) data
     - server library
   * - Process data on another node
     - ``PMIx_Get`` answered by direct modex
     - the server that holds the data (see below)
   * - Query and resolve
     - process tables, namespace information, ``PMIx_Resolve_peers``
     - server library for what it answers; host for the rest
   * - Output forwarding
     - ``PMIx_IOF_pull``, and a pull following a job into the jobs it
       spawns
     - host approves each pull; library for inheritance
   * - Output files
     - ``PMIX_IOF_OUTPUT_TO_FILE`` / ``_TO_DIRECTORY``
     - file ownership and mode (see below)
   * - Monitoring
     - ``PMIx_Process_monitor`` targets, process statistics
     - server library for processes on its node; host for others
   * - Job control
     - signal, kill, terminate, abort, stdin
     - host; server library for cleanup directives it handles
   * - Sessions and allocations
     - extending, releasing, spawning into an allocation
     - host (by user, not by namespace)

What the server library checks
------------------------------

Beyond the rule itself, a few things hold for every check the server
library makes:

* **A job's own processes** always access their own job.
* **The server's own namespace** describes the server, not anyone's
  job, and anyone may read it.
* **A job the host has not registered with this server** - one known
  here only because data for it arrived from elsewhere - has no
  permissions here to judge by. Requests for its data are checked by
  the server that holds it (see `Data held on another node`_). Acting
  on such a job - cleanup, monitoring, following its output - is
  allowed only for root and the server's own user.

Where the checks are made:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Request
     - Check
   * - ``PMIx_Get``, and a refresh of the client's cached copy
     - The requester must be allowed the target job. A request waiting
       for a job to be registered is checked again once it is.
   * - ``PMIx_Query_info`` naming a namespace (``PMIX_NSPACE``)
     - The requester must be allowed that job.
   * - ``PMIx_Resolve_peers``, ``PMIx_Resolve_nodes``
     - Naming a job, the requester must be allowed it. Naming none, the
       answer includes only the jobs the requester may access.
   * - ``PMIx_Process_monitor`` of processes on this node
     - Processes the request names must all belong to jobs the
       requester may access, or the request is refused. A request for
       every local process leaves out those the requester may not
       access. Requests from the host are not restricted.
   * - Cleanup directives (``PMIX_REGISTER_CLEANUP`` and its family)
       on ``PMIx_Job_control``
     - The cleanup runs as the target job's user, so the requester must
       be allowed every job it names.
   * - Output forwarding inherited by a spawned job
     - A subscription to a job's output follows into the jobs it
       spawns only if its requester may access each of them.

Data held on another node
-------------------------

When a requester asks for data the local server does not have, the
server asks its host, which fetches it from the server that holds it
(*direct modex*). The check is made **by the server that holds the
data**, because that server has the job's permissions:

* The requesting server gives its host the requester's identity in the
  ``direct_modex`` up-call.
* The host passes that identity to the holding server through
  ``PMIx_server_dmodex_request2``, a new form of
  ``PMIx_server_dmodex_request`` that takes an info array (every PMIx
  API carries one).
* The holding server applies the rule and answers, or refuses with
  ``PMIX_ERR_NO_PERMISSIONS``. Its answer also carries the job's owner
  and access list, so the requesting server can apply the same rule to
  later requests it answers from its copy.

A host that still uses ``PMIx_server_dmodex_request`` passes no
requester identity, and the holding server answers as it does today. A
capability flag tells a host whether the library provides
``PMIx_server_dmodex_request2``.

Output files and shared memory
------------------------------

**Output files.** Output written to files is owned by the job's owner
where the server can arrange it, and is created with mode 0600. If the
access list names groups, the files are made readable by the first
listed group, where the server can set that group. File modes can
express only an owner and one group, so further users and groups in
the list are not applied to files. Access control lists are not used,
as they are not portable.

**Shared-memory job data** (``gds/shmem3``). Its backing files belong
to the job's owner and group and are not readable by anyone else. A
requester allowed by the access list but not by the file mode receives
the job's data from the server, where the rule is applied, rather than
by attaching the shared segment.

Not covered
-----------

* **Event notification.** The process raising an event chooses who
  receives it through the event's range.
* **Published data** (``PMIx_Publish`` / ``PMIx_Lookup``). The host's
  data server already governs it per published item with
  ``PMIX_ACCESS_PERMISSIONS``.

Compatibility
-------------

* **A host that registers no owner** gets the fallback above: the user
  its clients were registered with, then the server's own user. On a
  node that hosts none of a job's processes there are no clients, so
  there only root and the server's own user can access that job. A host
  running as the same user as its jobs sees no change; a host running
  as a service account should register each job's owner.
* **A host that does not pass ``PMIX_ACCESS_PERMISSIONS``** at
  registration gets owner-only access for every job, which is the
  default.
* **Clients and tools of any version** are identified by their
  connection, so nothing changes for them.
* **Hosts** can detect the library's support through capability flags
  in ``pmix_version.h``: ``PMIX_CAP_REQUESTER_ID`` (the requester's
  identity on up-calls) is present now; flags for access control and for
  ``PMIx_server_dmodex_request2`` will be added with those features.

What a host needs to do
-----------------------

* Register each job's owner (``PMIX_USERID``, ``PMIX_GRPID``) and access
  list (``PMIX_ACCESS_PERMISSIONS``) with ``PMIx_server_register_nspace``
  on every node that hosts part of the job.
* Accept ``PMIX_ACCESS_PERMISSIONS`` in spawn requests and carry it with
  the job.
* Apply the rule to the operations it performs for clients and tools -
  job control, abort, stdin, IOF pull approval, queries it answers,
  monitoring of other nodes, sessions and allocations - using the
  requester identity the library passes in each up-call.
* Carry the requester's identity when it relays a request to another of
  its daemons.
* Answer direct-modex requests with ``PMIx_server_dmodex_request2``,
  passing the requester's identity.

PRRTE is the reference host, and implements these alongside the library
changes.

Status
------

.. list-table::
   :header-rows: 1
   :widths: 8 62 30

   * - Phase
     - Work
     - State
   * - 0
     - The requester's process ID, ``PMIX_USERID`` and ``PMIX_GRPID`` are
       passed on every up-call made for a client or tool; ``PMIX_REQUESTOR``
       where an up-call has no process argument; user and group names
       accepted; IOF pulls forwarded only after the host approves.
       PRRTE approves an IOF pull only for root, the DVM's user, or the
       job's owner.
     - Done
   * - 1
     - Each job's owner and access list stored by the server library,
       from the host's registration; one check implementing the rule;
       group membership cached per user.
     - Done
   * - 2
     - The server library applies the rule to what it answers itself:
       job and process data, query and resolve, monitoring of local
       processes, cleanup directives, and IOF inheritance.
     - Done (in review)
   * - 3
     - ``PMIx_server_dmodex_request2``; the requester's identity on the
       ``direct_modex`` up-call; the check at the holding server. PRRTE
       registers each job's owner (``PMIX_USERID``, ``PMIX_GRPID``) with
       ``PMIx_server_register_nspace``.
     - Planned
   * - 4
     - Output file ownership and modes; server-mediated job data for
       requesters the shared-memory file mode does not admit.
     - Planned
   * - 5
     - PRRTE: records and distributes each job's access list,
       accepts ``PMIX_ACCESS_PERMISSIONS`` at spawn, applies the rule to
       the operations it performs, carries requester identity on
       relays, and uses ``PMIx_server_dmodex_request2``.
     - Planned
