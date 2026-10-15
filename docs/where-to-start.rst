.. _label-where-to-start:

Where to start
==============

This documentation serves several kinds of reader. Find yours below
and start with the pages listed for it.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - If you are...
     - Start with
   * - **Installing PMIx**, or packaging it for a distribution
     - :ref:`label-quickstart`, then
       :ref:`building-and-installing-section-label`.
   * - **A system administrator** choosing releases, pairing PMIx with
       PRRTE, or deciding how PMIx authenticates connections
     - :ref:`label-sysadmin`, then :doc:`/mca` for how to set
       parameters.
   * - **Writing an application, library, or tool** that calls the PMIx
       client or tool APIs
     - :doc:`/building-apps/index`, then the
       :doc:`manual pages </man/index>` for each API you use.
   * - **Writing a host environment** - a resource manager, launcher,
       scheduler, or other daemon that embeds the PMIx *server* library
     - :ref:`label-host-env`. It says what PMIx does for you, what it
       expects you to do, and which of the two decides who may do what.
   * - **Working on PMIx itself**
     - :doc:`/developers/index` for the source tree and its rules, and
       :doc:`/how-things-work/index` for how the major subsystems work.
   * - **Reporting a problem**
     - :doc:`/getting-help`. Report a suspected security issue privately,
       as described in :doc:`/security`.


What each part covers
---------------------

:doc:`Manual pages </man/index>`
   The reference for every public API, data type, and command. Each
   page states what a call does, its arguments, and the attributes it
   accepts. Section 3 holds the functions, section 5 the data types
   (including :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`,
   the table of up-calls a host provides), and section 1 the commands.

:ref:`System administration <label-sysadmin>`
   Which PMIx release to install, which PRRTE release goes with it,
   and how to control the way PMIx authenticates connections.

:ref:`Host environments <label-host-env>`
   The contract between the PMIx server library and the program that
   hosts it. It covers authentication, authorization, and the work each
   side does for each operation.

:doc:`How things work </how-things-work/index>`
   How PMIx implements particular features: the modex, collectives,
   groups, authentication methods, shared-memory job data, and others.
   Read these when you need to know *why* PMIx behaves as it does.

:doc:`The MCA </mca>`
   How to set PMIx's run-time parameters - on the command line, in the
   environment, or in parameter files.

:doc:`Security </security>`
   How to report a security issue, which versions receive fixes, and
   which behaviors are intended.

:doc:`Version compatibility </versions>`
   Which PMIx versions interoperate, and the rules for binary
   compatibility.
