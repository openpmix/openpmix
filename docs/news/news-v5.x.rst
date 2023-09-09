PMIx v5.x series
================

This file contains all the NEWS updates for the PMIx v5.x
series, in reverse chronological order.

5.0.1 -- 9 Sep 2023
----------------------
.. warning:: CVE-2023-41915

    A security issue was reported by François Diakhate (CEA)
    which is addressed in the PMIx v4.2.6 and v5.0.1 releases.
    (Older PMIx versions may be vulnerable, but are no longer
    supported.)

    A filesystem race condition could permit a malicious user
    to obtain ownership of an arbitrary file on the filesystem
    when parts of the PMIx library are called by a process
    running as uid 0. This may happen under the default
    configuration of certain workload managers, including Slurm.

Detailed changes include:
 - PR #3149 Multiple commits
    - Do not follow links when doing `chown`
    - Cleanup a little debug in new pctrl tool
 - PR #3145
    - Retrieve pset names upon PMIx_Get request
    - Add a new "pctrl" tool for requesting job control ops
 - PR #3144
    - Properly support the "log" example
    - show_help: strip leading/trailing blank lines
    - docs: fix some leftover "Open MPI" references
    - docs: fix HTML word wapping in table cells
    - Improve error handling in setup_topology
    - Define a new server type and connection flags
    - Minor cleanups for disable-dlopen
    - Fix Python bindings
 - PR #3131 Multiple commits
    - Switch to using event lib for connections
    - Roll to v5.0.1


5.0.0 -- 6 Aug 2023
----------------------
.. important:: This is the first release in the v5 family
               and includes some significant changes, both internal
               and user-facing. A partial list includes:

               * initial attribute and API definitions in support of
                 scheduler integration to both applications and
                 resource managers/runtime environments.

               * a new shared memory implementation that removes the need
                 for special "workaround" logic due to limitations in the
                 prior method

               * support for "qualified" values whereby an application
                 can post multiple values to the same key, each with one
                 or more qualifiers - and then retrieve the desired one
                 by specifying the appropriate qualifier.

               * availability of both function and macro equivalents
                 for all support operations (e.g., PMIX_ARGV_APPEND and
                 PMIx_Argv_append). Note that the macro versions have
                 been deprecated by the PMIx Standard, but remain highly
                 recommended for use by those compiling against the
                 library (as opposed to dlopen'ing it)

A full list of individual changes will not be provided here,
but will commence with the v5.0.1 release.
