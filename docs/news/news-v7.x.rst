PMIx v7.x series
================

This file contains all the NEWS updates for the PMIx v7.x
series, in reverse chronological order.

7.0.0 -- TBD
------------
.. important:: This is the first release in the v7 family. As with the
               v6 series, releases are intended to be infrequent
               "reference tags" that mark the completion of some
               significant body of work rather than a regular cadence.

               For this release, that work is fivefold: completion of
               the Group family of APIs with the invite/join construction
               mode (see the
               :ref:`Group Construction <group-construction-label>`
               section of the documentation); a substantial reduction in
               the amount of data PMIx moves during startup, aided by a
               new shared-memory datastore (``gds/shmem3``); a connection
               model in which every client and tool reaches a server on
               its own host over a local socket, where the kernel names
               both ends of the connection; an access model in which
               every job records its owner and the server holding its
               data decides, request by request, who may see it; and a
               systematic review of every subtree under ``src`` that
               closed a long list of crashes, leaks, and use-after-free
               defects.

               Sites, host environments, and tool writers moving from
               an earlier release should read
               :ref:`Moving to PMIx v7 <label-transition-v7>`, which
               describes what changed for each role, and the
               :ref:`settings matrix <label-settings-matrix>`, which
               sets the configure options, MCA parameters, and server
               attributes that a server's behavior depends on against
               one another.

               Proper execution of the group construction modes, and of
               the session and allocation support described below,
               requires that the host provide the necessary backend
               support. Please check with your host provider to ensure it
               is available, or feel free to use the PMIx Reference
               RunTime Environment
               (`PRRTE <https://github.com/openpmix/prrte/releases>`_).

Highlights
^^^^^^^^^^

.. rst-class:: open

* **Group construction by invitation now works end to end,** including
  endpoint exchange, context-id assignment, and invitations that name a
  whole namespace. ``PMIX_CAP_GROUP_JOIN_COMPLETES``.

* **Startup moves far less data.** ``PMIx_Commit`` and the collecting
  fence send only what changed since the last commit rather than the
  entire store, and a get for a process of another job no longer
  resends that job's job-level data when the requester already holds
  it. The wire format is unchanged.

* **New shared-memory datastore.** ``gds/shmem3`` replaces
  ``gds/shmem2``, with lock-free reads and the ability to update the
  data of a running job.

* **Access control by user and group.** Every job records its owner --
  named by the host, or the user its clients run as -- together with an
  optional list of others who may read it. The server holding a job's
  data decides who may have it, answering ``PMIX_ERR_NO_PERMISSIONS``
  otherwise, and a job's output files and shared-memory segments belong
  to its owner. ``PMIX_ACCESS_PERMISSIONS``, ``PMIX_ACCESS_USERIDS``,
  and ``PMIX_ACCESS_GRPIDS`` -- until now accepted only for published
  data -- name who else may read a job when they are given in the
  job-level info of ``PMIx_Spawn`` or
  ``PMIx_server_register_nspace``. A process's groups are those the
  kernel reports for its connection, or those the host gives with the
  new ``PMIX_USER_GRPIDS``. ``PMIX_CAP_ACCESS_CHECK``.

* **Up-calls name the requester.** Every call a server makes on a
  peer's behalf now says who asked. ``PMIX_USERID`` and ``PMIX_GRPID``
  are always those of the connected peer, as its connection established
  them, with ``PMIX_REQUESTOR`` where the up-call's signature cannot
  carry it and ``PMIX_REQUESTOR_PID`` where the kernel reports the
  peer's pid. A relaying host names whom it speaks for with the new
  ``PMIX_ORIGIN_*`` attributes, which are passed up for the receiving
  host to judge, and a requester asks to have work charged to a group
  with ``PMIX_REQUESTED_GRPID``. A spawn up-call says whether a tool or
  a client asked. ``PMIx_server_dmodex_request2`` takes an info array,
  and the new ``abort2`` up-call carries the requester's identity,
  which ``abort`` cannot. ``PMIX_CAP_REQUESTER_ID``,
  ``PMIX_CAP_DMODEX_REQUEST2``, ``PMIX_CAP_ORIGIN_ID``,
  ``PMIX_CAP_ABORT2``.

* **Clients and tools connect over a local socket.** Every server
  listens on an owner-only ``AF_UNIX`` socket, where the kernel reports
  the pid, uid, gid, and groups of each peer, and takes connections
  from its own host only; TCP remains solely for clients, tools, and
  servers of earlier releases (see below). A connecting process offers
  its security, wire-format, and datastore components and the server
  chooses among them; the process then presents a credential and, for
  a client, the connection token ``PMIx_server_setup_fork`` hands its
  rank, which is bound to the first process that presents it. A rank
  has one connection at a time, the host's decision on a connection
  has a deadline, and a client or tool checks who runs the server it
  reached.

* **Servers are found in directories only their user writes.** A
  server's rendezvous files go in ``<system tmpdir>/pmix-user.<uid>``,
  or ``$XDG_RUNTIME_DIR/pmix`` when that directory cannot be used, and
  a root system server, scheduler, or system controller writes to
  ``/run/pmix``. A server holds a lock beside each file while it runs,
  so a search passes over the file of a server that is gone, and a
  tool takes a file only of the user who wrote it. A session-level
  server is named by ``PMIX_SESSION_RNDZ_ID`` or its ``PMIX_SESSION_ID``,
  so servers of different sessions can share a node. Before it starts,
  a server checks that no other user can change its tmpdirs
  (``ptl_base_tmpdir_check``).

* **Data can be deleted.** New ``PMIX_DEL_*`` scopes on ``PMIx_Put``
  remove a key, including from clients that cached it.
  ``PMIX_CAP_DATA_DELETE``.

* **Sessions are first class.** ``PMIx_server_register_session`` and
  ``PMIx_server_deregister_session`` describe a session independently of
  its jobs. ``PMIX_CAP_SESSION_REGISTRATION``.

* **Stdin flow control.** ``PMIx_server_IOF_flow_control`` lets a host
  suspend and resume its stdin source, including across chained
  launchers. ``PMIX_CAP_IOF_FLOW_CONTROL``.

* **Expanded allocation support** through new ``PMIX_ALLOC_*``
  attributes; ``palloc`` can target an existing allocation, and a spawn
  can carry the allocation request it needs.

* **New compression components.** ``zstd`` (preferred when available,
  roughly 6x faster than zlib) and ``lz4``. All nodes in a job must use
  the same component.

* **GPU discovery repaired.** ``pgpu/amd`` and ``pgpu/nvd`` activate on
  the right nodes, and Intel GPUs get a correct ``ZE_AFFINITY_MASK``.
  ``PMIX_CAP_DEVICE_ENUM``.

* **Distances can be measured from a device.**
  ``PMIX_DEVICE_DIST_ORIGIN`` answers questions such as which NICs are
  nearest a given GPU, and distances now account for the PCIe tree.
  ``PMIX_CAP_DEVICE_DIST_ORIGIN``.

* **Connections no longer stall a process.** A silent or unresponsive
  peer can no longer hang a server, client, or tool, and intermittent
  connection failures under spawn-heavy workloads have been fixed. A
  connect to a busy server is retried within the connect's bound, and
  ``PMIX_TIMEOUT`` bounds a tool's connect.

* **Collectives account for departed ranks.** A rank the host
  deregisters, or whose connection is lost, leaves every collective of
  its job exactly once, whichever happens first, and a rank that
  returns is counted back in. A rank that calls more than once, or is
  named twice in a participant list, is counted once.
  ``PMIX_CAP_DEREG_DEPARTS``.

* **A group ID belongs to one group.** A group ID is held by one group
  until it is destructed, and may not name a job the server knows.
  Every caller of a construct or destruct must be a participant and
  name the same participants, and a destruct runs over the recorded
  membership. An invitation is aborted when its leader departs, and a
  get by group rank is answered only for a member of the group.

* **Output files are written as the job's owner.** A server that is
  not the job's owner creates and opens the job's output files in a
  child running as the owner, with the groups the host registered for
  it, and writes them from a thread of its own. A server that cannot
  act as the owner leaves the files to a launcher that names itself
  with ``PMIX_IOF_FILE_WRITER``, or else tells the job once, when it is
  registered, that its files are not written.

* **Spawned jobs' output follows the parent's.** A spawned job's output
  now arrives formatted as its parent's (``PMIX_IOF_JOB_DIRECTIVES``),
  and the ``PMIX_FWD_*`` channels a spawn names are honored on every
  server.

* **Self-started tools keep namespaces of their own.** The namespace a
  tool names for itself is never merged into a job the host registers;
  a host registration of that name takes it back, and the namespace is
  released when its last tool leaves. ``PMIX_CAP_TOOL_NSPACE_RELEASE``.

* **Registering a rank again updates it,** and returns the new success
  status ``PMIX_UPDATED``.

* **New regex interface.** ``PMIx_generate_regex2`` and
  ``PMIx_parse_regex2`` work on a structured ``pmix_regex2_t``; the
  ``preg/native`` component has been removed. ``PMIX_CAP_REGEX2``.

* **New wire format.** ``bfrops/v70`` carries this release's data
  types, and ``bfrops/v61`` again carries exactly the set v6.1.0
  shipped.

* **No more hangs from event handlers.** Blocking APIs called from the
  progress thread now return ``PMIX_ERR_WOULD_BLOCK``.

* **Concurrent gets are answered separately.** A ``PMIx_Get`` shares an
  outstanding request to the server only when it asks for the same key
  of the same process with no directives, so a get for a key a peer
  never posted no longer fails the other gets for that peer.

* **PMIX_ANL_MAP on request.** When the host does not supply it, a
  server computes ``PMIX_ANL_MAP`` from the job's node list and each
  node's local peers, so MPICH need not resolve its peers node by node.

* **Finalize reports its outcome.** ``PMIx_Finalize`` and
  ``PMIx_tool_finalize`` return the host's answer to the finalize
  request, or ``PMIX_ERR_TIMEOUT``, and a server writes out the replies
  it has queued before its progress thread stops.

* **Safer file handling.** Shared-memory, ``hwloc.sm``, and rendezvous
  files are created exclusively and verified before they are reused or
  re-permissioned.

* **Wire data is bounded before it is trusted.** Every count read off
  the wire is checked against the bytes actually received before it
  sizes an allocation, and the datatype of each element is verified
  before it is read -- across ``bfrops``, ``gds``, ``pstat``, ``ptl``,
  the event system, and the client, server, and tool message handlers.

* **Email logging is configurable.** ``plog/smtp`` takes its recipients
  and sender from MCA parameters, accepts ``PMIX_LOG_EMAIL_MSG`` as the
  message body, bounds the SMTP dialogue with ``plog_smtp_timeout``,
  and reports a message as sent only once the server has accepted it.

* **pmix_info fixes.** ``--param`` and ``--show-version`` now honor the
  arguments their usage text documents.

* **MCA parameter files** are parsed by a new line reader, fixing
  several silent misreads (CRLF endings, quoted values, a missing final
  newline).

* **Command-line matching.** The new ``pmix_cli_match()`` resolves an
  input against all of an option's choices and reports ambiguous
  abbreviations. ``PMIX_CAP_CLI_MATCH``.

* **Argv lookup.** ``PMIx_Argv_idx`` returns the position of a string
  in an argv array. ``PMIX_CAP_ARGV_IDX``.

* **Memory binding.** ``PMIX_MEMBIND`` names the memory attribute (for
  example, ``latency`` or ``bandwidth``) by which ``PMIx_Spawn`` selects
  the memory a process is bound to.

* **FreeBSD.** A new ``pstat/pfreebsd`` component samples processes,
  nodes, disks, and interfaces, and ``psec/native`` finds the owner of
  a TCP peer there.

* **Build.** ``flex`` is no longer required, the Python bindings now
  receive the build's optimization and hardening flags, and the tree
  builds warning-free with gcc 16.

* **Documentation.** 277 new man pages cover the public API,
  ``docs/how-things-work`` describes the major subsystems, and the
  security policy now lives in ``SECURITY.md``. New guides cover host
  environments, system administration, and moving from v5 to v7 for
  each role, schedulers included.

* **Testing.** New unit tests, multi-node Docker Swarm suites,
  multi-user output-file tests, AddressSanitizer CI, cross-version CI
  with clients back to v2.1, and a CI build without support for
  earlier releases.

Compatibility notes
^^^^^^^^^^^^^^^^^^^

.. rst-class:: open

* **Out-of-tree MCA components must be rebuilt** against the v7
  headers; framework versions are now checked at load time.
  ``PMIX_CAP_MCA_FW_VERSION``.

* **Earlier releases are supported only when built for.** A default
  build speaks only this release's wire format and has no TCP
  transport. Configure with ``--enable-legacy-clients`` for a server to
  take clients and tools from v2.1 on, then turn it on at run time with
  ``ptl_base_legacy_clients`` or ``PMIX_SERVER_LEGACY_PEERS``; the
  server listens for them on loopback TCP only. Configure with
  ``--enable-legacy-servers`` for a client or tool to connect to
  servers from v3.2 through v6.x. ``--enable-legacy-peers`` and
  ``ptl_base_legacy_peers`` ask for both. A client of an earlier
  release under a server that does not take legacy clients cannot
  connect, and runs as a singleton. Refusals by version report
  ``PMIX_ERR_OUTDATED``.

* **Tools on other hosts are not supported.** A server takes
  connections from its own host only, and a process connects only to a
  server on its own host. ``PMIX_SERVER_REMOTE_CONNECTIONS`` is
  deprecated: true fails ``PMIx_server_init``, and false changes
  nothing.

* **A connecting peer must present a credential.** The handshake-only
  model is gone, and with it the ``psec/none`` and
  ``psec/dummy_handshake`` components. A host or tool that selected
  either must choose ``native`` or ``munge``.

* **One connection per rank.** A second connection for a connected
  rank is refused with ``PMIX_ERR_EXISTS``, and
  ``PMIX_ALLOW_CLIENT_CLONES`` is retired. Every socket the library
  makes is close-on-exec, so an exec ends the process's connection, and
  ``PMIx_Init`` and ``PMIx_tool_init`` remove the variables that
  identify the process and its server from the environment, so a
  process it starts afterwards does not inherit them.

* **Server URIs.** ``PMIX_MYSERVER_URI`` is now always the local
  socket's URI; ``PMIX_SERVER_URI`` is unchanged, and is what a process
  of an earlier release should be given. A TCP URI given alone to a
  tool, an attach, or a launcher is refused with
  ``PMIX_ERR_NOT_SUPPORTED``: name the server's local-socket URI, or
  ``file:<path>``. A server refuses a peer of this release on TCP with
  the new ``PMIX_ERR_LOCAL_URI_REQUIRED``. The ``-`` and ``+``
  report-URI outputs print the local socket's URI, and the new ``tcp-``
  and ``tcp+`` print the TCP URI.

* **No silent singleton fallback.** ``PMIx_Init`` fails with
  ``PMIX_ERR_COMM_FAILURE`` when it cannot reach a server it was told
  about, or when it is given ``PMIX_NAMESPACE`` but no server URI.
  ``PMIX_ERR_UNREACH`` now means no server was described.

* **Connection waits are bounded by default.**
  ``ptl_base_handshake_wait_time`` now defaults to 60 seconds and the
  new ``ptl_base_connect_ack_timeout`` to 5 seconds; set either to 0 for
  an unbounded wait.

* **Registration requirements.** ``PMIx_server_register_nspace``
  refuses job data that does not include ``PMIX_JOB_SIZE``, and a
  client connects only once the host has registered its namespace. A
  session-level server must be given ``PMIX_SESSION_RNDZ_ID`` or
  ``PMIX_SESSION_ID``, and a server given no namespace takes
  ``pmix-server-<host>-<pid>``. A host should not register the
  namespace a self-started tool names.

* **A user's groups come from the host.** The library no longer looks
  a user's groups up. A host supplies them with ``PMIX_USER_GRPIDS``,
  for a job's owner in ``PMIx_server_register_nspace`` and for any
  other user in ``PMIx_server_register_resources``, where a single
  ``PMIX_GRPID`` names one group.

* A scheduler or system controller serves tools unless the host passes
  ``PMIX_SERVER_TOOL_SUPPORT`` false, which now fails the init.

* Group membership keeps the order the host provided, and group ranks
  follow it. ``PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`` is deprecated and
  ignored.

* A blocking ``PMIx_IOF_pull`` now returns the registration id (a
  non-negative value) instead of ``PMIX_OPERATION_SUCCEEDED``.

* ``PMIx_Finalize`` and ``PMIx_tool_finalize`` can return
  ``PMIX_ERR_TIMEOUT``, and ``PMIx_tool_finalize`` returns
  ``PMIX_ERR_WOULD_BLOCK`` when called on the progress thread.

* ``PMIx_Compute_distances`` values differ from earlier releases because
  of the new PCIe term; relative order is otherwise unchanged.

* Directories PMIx creates below a given root are reused only when
  owned by the effective uid, and existing directories PMIx is given are
  used with their permissions as found.

* ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` now defaults to ``false``. Hosts
  that want tools from other users to connect must pass it; PRRTE does.
  Tools of the user the host names as ``PMIX_USERID`` are taken without
  it.

* A command-line option value must now be a leading part of the name it
  matches, so ``packagefoo`` no longer matches ``package``.

* Support for the Solaris, Sun Studio, and KAI toolchains has been
  removed.

* ``PMIX_COMPRESSED_STRING`` is deprecated.

* A server answers a request for a job's data only when the requester
  is permitted it, and returns ``PMIX_ERR_NO_PERMISSIONS`` when it is
  not. A host that registers a namespace for another user should name
  the owner, since a job with no owner named belongs to the user its
  clients run as.

* ``PMIX_USERID`` and ``PMIX_GRPID`` may now be given as a user or
  group name as well as a number. ``palloc`` drops ``--uid`` and
  accepts a group name for ``--gid``.

* A tool looks for a server's rendezvous file in the ``pmix-user``
  directories of the users whose servers it may talk to -- its own,
  root's, and those ``ptl_base_server_access`` names -- and in
  ``/run/pmix``. It walks the system tmpdir, at most
  ``ptl_base_search_depth`` directories deep (default 8), only for a
  server of an earlier release when ``ptl_base_legacy_servers`` is set,
  and matches a file it was told to look for by its whole name rather
  than by prefix.

* New limits: ``psensor_base_max_monitors_per_peer`` and
  ``pstat_base_max_monitors_per_peer`` (each 16 by default) cap the
  periodic monitors one process may hold, ``iof_max_partial_line``
  (64KB) caps how long a partial output line is held before it is
  written out as it stands, and ``pmix_hwloc_max_cpu_index`` bounds the
  processor indices accepted in a cpuset list.

* More limits: ``ptl_base_max_pending_connections`` (a quarter of the
  descriptor limit) and ``ptl_base_max_pending_bytes`` (16MB) bound
  connections still being read; ``pmix_server_max_event_regs_per_peer``,
  ``pmix_server_max_iof_reqs_per_peer``, and
  ``pmix_server_max_invites_per_peer`` (each 1024) cap what one
  connection may hold; ``pmix_max_events_per_source`` (a quarter of
  ``pmix_max_events``) caps one namespace's share of the event cache;
  ``pcompress_base_max_size`` (512MB) caps a decompressed size;
  ``plog_base_max_suppressed`` (1024) caps the log messages held for
  duplicate suppression; ``pmdl_ompi_max_param_file_size`` (1MB) caps
  an Open MPI parameter file read for a job; ``iof_writer_backlog``
  (16384 chunks) caps output queued for a file another user's job
  writes; ``runas_timeout`` (10 seconds) bounds work done as another
  user; and ``pmix_tool_finalize_timeout`` (5 seconds) bounds a tool's
  wait at finalize.

* As root, PMIx reads only parameter files that root controls, and the
  MCA home-directory defaults are omitted when the user has no home
  directory.

* ``pstat`` takes an empty field list to mean every field, and answers
  a pid the requester does not own as one with no process.

* The installed ``pmix_getline()`` utility reads a line of any length,
  and takes a second argument that reports a read that stopped short.

* The provisional resource-usage structures of v5.0
  (``pmix_proc_stats_t`` and its relatives), with their data types,
  functions, and macros, have been removed in favor of the attribute
  arrays the Standard adopted; use ``PMIx_Process_monitor``. Three
  attributes that shared another attribute's string in v5.0 --
  ``PMIX_FABRIC_DEVICE_COORDINATES``,
  ``PMIX_JOB_CTRL_CHECKPOINT_TIMEOUT``, and
  ``PMIX_QUERY_SUPPORTED_QUALIFIERS`` -- now have strings of their own.

* ``PMIX_SESSION_PROVISION``, missing since v5.0, is restored, and
  ``PMIX_CAP_BASE`` is again 1, as it was in v5.0.

* The Autotools versions used to build official release tarballs now
  live in ``VERSION`` as ``*_dist_version`` fields rather than inside
  ``contrib/make_dist_tarball``, and the required toolchain is m4
  1.4.21, autoconf 2.73, automake 1.19, and libtool 2.6.2.

A full list of individual changes will not be provided here,
but will commence with the v7.0.1 release.
