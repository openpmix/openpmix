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
  reach as a constructed one. Detectable as
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

* **A new regular expression interface,** ``PMIx_generate_regex2`` /
  ``PMIx_parse_regex2``, working on a structured ``pmix_regex2_t``
  instead of an encoded string. The older interface is retained as a
  serialization format only, and the ``preg/native`` component has been
  removed. ``PMIX_CAP_REGEX2``.

* **Calling PMIx from an event handler no longer hangs the process.**
  Every blocking entry point now returns ``PMIX_ERR_WOULD_BLOCK`` when
  called from within the PMIx progress thread, and reports which call it
  was; the affected man pages state the rule.

* **Documentation.** The public API is now covered by man pages — 277 new
  pages — and ``docs/how-things-work`` gained descriptions of the modex,
  the shared-memory datastore, the transport layer, group construction,
  init/finalize, logging, and inheritance.

* **Testing.** The review work landed with unit coverage for the defects
  it found, multi-node suites that run the same cases across real
  servers under Docker Swarm, and a CI job that builds and tests under
  AddressSanitizer.

Compatibility notes
^^^^^^^^^^^^^^^^^^^

* MCA framework interface versions are now checked when a component is
  loaded, so a plugin built against an older framework header is
  declined rather than called through. **Out-of-tree components must be
  rebuilt against the v7 headers.** A declined component is reported
  through ``mca_base_component_show_load_errors``.
  ``PMIX_CAP_MCA_FW_VERSION``.

* Support for the Solaris, Sun Studio, and KAI toolchains has been
  removed from ``configure``.

* ``PMIX_COMPRESSED_STRING`` is deprecated.

A full list of individual changes will not be provided here,
but will commence with the v7.0.1 release.
