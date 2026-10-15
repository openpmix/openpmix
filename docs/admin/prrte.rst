.. _label-sysadmin-prrte:

PMIx and PRRTE Releases
=======================

PRRTE, the PMIx Reference RunTime Environment, is the reference host
environment for PMIx. It provides the ``prterun`` launcher and a
persistent distributed virtual machine (DVM), and is the runtime that
Open MPI's ``mpirun`` uses.

PRRTE and PMIx are released in pairs. **Install the PRRTE release that
pairs with the PMIx release you install.**


Why the pairing is strict
-------------------------

Like any host environment, PRRTE must be built against the PMIx
release it runs with (see :doc:`releases`). PRRTE goes further: besides
the public PMIx API, it is built on PMIx's *internal* code - its class
system, its component framework, and many of its utilities. Those are
not covered by PMIx's forward-compatibility promise, and they change
between release series.

So each PRRTE release series states the range of PMIx releases it
works with, and its ``configure`` refuses a PMIx outside that range.


Which releases pair
-------------------

.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * - PRRTE series
     - Requires PMIx
     - Status
   * - v5.0
     - v7.0 or later
     - Current. Pair with PMIx v7.0.
   * - v4.1
     - v6.1 or later
     - End of life (see below).
   * - v4.0
     - v6.0 or later
     - End of life (see below).
   * - v3.0
     - v4.2.4 or later, and earlier than v6.0
     - End of life. Was paired with PMIx v5.0.

The newest release of a PRRTE series is the one to use with its PMIx
series. PRRTE's own documentation lists the exact requirement for each
release.


The break between PMIx v5 and PMIx v7
-------------------------------------

There is no PRRTE release that works with both PMIx v5 and PMIx v7:

* PRRTE v3.0 refuses PMIx v6.0 and later.
* PRRTE v5.0 refuses PMIx earlier than v7.0.

A system moving from PMIx v5 to PMIx v7 must therefore move from PRRTE
v3 to PRRTE v5 at the same time. Upgrade the two together, and rebuild
anything else that hosts a PMIx server.

The break came from PMIx's help-message system, which PRRTE shares.
Up to PMIx v5, help text was installed as files and read at run time.
From PMIx v6, it is compiled into the library and held in memory. A
PRRTE built for one arrangement cannot work with the other.

This break concerns PRRTE and other host environments only.
Applications are not affected:

* An application built against PMIx v5 runs, without being rebuilt,
  against an installed PMIx v7 library.
* An application in a container that carries its own PMIx v5 client
  library runs under a PMIx v7 server - for example, one launched by
  PRRTE v5.


What happened to PMIx v6 and PRRTE v4
-------------------------------------

PMIx v6 and PRRTE v4 were released as a pair:

* PMIx v6.0.0 and PRRTE v4.0.0 in May 2025;
* PMIx v6.1.0 and PRRTE v4.1.0 in February 2026.

No further releases were made in either series; PMIx v6.1.1 and PRRTE
v4.1.1 reached release candidates only. Both series are now end of
life, and receive no fixes (see :doc:`/security`).

The series ended because of the scale of the work that followed them.
A wave of fixes reached both code bases after the projects adopted
AI-assisted programming, and with it came a comprehensive test harness:
thousands of unit tests, and a multi-node test environment built on
Docker Swarm. The changes were too extensive to carry back into v6 and
v4. Since releases had already been made in those series, the changes
called for a new major release - PMIx v7 and PRRTE v5. The new pair is
a significant improvement, so there was no reason to keep the old one
alive.

**A system running PMIx v6 and PRRTE v4 should move to PMIx v7 and
PRRTE v5**, as a pair.


Open MPI
--------

Open MPI includes a PMIx and a PRRTE in its source distribution, and
they are a matching pair:

.. list-table::
   :header-rows: 1
   :widths: 30 35 35

   * - Open MPI series
     - Includes PMIx
     - Includes PRRTE
   * - v5.0
     - v5.0
     - v3.0
   * - v6.0
     - v7.0
     - v5.0 (Open MPI's own copy)

If you install PMIx and PRRTE yourself, build Open MPI against them so
that the system has one PMIx and one PRRTE. Give Open MPI's
``configure`` **both** locations:

.. code-block:: sh

   shell$ ./configure --with-pmix=<pmix-dir> --with-prrte=<prrte-dir> ...

Give both even if you only want to change PMIx. Without
``--with-prrte``, Open MPI builds its included PRRTE, and that PRRTE
refuses a PMIx outside its own range: the PRRTE in Open MPI v5.0
refuses PMIx v7, for example.

Open MPI v5.0 can be built against an installed PMIx v7 and PRRTE v5
this way.

.. DRAFT: Open MPI v6 with an external PMIx v5 / PRRTE v3 - configure
   appears to accept it (no maximum version check, and no use of
   v6/v7-only PMIx names), but it has not been built or run. Decide
   what to say once checked.
