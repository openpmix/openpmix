PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

6.1.0 -- TBD
------------
.. important:: This is the second release in the v6 family. The release
               is based on a refork of the PMIx master branch as the
               changes since v6.0.0 were extensive. Many of the changes
               were bug fixes, but the following significant changes
               are included:

               * build requirements for Git clones includes a minimum
                 Python version of 3.6 - the requirement does not apply
                 to builds from tarball
               * server upcalls no longer require that the upcall return
                 prior to the server executing the provided callback
                 function
               * all APIs are now threadshifted prior to execution for
                 thread safety. Hosts that are providing their own
                 progress engine (in lieu of using the PMIx internal
                 progress thread) must ensure that progress is being
                 provided sufficient to avoid threadlock when calling
                 PMIx APIs.
               * listener thread ports can now be specified as a comma-delimited
                 list of ranges instead of only a single port
               * connection authentication has been tightened and greater
                 controls provided via attributes
               * support for process and node statistics gathering has
                 been added as per the Standard
               * a new API (PMIx_Progress_thread_stop) has been added to
                 direct that the internal progress thread be stopped. This
                 allows the host to stop the progress thread independent
                 from calling a PMIx "finalize" routine.


Detailed changes since refork include:
 - PR #: Multiple commits
    - Silence a few Coverity warnings
    - Enable including help topics in topics
    - Silence gcc15 warnings
 - PR #3800: Multiple commits
    - Make thread start/stop marker consistent
    - Final NEWS update
 - PR #3798: Update NEWS and VERSION for rc1
 - PR #3797: Multiple commits
    - fix a problem after second pmix init
    - Add missing attribute
 - PR #3794: Do not double-process IOF formats
 - PR #3792: Multiple commits
    - Cleanup inii/finalize cycle
    - Do not shutdown libevent during finalize
 - PR #3788: Multiple commits
    - Update PMIx_Fence to fully conform to Standard
    - Threadshift IOF API calls
    - Update log support to conform to Standard
    - update-my-copyright.py: properly support git workspaces
    - Seal memory leak
    - Update the monitor_multi example
    - Stop the progress thread right away in server_finalize
    - Silence valgrind issues
    - Implement new API to stop the progress thread
    - Allow passing of progress thread to stop - default NULL to all
    - Protect callbacks from threadshift when progress thread is stopped
    - Add capability: get number function available
    - Ensure to store group info in PMIx server
    - Revamp the pmix_info support
    - Fine-tune the show-version option
    - Improve description of PMIx_Compute_distances API
    - Fully support return of static values
    - Cleanup and abstract pmix_info support
    - Correctly threadshift PMIx_IOF_push directives
    - Correct cflags used for check_compiler_version.m4


Detailed changes since v6.0.0 included from master:
 - PR #3759: Silence some Coverity warnings
 - PR #3751: Potential double free and use after free (alerts 13,14)
 - PR #3750: Potentially overflowing call to snprintf (alerts 11, 12)
 - PR #3749: Workflow does not contain permissions
 - PR #3748: printf.c: fix off-by-one + underflow errors
 - PR #3747: Use floating numbers for float/double comparisons
 - PR #3743: Extend debugger CI tests
 - PR #3741: Fix indirect debugger launch
 - PR #3739: Add refresh test
 - PR #3735: Cleanup ready-for-debug announcement
 - PR #3734: Fix cmd line option checker
 - PR #3732: Fix bitmap mask literal size
 - PR #3729: Check if we fork'd the tool ourselves
 - PR #3724: Protect against equal signs in option check
 - PR #3722: Implement support for resource usage monitoring
 - PR #3718: Enable use of loopback interface
 - PR #3716: Add new PMIX_GROUP_FINAL_MEMBERSHIP_ORDER attribute
 - PR #3714: Replace sprintf with snprintf
 - PR #3713: Replace int taint limits with defined names
 - PR #3712: Silence latest Coverity warning report
 - PR #3711: Flush namespace sinks' residuals before destroying namespace
 - PR #3710: Port bug fixes to zlibng component
 - PR #3709: bitmap num_set boundary condition bugfix
 - PR #3708: preg/compress parsing bugfix
 - PR #3707: Silence Coverity warning
 - PR #3706: Update the plog framework
 - PR #3705: Check only for existence of PMIx capability flag
 - PR #3702: Remove unnecessary locks from munge psec module
 - PR #3701: Avoid use of API in PMIx_Init
 - PR #3700: Silence Coverity warnings
 - PR #3699: Silence Coverity warnings
 - PR #3697: Silence Coverity warnings
 - PR #3696: Switch to atomics for tracking initialization
 - PR #3695: Change to using atomic for show_help_enabled
 - PR #3694: Silence Coverity warning
 - PR #3693: Remove stale/unused tests
 - PR #3692: Silence more Coverity warnings
 - PR #3691: Silence Coverity warnings
 - PR #3690: Use the correct value for the number of info to unpack
 - PR #3689: Silence more Coverity warnings
 - PR #3688: Silence Coverity warnings
 - PR #3686: Extend listener thread port specification to support ranges
 - PR #3684: Fix compression components
 - PR #3682: Add attribute to request reports be in physical CPU IDs
 - PR #3681: Add set-env cmd line option definition
 - PR #3680: Minor change to thread construct/ops
 - PR #3678: Fix error code on blocking PMIx_Notify_event calls
 - PR #3676: Silence a few Coverity complaints
 - PR #3675: Improve selection of interfaces
 - PR #3674: Multiple commits
    - Do not remove nspace from global list on rejected connection
    - Allow foreign tools by default
    - Cleanup a bit on connection handling
    - Avoid duplicate namespace entries
 - PR #3672: define default MAXPATHLEN if not defined by system
 - PR #3670: Bugfix in pmix_bitmap_num_set_bits
 - PR #3669: Fix the abort server upcall
 - PR #3667: Update listener thread setting of permissions on connection files
 - PR #3664: Extend authentication support
 - PR #3663: Provide more info on connections
 - PR #3662: Pass the client's pid as well
 - PR #3661: ci: add group_bootstrap to CI
 - PR #3659: Prevent memory overrun in regx calculation
 - PR #3658: Silence Coverity complaints
 - PR #3657: Revamp stats implementation to reflect Standard
 - PR #3656: Pass the uid/gid for client connections
 - PR #3654: Provide better FQDN support
 - PR #3651: Don't fail when PMIX_IOF_OUTPUT_TO_FILE directory exists
 - PR #3650: Parameterize client finalize timeout
 - PR #3649: Update termios right away
 - PR #3648: Continue work on pty support
 - PR #3647: Work on enabling "pty" behaviors
 - PR #3646: Always search help arrays if initialized
 - PR #3643: Check return code for notify ready-for-debug
 - PR #3642: Add debugger checks to CI
 - PR #3641: Correct client notify of ready for debugger
 - PR #3640: Only report bad prefix if verbose requested
 - PR #3638: Change default show-load-errors to "none"
 - PR #3637: Prevent show-help from using IOF too soon
 - PR #3635: Update to track changes in Standard
 - PR #3633: Cleanup some group docs
 - PR #3632: Update CI
 - PR #3631: Ensure cleanup of allocated pmix_info_t
 - PR #3630: Properly trigger the "keepalive failed" event
 - PR #3629: Provide better singleton support and support blocking event notify
 - PR #3628: python-bindings: add CI and avoid 'long' integer error
 - PR #3622: Update OAC submodule
 - PR #3620: Handle some corner cases for data ops
 - PR #3619: Update the Data pack/unpack functions
 - PR #3618: First set of API updates
 - PR #3615: Check for pthread_np.h header
 - PR #3614: Complete sweep of server upcall callback functions
 - PR #3613: Add a PMIX_FWD_ENVIRONMENT attribute
 - PR #3610: Threadshift the PMIx_Notify_event API
 - PR #3609: Delete built files on "make clean"
 - PR #3608: Decrease min Py version to 3.6
 - PR #3607: Continue work on threadshifting all upcall callbacks
 - PR #3606: Continue work on threadshifting all upcall callbacks
 - PR #3605: Ensure more upcall cbfuncs threadshift
 - PR #3602: Fix the wrapper compiler
 - PR #3601: Ensure to threadshift callback functions
 - PR #3599: Update news from release branches


6.0.0 -- 19 May 2025
--------------------
.. important:: This is the first release in the v6 family. The intent
               for this series is to provide regular "reference tags",
               effectively serving as milestones for any development
               that might occur after the project achieved a stable
               landing zone at the conclusion of the v5 series. It
               is expected, therefore, that releases shall be infrequent
               and rare occurrences, primarily driven by the completion
               of some significant feature or some particularly
               critical bug fix.

               For this initial release, that feature is completion of
               the Group family of APIs. This includes support for all
               three of the group construction modes, including the new
               "bootstrap" method. A description of each mode can be
               found in the :ref:`Group Construction <group-construction-label>`
               section of the documentation.

               A few notes:

               (1) Proper execution of the various group construction
               modes requires that the host provide the necessary backend
               support. Please check with your host provider to ensure it
               is available, or feel free to use the PMIx Reference
               RunTime Environment (`PRRTE <https://github.com/openpmix/prrte/releases>`_) - you will require v4.0 or above.

               (2) Starting with this release, PMIx requires
               Python >= v3.7 to build a Git clone (ie., not a tarball).
               Certain elements of the code base are constructed at build
               time, with the construction performed by Python script. The
               constructed elements are included in release tarballs.

               (3) PRRTE < v4.0 is not compatible with PMIx >= v6.0 due
               to internal changes (e.g., show-help messages are now
               contained in memory instead of on-disk files).

A full list of individual changes will not be provided here,
but will commence with the v6.0.1 release.
