Applications and Libraries on PMIx v7
=====================================

For developers of applications, libraries, and programming models -
MPI implementations, for example - that call ``PMIx_Init``.


Binary and source compatibility
-------------------------------

**A program built against PMIx v5.0 runs against v7 without being
rebuilt**, and builds against v7 unchanged. See
:ref:`label-version-numbers`.

There are two exceptions.

**The provisional resource-usage structures are gone.** In v5.0,
``pmix_proc_stats_t``, ``pmix_node_stats_t``, ``pmix_disk_stats_t``,
``pmix_net_stats_t``, their ``PMIx_*_stats_*`` functions, and their
macros were provisional, in the PMIx Standard's sense: open to change
until the Standard's committee decided. The committee chose arrays of
attributes instead, and v7 replaces the provisional definitions with
that final one.
No v5.0 API returned them, so only a program that built them itself is
affected. For resource usage, call ``PMIx_Process_monitor`` with
``PMIX_MONITOR_PROC_RESOURCE_USAGE`` (or the ``NODE``, ``DISK``, or
``NET`` form); the results are arrays of attributes, such as
``PMIX_PROC_RSS``.

**Three attribute strings changed, to fix a bug.** In v5.0, three
attributes had the same string as another attribute, so the two could
not be told apart. Each now has its own string:

* ``PMIX_FABRIC_DEVICE_COORDINATES`` (shared with
  ``PMIX_FABRIC_COORDINATES``);
* ``PMIX_JOB_CTRL_CHECKPOINT_TIMEOUT`` (shared with
  ``PMIX_JOB_CTRL_CHECKPOINT_SIGNAL``);
* ``PMIX_QUERY_SUPPORTED_QUALIFIERS`` (shared with
  ``PMIX_QUERY_QUALIFIERS``).

A program built against v5.0 that uses one of the three still sends the
old string, which v7 reads as the other attribute of the pair. Rebuild
it to use the new one.

* **Newly deprecated**, still present: ``PMIX_CRYPTO_KEY``,
  ``PMIX_COMPRESSED_STRING``, ``PMIX_REGEX``,
  ``PMIX_QUERY_PARTIAL_SUCCESS``, ``PMIX_GROUP_CONTEXT_ID_ASSIGNED``,
  ``PMIx_generate_regex``, and ``PMIx_generate_ppn``.
* **Capability flags** (``PMIX_CAP_*`` in ``pmix_version.h``) say at
  build time which features the library has. Test for a flag rather
  than a version number.


Initialization
--------------

**Failures are more precise.**

* ``PMIX_ERR_UNREACH`` - run as a singleton - now means only that no
  server was described to the process.
* A server that was described but cannot be reached gives
  ``PMIX_ERR_COMM_FAILURE``. v5.0 became a singleton silently.
* New codes: ``PMIX_ERR_EXISTS`` (the rank is already connected),
  ``PMIX_ERR_OUTDATED`` (the server is older than v3.2),
  ``PMIX_ERR_LOCAL_URI_REQUIRED`` (the process was given a TCP address
  for a v7 server), and ``PMIX_ERR_INVALID_CRED`` (the namespace, rank,
  or connection token does not match what the host registered).

**The identity variables leave the environment at init.**
``PMIX_NAMESPACE``, ``PMIX_RANK``, the server's address, the connection
token, and the others that describe the process to PMIx are removed
from the environment by ``PMIx_Init``.

* If you need them, read them before ``PMIx_Init``, or ask PMIx with
  ``PMIx_Get`` afterwards.
* Copies a language runtime made earlier - Python's ``os.environ``,
  say - are not changed, except by PMIx's own bindings.

**One connection per rank.**

* A process started after ``PMIx_Init`` does not inherit the identity,
  so if it calls ``PMIx_Init`` it is a singleton.
* A process started before ``PMIx_Init`` that also calls it, as the
  same rank, is refused.
* A process that ``exec``\ s loses its connection. The new program is a
  new process to the server.

A library that forks helper processes and expects them to share the
parent's PMIx identity needs to change: have the parent do the PMIx
work, or have the host start the helpers.


Running under a server of a different release
---------------------------------------------

* **A v7 library connects to servers from v3.2 on.** A newer container
  on an older system works.
* **A library older than the server** - an application in a container
  built with PMIx v5, say - needs a server that takes legacy peers.
  Otherwise every process starts as a singleton: ``PMIx_Init`` reports
  one error, and the process runs alone. Sites choose whether to take
  them; see :doc:`admins`.
* **Open MPI singletons that spawn.** A singleton that calls
  ``MPI_Comm_spawn`` starts a ``prte`` and reads its address from a
  pipe. A v7 ``prte`` that takes legacy peers writes two addresses
  there, TCP first, and Open MPI built against PMIx v7 must take the
  second. That needs an Open MPI release with the change.

.. DRAFT: name the Open MPI release once open-mpi/ompi#14754 merges.


Data access
-----------

* **Another job's data, output, or control** follows that job's access
  list. A process of a user the list does not admit gets
  ``PMIX_ERR_NO_PERMISSIONS``.
* ``PMIx_Put`` **refuses reserved** ``pmix`` **keys**
  (``PMIX_ERR_BAD_PARAM``), and a commit carrying one is refused
  whole.
* **Shared-memory job data** (``gds/shmem3``, which replaces
  ``shmem2``) is read-only. A process the file permissions do not admit
  fetches from the server instead. A client of an earlier release falls
  back to the hash store.
* ``PMIx_Get`` **on the caller's thread** with ``shmem3`` is now the
  default.
* ``PMIx_Get`` **qualified by** ``PMIX_SESSION_ID`` answers about the
  session named.


Callbacks and threads
---------------------

A blocking PMIx call made from a PMIx callback or event handler
returns ``PMIX_ERR_WOULD_BLOCK`` instead of hanging. Use the
non-blocking form there, or hand the work to another thread.


Groups and events
-----------------

* **A group ID** must be unique while the group exists, non-empty, and
  may not be a job's namespace (``PMIX_ERR_EXISTS``).
* **A process that is not a participant** in a collective or group
  operation gets ``PMIX_ERR_NOT_A_MEMBER``. The server checks this too,
  so clients of earlier releases see it.
* **Events a process raises** reach the host with its verified
  identity. An event at ``PMIX_RANGE_NAMESPACE`` with no targets goes
  only to the source's own job; more than one
  ``PMIX_EVENT_CUSTOM_RANGE`` is refused.
* **A rank that dies or is deregistered** leaves its job's whole-job
  collectives, so the survivors' next ``PMIx_Fence`` completes. A rank
  the host restarts is counted back in.


Spawning
--------

* **When the host provides no spawn**, PMIx starts the processes
  itself only for a requester of the server's own user.
* **GPU visibility is set for you.** New in v7: when a process is
  mapped to GPUs, PMIx sets ``CUDA_VISIBLE_DEVICES``,
  ``ROCR_VISIBLE_DEVICES``, or ``ZE_AFFINITY_MASK`` in its environment
  at launch, naming those devices. A value already set is replaced. A
  process not mapped to a GPU is left alone.
