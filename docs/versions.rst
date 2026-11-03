.. _label-version-numbers:

Version Numbers and Binary Compatibility
========================================

OpenPMIx has two sets of version numbers that are likely of interest
to end users / system administrator:

* Software version number
* Shared library version numbers

Both are described below, followed by a discussion of application
binary interface (ABI) compatibility implications.

Software Version Number
-----------------------

OpenPMIx's version numbers are the union of several different values:
major, minor, release, and an optional quantifier.

* Major: The major number is the first integer in the version string
  (e.g., v1.2.3). Changes in the major number typically indicate a
  significant change in the code base and/or end-user
  functionality. The major number is always included in the version
  number.

* Minor: The minor number is the second integer in the version
  string (e.g., v1.2.3). Changes in the minor number typically
  indicate an incremental change in the code base and/or end-user
  functionality. The minor number is always included in the version
  number:

* Release: The release number is the third integer in the version
  string (e.g., v1.2.3). Changes in the release number typically
  indicate a bug fix in the code base and/or end-user
  functionality.

* Quantifier: OpenPMIx version numbers sometimes have an arbitrary
  string affixed to the end of the version number. Common strings
  include:

  * ``aX``: Indicates an alpha release. X is an integer indicating
    the number of the alpha release (e.g., v1.2.3a5 indicates the
    5th alpha release of version 1.2.3).
  * ``bX``: Indicates a beta release. X is an integer indicating
    the number of the beta release (e.g., v1.2.3b3 indicates the 3rd
    beta release of version 1.2.3).
  * ``rcX``: Indicates a release candidate. X is an integer
    indicating the number of the release candidate (e.g., v1.2.3rc4
    indicates the 4th release candidate of version 1.2.3).

Although the major, minor, and release values (and optional
quantifiers) are reported in OpenPMIx nightly snapshot tarballs, the
filenames of these snapshot tarballs follow a slightly different
convention.

Specifically, the snapshot tarball filename contains three distinct
values:

* Most recent Git tag name on the branch from which the tarball was
  created.

* An integer indicating how many Git commits have occurred since
  that Git tag.

* The Git hash of the tip of the branch.

For example, a snapshot tarball filename of
``pmix-v1.0.2-57-gb9f1fd9.tar.bz2`` indicates that this tarball was
created from the v1.0 branch, 57 Git commits after the ``v1.0.2`` tag,
specifically at Git hash gb9f1fd9.

OpenPMIx's Git master branch contains a single ``dev`` tag.  For example,
``pmix-dev-8-gf21c349.tar.bz2`` represents a snapshot tarball created
from the master branch, 8 Git commits after the "dev" tag,
specifically at Git hash gf21c349.

The exact value of the "number of Git commits past a tag" integer is
fairly meaningless; its sole purpose is to provide an easy,
human-recognizable ordering for snapshot tarballs.

Shared Library Version Number
-----------------------------

OpenPMIx uses the GNU Libtool shared library versioning scheme.

.. note:: Only official releases of OpenPMIx adhere to this versioning
          scheme. "Beta" releases, release candidates, and nightly
          tarballs, developer snapshots, and Git snapshot tarballs
          likely will all have arbitrary/meaningless shared library
          version numbers.

The GNU Libtool official documentation details how the versioning
scheme works.  The quick version is that the shared library versions
are a triple of integers: (current,revision,age), or ``c:r:a``.  This
triple is not related to the PMIx software version number.  There
are six simple rules for updating the values (taken almost verbatim
from the Libtool docs):

#. Start with version information of ``0:0:0`` for each shared library.

#. Update the version information only immediately before a public
   release of your software. More frequent updates are unnecessary,
   and only guarantee that the current interface number gets larger
   faster.

#. If the library source code has changed at all since the last
   update, then increment revision (``c:r:a`` becomes ``c:r+1:a``).

#. If any interfaces have been added, removed, or changed since the
   last update, increment current, and set revision to 0.

#. If any interfaces have been added since the last public release,
   then increment age.

#. If any interfaces have been removed since the last public release,
   then set age to 0.

Application Binary Interface (ABI) Compatibility
------------------------------------------------

PMIx keeps its public interface forever. Public APIs, macros,
structures, and attributes are not removed, and a structure's layout
is not changed. Every release from v4.2 through v7.0 has the same
shared-library name, ``libpmix.so.2``.

**A program built against an earlier PMIx release therefore runs
against a later one without being recompiled** - on Linux, for
example, by pointing ``LD_LIBRARY_PATH`` at the newer library, or by
installing the newer library in place of the older. This has been
tested: the example programs of PMIx v5.0.11, built against v5.0.11,
run against the v7.0 library and behave exactly as the same programs
rebuilt against v7.0.

Two kinds of program are exceptions:

* **Host environments** - programs that call ``PMIx_server_init`` - must
  be built against the PMIx release they run with. The library
  receives only a pointer to the host's ``pmix_server_module_t``, and
  cannot tell which release's definition of that structure the host
  was built with. See :doc:`/hosts/partnership`.
* **Programs that use PMIx's internal headers** - those installed under
  ``include/pmix/src`` - get no compatibility guarantee at all. PRRTE is
  the main such program, which is why each PRRTE series works only
  with particular PMIx series (see :doc:`/admin/prrte`).

The promise was broken in two places between v5.0 and v7.0.

**Provisional resource-usage structures, replaced by the final
definitions.** v5.0 defined four structures for resource-usage
statistics, with functions to manage them. They were *provisional*,
as the PMIx Standard defines the term: proposed, but not yet decided
by the Standard's committee, and open to change. The committee has
since decided to represent these quantities as arrays of attributes.
v7.0 implements that final definition and drops the provisional one.
Removed:

* the types ``pmix_proc_stats_t``, ``pmix_node_stats_t``,
  ``pmix_disk_stats_t``, and ``pmix_net_stats_t``, and their data type
  codes ``PMIX_PROC_STATS``, ``PMIX_NODE_STATS``, ``PMIX_DISK_STATS``,
  and ``PMIX_NET_STATS``;
* the functions ``PMIx_Proc_stats_construct``, ``_destruct``,
  ``_create``, and ``_free``, and the same four for ``Node``, ``Disk``,
  and ``Net``;
* the ``PMIX_PROC_STATS_*``, ``PMIX_NODE_STATS_*``,
  ``PMIX_DISK_STATS_*``, and ``PMIX_NET_STATS_*`` macros.

No v5.0 API returned these structures; a program could only build them
itself. A program that uses them does not build against v7.0, and one
built against v5.0 cannot run against it. To get resource usage in
v7.0, call ``PMIx_Process_monitor`` with
``PMIX_MONITOR_PROC_RESOURCE_USAGE`` (or the ``NODE``, ``DISK``, or
``NET`` form). The results are arrays of attributes such as
``PMIX_PROC_RSS`` and ``PMIX_NODE_LOAD_AVG``, one for each field of the
old structures.

**Three attribute strings, changed to fix a bug.** In v5.0, three
attributes had the same string as another attribute, so the library
could not tell the two apart. v7.0 gives each its own string:

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Attribute
     - v5.0 string, shared with
     - v7.0 string
   * - ``PMIX_FABRIC_DEVICE_COORDINATES``
     - ``pmix.fab.coord``, ``PMIX_FABRIC_COORDINATES``
     - ``pmix.fabdev.coord``
   * - ``PMIX_JOB_CTRL_CHECKPOINT_TIMEOUT``
     - ``pmix.jctrl.ckptsig``, ``PMIX_JOB_CTRL_CHECKPOINT_SIGNAL``
     - ``pmix.jctrl.ckpttime``
   * - ``PMIX_QUERY_SUPPORTED_QUALIFIERS``
     - ``pmix.qry.quals``, ``PMIX_QUERY_QUALIFIERS``
     - ``pmix.qry.supqual``

A program built against v5.0 that uses one of these three still sends
the v5.0 string, which v7.0 reads as the other attribute of the pair.
Rebuild it against v7.0 to use the new string. A program that uses the
other attribute of each pair is unaffected.

Cross-Version Compatibility
---------------------------

As PMIx adoption has grown, the problem of managing application-SMS interactions between different PMIx library versions has increased in visibility. It soon became clear that use of a common library version by both SMS and application could not be guaranteed, especially in the container-based application use-case. Thus, cross-version compatibility arose as a problem.

PMIx has addressed this by utilizing a plugin-based architecture that allows both the client and server to select from a range of supported protocol levels. The resulting coordination is based on a client-driven handshake – i.e., the client selects the protocol to be used, and the server adapts to support it. The client's selection is based on a combination of environmental parameters passed to it at launch by the server, filtered against the protocols available to that particular client.

That support has limits in both directions:

* A server supports clients and tools using PMIx v2.1 or later. Those of releases before v7.0 connect over TCP, and a server takes them only when its ``ptl_base_legacy_peers`` MCA parameter, or its host's ``PMIX_SERVER_LEGACY_PEERS`` attribute, says so. Both are off by default, and only a PMIx configured ``--enable-legacy-peers`` can take them at all - by default it cannot, as clients and tools are to be updated along with their servers. On a platform whose kernel keeps no table of TCP connections - any but Linux, macOS and FreeBSD 12 or later - one that authenticates with ``native`` is taken on the identity it claims, as every earlier release took it.
* A client or tool supports servers using PMIx v3.2 or later.

Connections outside those limits are refused, with a message naming the version that was found, rather than being allowed to fail later in some less obvious way. In particular:

* PMIx v1.x clients cannot connect to a current server at all: they support only the original ``usock`` transport, whose protocol no current server speaks. (The local-socket transport of v7, also named ``usock``, is a different protocol.)
* PMIx v2.0.x clients are not supported. A server does not advertise itself in the form they look for, and one that reaches a server anyway is refused at its handshake, which does not identify the buffer format it uses, so a server cannot reliably read its messages.
* A current client or tool will not connect to a server older than v3.2. Such a server accepts the connection, but cannot answer requests that current clients make during initialization.
* PMIx v4.1 is no longer maintained. Its clients and tools cannot use a server built with ``--enable-debug``: such a server has its clients use fully described buffers, and a v4.1 library built without that option builds its first request in its own default format and fails ``PMIx_Init`` with ``PMIX_ERR_PACK_MISMATCH``. They work with a server built without ``--enable-debug``.

.. note:: The cross-version guarantee only means that users of two different versions will be able to communicate requests and their responses. It does not guarantee that both sides will completely support the requests. It is possible, for example, for a server to not include support for an operation that was introduced in a newer version being used by a client. Likewise, it is possible that a bug existed in an earlier version that precludes correct completion of the request.
