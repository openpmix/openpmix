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
               the Group family of APIs with the invite/join
               construction mode, a substantial reduction in the amount
               of data PMIx moves during startup, and a systematic
               review of every subtree under ``src`` that closed a long
               list of crashes, leaks, and use-after-free defects.

               Proper execution of the group construction modes, and of
               the session and allocation support described below,
               requires that the host environment provide the necessary
               backend support. Please check with your host provider, or
               use the PMIx Reference RunTime Environment
               (`PRRTE <https://github.com/openpmix/prrte/releases>`_).

Highlights since v6.1.0
^^^^^^^^^^^^^^^^^^^^^^^

* **Group construction by invitation now runs end to end.** Participants
  exchange endpoint data, ``PMIX_GROUP_ASSIGN_CONTEXT_ID`` is honored on
  the invite/join path, and a group formed by invitation has the same
  reach as a constructed one. An invitation that names a whole
  namespace with ``PMIX_RANK_WILDCARD`` is expanded into its members
  rather than crashing the server. Detectable as
  ``PMIX_CAP_GROUP_JOIN_COMPLETES``.

* **Startup moves far less data.** ``PMIx_Commit`` and the collecting
  fence now send only what a process has published since the last one,
  rather than resending its entire store each time — *n* put/commit
  cycles previously moved O(n\ :sup:`2`) bytes. Nothing changed on the
  wire, so clients and servers of different releases interoperate
  exactly as before.

* **A new shared-memory datastore, ``gds/shmem3``,** replaces
  ``gds/shmem2``. It has a lock-free read path, holds sessions and jobs
  in segments of their own, and can revise the data of a job that is
  already running.

* **Data can now be deleted.** New ``PMIX_DEL_*`` scopes on ``PMIx_Put``
  remove a key, and a server takes a removal back from the clients that
  cached it. ``PMIx_server_deregister_resources`` honors the qualifiers
  its man page describes and reaches jobs that are already running.
  ``PMIX_CAP_DATA_DELETE``.

* **Sessions are first class.** ``PMIx_server_register_session`` and
  ``PMIx_server_deregister_session`` let a host describe a session
  independently of the jobs within it, and update that description while
  those jobs run. ``PMIX_CAP_SESSION_REGISTRATION``.

* **Flow control for stdin.** ``PMIx_server_IOF_flow_control`` lets a
  host suspend and resume whoever is feeding it stdin; a host can also
  throttle opportunistically by answering a ``push_stdin`` upcall with
  ``PMIX_ERR_IOF_XOFF``. The request is relayed through chained
  launchers. ``PMIX_CAP_IOF_FLOW_CONTROL``.

* **Expanded allocation and scheduler support** — a family of
  ``PMIX_ALLOC_*`` attributes covering allocation properties,
  inheritance rules, advance warning of timeout, and events for
  resource release; ``palloc`` can name the allocation it is extending
  or shrinking, and a spawn can carry the allocation request it needs.

* **New compression components.** ``zstd`` and ``lz4`` join the
  ``pcompress`` framework, with zstd ranked above the zlib components
  wherever ``libzstd`` is found: on a 25.6 MB aggregated modex it
  compresses roughly 6x faster and decompresses roughly 6x faster than
  the previous zlib default, at a better ratio. Every component's
  compression level is now an MCA parameter. Note that zstd and lz4
  blobs are not DEFLATE, so all nodes in a job must use the same
  component.

* **GPU discovery repaired and extended.** The ``pgpu/amd`` and
  ``pgpu/nvd`` components now activate on the nodes that actually hold
  their vendor's cards, and a process mapped against Intel GPUs is given
  a correct ``ZE_AFFINITY_MASK``. ``PMIX_CAP_DEVICE_ENUM``.

* **Device distances can be measured from a device.**
  ``PMIX_DEVICE_DIST_ORIGIN`` names a device to measure from instead of
  a process location, so an unbound process can ask which NICs share a
  PCIe root complex with a given GPU. Distances now include the path
  through the PCIe tree, which breaks the ties the CPU tree left between
  devices under the same package. A device may be named by vendor uuid
  or PCI bus id as well as by OS name, every device a request names is
  reported rather than only the first, and a fabric device reporting no
  usable GUIDs is given a unique name rather than dropped.
  ``PMIX_CAP_DEVICE_DIST_ORIGIN``.

* **Remote tools can reach a server on any of its networks.** A server
  accepting remote tool connections now listens on every public
  interface rather than only the first, and publishes the extra
  addresses as ``PMIX_MYSERVER_ALT_URIS`` and on a line of its contact
  file that older readers do not see. A tool that cannot reach the
  server's URI tries those alternates; ``PMIX_SERVER_ALT_URIS`` supplies
  them alongside an explicit URI.

* **Connections can no longer stall a process.** A peer that connects
  to a server and then goes silent — a port probe, or a tool suspended
  mid-connect — no longer freezes the server's progress thread.
  ``PMIx_tool_attach_to_server`` now connects without blocking the
  progress thread, and a client or tool connecting to a server that
  never answers gives up after a bounded wait instead of hanging. A
  server no longer intermittently drops new client connections after
  the host deregisters a finalized client — a failure that surfaced
  under spawn-heavy workloads as a client that came up as a singleton
  and could not reach its peers. A host's
  ``PMIx_server_register_client`` for a self-started tool or singleton
  that has already connected is now accepted, rather than refused with
  ``PMIX_ERR_DUPLICATE_KEY``.

* **A new regular expression interface,** ``PMIx_generate_regex2`` /
  ``PMIx_parse_regex2``, working on a structured ``pmix_regex2_t``
  instead of an encoded string. The older interface is retained as a
  serialization format only, and the ``preg/native`` component has been
  removed. ``PMIX_CAP_REGEX2``.

* **Calling PMIx from an event handler no longer hangs the process.**
  Every blocking entry point now returns ``PMIX_ERR_WOULD_BLOCK`` when
  called from within the PMIx progress thread, and reports which call it
  was; the affected man pages state the rule.

* **The files PMIx creates are handled more carefully.** The
  shared-memory segment backing files, ``hwloc.sm``, and the rendezvous
  files all sit at predictable names in a directory the host provides,
  which a host running its server with privilege commonly hands to the
  job's user. They are now made through a single exclusive-create
  helper, relative to a descriptor on their directory so that the
  directory name is resolved exactly once for the create, the
  stale-file check, the reclaim and the eventual removal. A segment is
  re-owned and re-permissioned only when the file that opens is still
  the regular, singly linked file the handle created, identified by its
  device and inode, rather than whatever the name leads to at that
  moment. Created files are close-on-exec, a rendezvous file whose
  write fails is removed at once instead of being left for a later
  server to reclaim, and a name occupied by something that is not a
  regular file is declined rather than read.

* **``pmix_info`` now shows what you asked for.** ``--param`` reads the
  ``<framework>[:<component>[,...]]`` value its usage text has always
  documented — or ``all`` — instead of printing every parameter in PMIx
  whatever it was given; any number of ``--param`` and ``--params``
  options are honored, and a malformed value is reported rather than
  ignored. ``--show-version`` honors its ``<part>`` argument for
  ``pmix`` and ``all`` as it already did for a framework or component.
  The help text and man page describe the accepted syntax and list
  every ``--path`` keyword the tool accepts.

* **MCA parameter files are read more carefully.** The flex scanner that
  parsed them has been replaced by a line reader, closing four cases
  where a file was silently misread: a malformed parameter name set some
  *other* parameter instead of being reported, CRLF line endings left a
  carriage return on the end of every value, a quoted ``-mca`` value that
  ended a line was truncated at the first space inside the quotes, and a
  final line with no closing newline lost its last character. A UTF-8
  byte-order mark is now skipped rather than reported as an error.
  Everything the old scanner accepted still parses the same way.

* **Build.** ``flex`` is no longer a prerequisite, and the generated
  scanner it produced is gone from the release tarball. The Python
  bindings extension is now compiled with the optimization and hardening
  flags the build environment supplied — it had been receiving none of
  them — while still avoiding the picky warning flags that Cython's
  generated source cannot survive. A build from a git checkout also
  checks its objects for common symbols at install time, a class of
  tentative definition that merges silently instead of being reported as
  a duplicate and that causes link problems on macOS. The hand-rolled
  ``openpty`` replacement in ``pmix_pty``, which no supported platform
  ever compiled, has been removed: ``pmix_openpty()`` and ``pmix_forkpty()``
  are now thin wrappers over ``openpty(3)`` and ``forkpty(3)`` and
  report no pty where the platform lacks them, which their callers
  already handle by using a pipe.

* **Documentation.** The public API is now covered by man pages — 277 new
  pages — and ``docs/how-things-work`` gained descriptions of the modex,
  the shared-memory datastore, the transport layer, group construction,
  init/finalize, logging, and inheritance. ``PMIx_server_init(3)`` now
  says which rendezvous files a server writes, where, and who can read
  them, and lists the launcher-rendezvous and TCP listener directives
  it had omitted; the session-directory page gives the correct names of
  those files. The security policy now
  lives in ``SECURITY.md``, where GitHub offers a private reporting
  channel, and describes the process the project actually follows.

* **Testing.** The review work landed with unit coverage for the defects
  it found, multi-node suites that run the same cases across real
  servers under Docker Swarm, a CI job that builds and tests under
  AddressSanitizer, and cross-version CI that runs clients as old as
  v2.1 against the current server.

Compatibility notes
^^^^^^^^^^^^^^^^^^^

* MCA framework interface versions are now checked when a component is
  loaded, so a plugin built against an older framework header is
  declined rather than called through. **Out-of-tree components must be
  rebuilt against the v7 headers.** A declined component is reported
  through ``mca_base_component_show_load_errors``.
  ``PMIX_CAP_MCA_FW_VERSION``.

* **Supported version floors.** A server accepts clients and tools
  from v2.1 on; a client or tool connects only to servers from v3.2 on.
  Support for v1.x peers, and for v2.0 clients, has been removed — those
  combinations could not work in practice. A refusal is reported with
  the new status ``PMIX_ERR_OUTDATED``, and ``PMIx_Init`` now fails with
  it rather than quietly running as a singleton.

* ``PMIx_Init`` no longer falls back to singleton operation when it
  cannot reach a server it was told about. ``PMIX_ERR_UNREACH`` now
  means only that no server was described to the process; one that was
  given a server — by its launcher's environment or a
  ``PMIX_SERVER_URI`` directive — and cannot connect fails with
  ``PMIX_ERR_COMM_FAILURE``, and any other connection failure likewise
  fails the init.

* Connection waits are now bounded by default.
  ``ptl_base_handshake_wait_time``, which bounds how long a client or
  tool waits on a server while connecting, now defaults to 60 seconds
  (previously 0, unbounded), and also bounds the ``connect()`` itself.
  The new ``ptl_base_connect_ack_timeout`` (default 5 seconds) bounds
  how long a server waits for a connecting peer's handshake. Setting
  either to 0 restores the unbounded wait.

* A group's membership is kept in the order the host provided, and
  group ranks count across that order, so they now agree with
  ``PMIX_GROUP_MEMBERSHIP``. ``PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`` is
  deprecated and ignored.

* A blocking ``PMIx_IOF_pull`` (``NULL`` ``regcbfunc``) now returns the
  registration id — a non-negative value — as ``pmix_tool.h`` has
  always documented, instead of ``PMIX_OPERATION_SUCCEEDED``. Code that
  must also work with earlier releases should treat either as success.

* Device distances reported by ``PMIx_Compute_distances`` now include a
  PCIe-path term, so their absolute values differ from earlier
  releases. Their relative order among devices that the CPU tree
  already distinguished is unchanged.

* A directory PMIx composes below a root it was handed — the
  ``<nspace>/rank.N`` levels of ``PMIX_IOF_OUTPUT_TO_DIRECTORY``, the
  levels an output file pattern expands to — is reused only when it is
  already owned by the effective uid. One left over from another user's
  job is refused, with a message naming it. The root itself, and any
  directory given to PMIx by its caller, are trusted as handed over;
  judging those remains the host's part.

* An existing directory PMIx is given — ``$TMPDIR``, the system tmpdir,
  ``PMIX_SERVER_TMPDIR``, an IOF output directory — is now used with its
  permissions as found, rather than having the requested mode bits added
  to it. Directories PMIx creates itself still get the requested mode.

* ``PMIX_SERVER_ALLOW_FOREIGN_TOOLS`` now defaults to ``false``. A host
  that wants tools running under other user IDs to connect, and its
  rendezvous files readable by all users, must now pass the attribute;
  previously that was the behavior unless the host said otherwise. PRRTE
  always passes it and is unaffected. Because the rendezvous directory's
  permissions are no longer widened, one that other users cannot search
  still keeps foreign tools out; the listener reports that case at
  ``ptl_base_verbose`` level 2.

* Support for the Solaris, Sun Studio, and KAI toolchains has been
  removed from ``configure``.

* ``PMIX_COMPRESSED_STRING`` is deprecated.

A full list of individual changes will not be provided here,
but will commence with the v7.0.1 release.
