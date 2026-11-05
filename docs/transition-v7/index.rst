.. _label-transition-v7:

Moving to PMIx v7
=================

.. DRAFT: review draft. Notes marked DRAFT are open items for
   reviewers; everything else is meant as final text.

PMIx v7 is the result of a review of the whole library: every source
directory, every connection path, and every place one process's data
is handed to another. The review set out to make PMIx's behavior
precise, so that a site, a host environment, or an application
developer can rely on it without reading the code.

Most programs see no difference. Some will, and this section says
which, why, and what to do about it. Start with this page, then read
the page for your role:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - If you...
     - Read
   * - run jobs
     - :doc:`users`
   * - install PMIx, or administer a system that runs it
     - :doc:`admins`
   * - write a tool - a debugger, a launcher front end such as
       ``prun``, a monitor - that calls ``PMIx_tool_init``
     - :doc:`tools`
   * - write a resource manager, launcher, or other host environment
       that embeds the PMIx server library
     - :doc:`hosts`
   * - write a system scheduler (workload manager) that runtime
       environments attach to
     - :doc:`schedulers`, after :doc:`hosts`
   * - write an application, library, or programming model (an MPI
       implementation, say) that calls ``PMIx_Init``
     - :doc:`libraries`

Each page stands on its own. A change that affects more than one role
is described on each page it affects.


Why v7 changes things
---------------------

The review had four goals.

**Every connection is authenticated by the operating system.** A
process connecting to a PMIx server is identified by what the kernel
reports for the connection - its user, its groups, and its process ID
- not by what the process says about itself. A client must match the
user and group its host registered it with.

**Each job's data is visible only to those it belongs to.** By
default, a job's data, output, and control belong to the user who owns
the job. A host can widen that to other users and groups. Data held in
shared memory and written to output files follows the same rule.

**PMIx and its host each know their part.** The division of work
between the PMIx server library and the host environment that embeds
it - who checks what, who decides what - is now written down
(:ref:`label-host-env`). The library tells the host who is asking on
every request it passes up, and leaves the decision to the host.

**Behavior is documented and tested.** See `Documentation`_ and
`Testing`_ below.


What has not changed
--------------------

* **The API.** A program built against PMIx v5.0 runs against the v7
  library without being rebuilt. There are two exceptions
  (:ref:`label-version-numbers`):

  * the resource-usage structures (``pmix_proc_stats_t`` and its
    relatives) and their functions are gone. In v5.0 they were
    provisional, in the PMIx Standard's sense. The Standard's
    committee then chose arrays of attributes for these quantities,
    and v7 implements that final definition. No v5.0 API returned the
    structures.
  * three attributes that shared a string with another attribute now
    have their own - a bug fix.

* **Cross-version operation is preserved.** Clients, tools, and
  servers of different releases still work together:

  * a v7 client or tool connects to servers from PMIx v3.2 on;
  * a v7 server takes clients and tools from PMIx v2.1 on, when it is
    built and set up to take them. That is a choice each site makes
    (see below).

  This matters most for containers, which carry their own PMIx
  library.

* **The PMIx Standard.** v7 implements the Standard as before.


Changes that affect everyone
----------------------------

These appear on every role's page too, with what each means for that
role.

**Connections are node-local only.** A client or tool connects only to
a PMIx server on its own node. PMIx no longer carries messages between
nodes: a tool on another node - a login node, say - cannot connect to
a server directly. Today, run the tool on the node where the server
runs: for a launcher or debugger, ssh to the node where the DVM
controller (``prte`` or ``mpirun``) is running. Launching from a login
node will remain possible; how is being worked out. Communication
across nodes is the host environment's job, as it always was for
clients.

**Connections use a local socket (AF_UNIX), not TCP.** v7 clients and
tools reach a v7 server only through a local socket in the server's
temporary directory. The kernel reports the peer's identity for every
such connection.

**Earlier-release clients and tools need a server set up to take
them.** Releases before v7 connect over TCP. A v7 server takes them
only when PMIx was configured with ``--enable-legacy-peers`` *and* the
site or host turns the support on (``ptl_base_legacy_peers``, or the
host's ``PMIX_SERVER_LEGACY_PEERS``). Even then it listens on loopback
only. Without both, those clients and tools are refused. An older
client then runs as a singleton: alone, and unaware of its job.

**By default, only the job's owner sees the job.** Data, output,
monitoring, and job control are limited to the job's user unless the
host widens access (``PMIX_ACCESS_PERMISSIONS``).

**Tools of another user are refused by default.** A host must allow
them explicitly, and widen the server's socket to them.

**One connection per process.** Each rank of a job connects once, with
a token its host issued for that process. A copy of a process
connecting as the same rank is refused, and so is a program a process
``exec``\ s: the new program is a new process to the server.

**Identity variables leave the environment at init.** A client or
tool removes the variables that name its server and its identity from
its environment when it initializes, so a process it starts afterwards
does not inherit them.

**Fewer authentication methods.** ``native`` (the kernel) and
``munge`` remain. Components that admitted a peer without checking it
are gone.

**Temporary directory paths must be short.** The local socket lives
under the server's temporary directory, and a socket path is limited
to about 100 characters. The default ``$TMPDIR`` on macOS is too long.

**Settings PMIx cannot honor now fail.** A parameter naming a removed
component, an unknown user, or an invalid path stops the process at
init with a message, rather than being ignored.

**Upgrade the host environment with PMIx.** PRRTE v5 requires PMIx
v7; PRRTE v3 refuses it. Skip PMIx v6.x, which was announced "do not
use".


Documentation
-------------

* **A manual page for every public API.** v5.0 had pages for three
  functions (``PMIx_Init``, ``PMIx_Finalize``, ``PMIx_Abort``). v7 has
  one for every client, tool, and server function - about 280 in
  section 3. Each gives the function's arguments, the attributes it
  accepts, its return codes, and whether it blocks. About 55 pages in
  section 5 cover the data types, the callbacks, and the server module
  (:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`, with an
  entry for each up-call). Section 1 covers the commands.
* **How things work.** v5.0 had two pages; v7 has more than thirty
  (:doc:`/how-things-work/index`). They cover connections and
  authentication, the modex, collectives, groups and process sets,
  shared-memory job data, session directories, I/O forwarding and
  inheritance, monitoring, distances, the command-line parser, and
  more.
* **Guides by role**, new in v7:

  * :ref:`label-where-to-start`, for finding the right page;
  * :ref:`label-sysadmin`: choosing a release, pairing with PRRTE, and
    controlling authentication;
  * :ref:`label-host-env`: the contract between the server library
    and its host, operation by operation.


Testing
-------

* **Unit tests in** ``make check``. About 150 programs under
  ``test/unit`` - none existed in v5.0 - cover each subsystem:
  classes, the MCA, buffer operations, datastores, transports and
  connection handling, servers, tools, events, I/O forwarding, groups,
  and the components. Most run a real server with real clients.
* **Continuous integration** on every pull request:

  * Linux and macOS builds;
  * a build with every component as a plugin;
  * a build with MUNGE;
  * an AddressSanitizer build;
  * ``make distcheck``;
  * the Python bindings, across Python versions;
  * the PRRTE, debugger, group, and mpi4py test suites.

* **Cross-version tests** in CI: clients and tools of v3.2, v4.1,
  v4.2, and v5.0 against a v7 server, and v7 clients and tools against
  servers of those releases.
* **A multi-node harness** (``contrib/dockerswarm``): ten containers,
  each a node running its own PMIx server under PRRTE. It exercises
  what a single host cannot: data fetched from other nodes, groups
  that span nodes, a participant lost mid-operation, topology and
  locality, and every data type moved between nodes.
* **Multi-user tests** (``test/multiuser``): a DVM run by one user and
  jobs submitted by another, checking who owns the output files.
* **Leak and sanitizer checks**: the examples and the server, run
  under valgrind, on one node and across nodes.


.. toctree::
   :maxdepth: 1

   users
   admins
   tools
   hosts
   schedulers
   libraries
