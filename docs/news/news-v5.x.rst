PMIx v5.x series
================

This file contains all the NEWS updates for the PMIx v5.x
series, in reverse chronological order.

5.0.5 -- 15 Dec 2024
--------------------
Detailed changes include:
 - PR #3469: Final update for release
 - PR #3468: Remove remnants of unsupported capability
 - PR #3467: Remove stale configure.m4 scripts
 - PR #3465: Update NEWS and VERSION
 - PR #3463: Remove prm framework as not currently implemented
 - PR #3462: Remove unused components from various frameworks
 - PR #3461: Remove pgpu framework
 - PR #3460: Remove pstrg framework
 - PR #3452: .gitignore: add configure~
 - PR #3449: Multiple commits
   - Retry fetch with wildcard rank
   - Fix typo in shmem2 fetch


5.0.4 -- 13 Nov 2024
--------------------
.. important:: This release represents the expected end of
               the v5.0 series. Any follow-on bug fixes will
               be committed to the release branch, but are
               unlikely to generate an official release
               tarball.

Detailed changes include:
 - PR #3441: Update NEWS and VERSION for final release
 - PR #3440: Minor cleanups plus resolve peers example
 - PR #3434: Collapse the pfexec framework
 - PR #3432: Update VERSION for release
 - PR #3430: Multiple commits
   - Update NEWS to include v5.0 branch
   - Drop the sphinx required level to match PRRTE
   - Ensure IOF respects formatting requests
 - PR #3419: Add some missing attributes
 - PR #3417: Multiple commits
   - Fix typo in equality check
   - Fix delayed get
   - avoid warn-as-error for variable init
   - Add support for libz-ng
 - PR #3408: Update pmix_portable_platform_real.h from upstream gasnet
 - PR #3404: Path must start with "src"
 - PR #3402: Remove unused yaml
 - PR #3400: add contrib/construct_event_strings.py to the dist tarball
 - PR #3397: Multiple commits
   - Add missing files
   - mca/pif: fix pmix_found_linux typo
   - Add cross-version compatibility to docs
 - PR #3393: Multiple commits
   - Add python directive
   - Cleanup pfexec spawn operations
   - Add missing function call
 - PR #3387: Update OAC to latest HEAD
 - PR #3385: Correctly check MCA params
 - PR #3383: Protect against LTO optimizer
 - PR #3381: Read The Docs updates
 - PR #3379: Multiple commits
   - Revert Sphinx requirements
   - Warn against building tarball on MacOSX
   - configure: fix regression that caused python to be mandatory to build
   - configure: fix broken bashisms resulting in logic failure
   - Update the requirements for Sphinx
 - PR #3372: Multiple commits
   - Update MLNX CI
   - Apply prefix to copied version of the app array

5.0.3 -- 8 Jul 2024
-------------------
Detailed changes include:
 - PR #3369: Update NEWS and VERSION for release
 - PR #3366: Transfer results from partial success of lookup
 - PR #3363: Multiple commits
   - Fix singletons
   - Protect against NULL fields
 - PR #3361: Remove unused function in shmem2
 - PR #3357: Github action: bring back MacOS builds
 - PR #3354: Multiple commits
   - Don't strip quotes from cmd line entries
   - Handle single character filenames
   - Update tar format to tar-pax
   - Perform some cleanup
   - Include devel-check status in configure summary
   - Turn off MacOS CI
 - PR #3334: Fix function declaration
 - PR #3332: Fixes for PR3329

5.0.2 -- 21 Mar 2024
--------------------
.. important:: Cross-version incompatibility
               The known issue of cross-version operability between
               members of the PMIx v5.0 release series has been
               resolved in this release. Thus, v5.0.2 and all subsequent
               releases can operate across versions, including the
               v5.0.1 and v5.0.0 releases.

Detailed changes include:
 - PR #3330: Do not include PMIX_PREFIX in spawn upcall
 - PR #3325: Multiple commits
    - Correctly set the app cmd and argv0 fields
    - Don't overwrite user's args
    - Correct error in retrieval of node and app info
 - PR #3319: Toughen the submodule checks in autogen.pl
 - PR #3317 Correct group modex storage to avoid duplication
 - PR #3314 Fix memory leak in storing of modex data
 - PR #3311 More cleanup of group operations and local client array
 - PR #3307 Include notes about submodules in docs
 - PR #3299 Multiple commits
    - gds/shmem2: provide a useful error message on memory allocation failure
    - Add "close stale issues" actions
    - oac: strengthen Sphinx check
    - Remove stat call when destroying a dirpath
    - Do not remove the system tmpdir during cleanup
 - PR #3293 Multiple commits
    - gds/shmem: fix build
    - Update how PMIx attributes are looked up.
    - Improve PMIx attribute lookup efficiency.
    - gds/shmem: improve cross-version capabilities
    - Revert "Disable gds/shmem at runtime"
    - Revert "gds/shmem: fix build."
    - Rename the gds/shmem component to gds/shmem2
    - Protect output files during cleanup
    - Begin to add man pages for PMIx commands
    - Restore support for HWLOC truly ancient
    - Continue work on tool man pages
    - Fix the dictionary transfer in shmem2
 - PR #3280 Multiple commits
    - Implement attribute to specify connection order and process MCA params
    - Error out of attempts for 32-bit builds
    - hash: Add internal APIs that specify target key index.
    - hash: Update pmix_hash functions to accept a pmix_keyindex_t*
    - gds/shmem: Improve error message in tma_realloc()
    - Remove static version of global function
    - Fix handling of "--" in cmd lines
    - Update the doubleget test
    - Fully implement refresh cache support
    - Adjust preg component priorities
    - Remove unused function
    - gds/shmem: Implement first cut of tma_realloc()
    - Begin work on removing use of "stat"
    - Fix typo
    - avoid loopback in resolve_nspace_requests
    - Refactor the prm framework
    - Assign NULL to free'd pointer
    - Cleanup some "unused params" errors
    - Protect a variable
    - Check for stdatomic.h
    - Remove pmix_osd_dirpath_access
    - Remove use of stat from pmix_getcwd
    - Remove use of stat
    - Remove use of stat in pmix_globals
    - Remove use of stat to check file existence
    - Test open a dir instead of using stat
    - Minor cleanups for unused params
    - pmix.h: Add capability flags
    - Cleanup comments and prep for commit
    - Do not remove the system tmpdir during cleanup
    - Cleanup palloc and prun connections
    - Cleanup a few typos and remove debug output
    - Cast a few parameters when translating macros to functions
    - Resolve problem of stack variables and realloc
    - Restore support for detecting shared file systems
    - Properly handle directories during cleanup
    - gds/shmem: revert tma_free() strategy
    - gds/shmem: fix potentially confusing error output
    - Touchup the dirpath_destroy code
    - Fix broken link in README
    - Add a query attribute for number of available slots
    - Do not add no-unused-parameter for non-devel-check builds
    - Better support global keys
    - PMIx_Query_info: removed duplicated PMIX_RELEASE
    - Provide an explanation of session directories
    - Fix --enable-devel-check builds
    - Restore default to enable-devel-check in Git repos
    - Protect against empty envar definition for mca_base_param_files
    - Fix test builds with picky compiler options
    - Protect against NULL hash table labels in debug output
    - Update the Python regex for doc build
    - Disable gds/shmem at runtime
    - Cleanup update
 - PR #3182 Multiple commits
    - Remove debug print
    - Make checking min versions consistent
    - Add an action to test older HWLOC version
    - Touchup the OMPI integration
    - Fix couple of bugs in cmd line parser
    - Fix typo in cmd line processor
    - Add a new attribute to specify connection order
 - PR #3166: Blacklist the HWLOC GL component to avoid deadlock
 - PR #3162: Add a new Github Action


5.0.1 -- 9 Sep 2023
-------------------
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

.. warning:: Cross-version incompatibility

             There is a known issue when operating between
             PMIx versions v5.0.1 and v5.0.0. This occurs due
             to a difference in the key-to-index conversion
             between the two versions. Users are advised
             to set the PMIX_MCA_gds=hash parameter
             in their environment when using these two
             versions.

Detailed changes include:
 - Update news and version for release
 - PR #3149 Multiple commits
    - Do not follow links when doing "chown"
    - Cleanup a little debug in new pctrl tool
 - PR #3145 Multiple commits
    - Retrieve pset names upon PMIx_Get request
    - Add a new "pctrl" tool for requesting job control ops
 - PR #3144 Multiple commits
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
    - Roll to version 5.0.1


5.0.0 -- 7 Aug 2023
-------------------
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
