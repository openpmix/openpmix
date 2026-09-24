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

               For this release, that work is threefold: completion of
               the Group family of APIs with the invite/join construction
               mode (see the
               :ref:`Group Construction <group-construction-label>`
               section of the documentation); a substantial reduction in
               the amount of data PMIx moves during startup, aided by a
               new shared-memory datastore (``gds/shmem3``); and a
               systematic review of every subtree under ``src`` that
               closed a long list of crashes, leaks, and use-after-free
               defects.

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
  entire store. The wire format is unchanged.

* **New shared-memory datastore.** ``gds/shmem3`` replaces
  ``gds/shmem2``, with lock-free reads and the ability to update the
  data of a running job.

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

* **Remote tools can reach a server on any of its networks.** Servers
  listen on every public interface and advertise the extra addresses as
  ``PMIX_MYSERVER_ALT_URIS``.

* **Connections no longer stall a process.** A silent or unresponsive
  peer can no longer hang a server, client, or tool, and intermittent
  connection failures under spawn-heavy workloads have been fixed.

* **New regex interface.** ``PMIx_generate_regex2`` and
  ``PMIx_parse_regex2`` work on a structured ``pmix_regex2_t``; the
  ``preg/native`` component has been removed. ``PMIX_CAP_REGEX2``.

* **No more hangs from event handlers.** Blocking APIs called from the
  progress thread now return ``PMIX_ERR_WOULD_BLOCK``.

* **Safer file handling.** Shared-memory, ``hwloc.sm``, and rendezvous
  files are created exclusively and verified before they are reused or
  re-permissioned.

* **pmix_info fixes.** ``--param`` and ``--show-version`` now honor the
  arguments their usage text documents.

* **MCA parameter files** are parsed by a new line reader, fixing
  several silent misreads (CRLF endings, quoted values, a missing final
  newline).

* **Command-line matching.** The new ``pmix_cli_match()`` resolves an
  input against all of an option's choices and reports ambiguous
  abbreviations. ``PMIX_CAP_CLI_MATCH``.

* **Build.** ``flex`` is no longer required, and the Python bindings now
  receive the build's optimization and hardening flags.

* **Documentation.** 277 new man pages cover the public API,
  ``docs/how-things-work`` describes the major subsystems, and the
  security policy now lives in ``SECURITY.md``.

* **Testing.** New unit tests, multi-node Docker Swarm suites,
  AddressSanitizer CI, and cross-version CI with clients back to v2.1.

Compatibility notes
^^^^^^^^^^^^^^^^^^^

.. rst-class:: open

* **Out-of-tree MCA components must be rebuilt** against the v7
  headers; framework versions are now checked at load time.
  ``PMIX_CAP_MCA_FW_VERSION``.

* **Version floors.** A server accepts clients and tools from v2.1 on;
  a client or tool connects only to servers from v3.2 on. Refusals
  report ``PMIX_ERR_OUTDATED``.

* **No silent singleton fallback.** ``PMIx_Init`` fails with
  ``PMIX_ERR_COMM_FAILURE`` when it cannot reach a server it was told
  about. ``PMIX_ERR_UNREACH`` now means no server was described.

* **Connection waits are bounded by default.**
  ``ptl_base_handshake_wait_time`` now defaults to 60 seconds and the
  new ``ptl_base_connect_ack_timeout`` to 5 seconds; set either to 0 for
  an unbounded wait.

* Group membership keeps the order the host provided, and group ranks
  follow it. ``PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`` is deprecated and
  ignored.

* A blocking ``PMIx_IOF_pull`` now returns the registration id (a
  non-negative value) instead of ``PMIX_OPERATION_SUCCEEDED``.

* ``PMIx_Compute_distances`` values differ from earlier releases because
  of the new PCIe term; relative order is otherwise unchanged.

* Directories PMIx creates below a given root are reused only when
  owned by the effective uid, and existing directories PMIx is given are
  used with their permissions as found.

* ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` now defaults to ``false``. Hosts
  that want tools from other users to connect must pass it; PRRTE does.

* A command-line option value must now be a leading part of the name it
  matches, so ``packagefoo`` no longer matches ``package``.

* Support for the Solaris, Sun Studio, and KAI toolchains has been
  removed.

* ``PMIX_COMPRESSED_STRING`` is deprecated.

A full list of individual changes will not be provided here,
but will commence with the v7.0.1 release.
