PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

6.1.1 -- xx May 2026
--------------------
Detailed changes since v6.1.0:
 - Added the --enable-test-build configure option. It force-builds the
   test and environment-specific components - the GPU vendor components,
   the NVIDIA/simptest/TCP transports, the pgpu test component, and the
   optional-library wrappers (zlib, zlib-ng, libesmtp, MUNGE) - so they
   can be compile-checked on a machine that lacks the supporting hardware
   or libraries. Components that need a third-party header supply a
   non-functional shim header used under the resulting PMIX_TESTBUILD
   macro. The option is intended for developers and CI: the shimmed
   components are not functional, so a test-build must not be installed
   for real use
 - Revived the pnet/simptest test fabric component. It had grown stale
   (its module signatures and internal node struct no longer matched the
   pnet interface) and no longer compiled; it has been ported to the
   current interface and once again drives the static endpoint/coordinate
   assignment path end-to-end. Fabric endpoints are delivered as per-proc
   data and fabric coordinates as per-node info, matching how PMIx_Get
   resolves each
 - PMIx_Load_topology no longer leaks the source string. When the caller
   does not request a specific source, the returned topology's source
   field now points to a read-only, statically allocated string instead
   of a heap copy the caller had no safe way to release (the returned
   topology is shared, library-managed state that must not be destructed
   by the caller). A source the caller did supply remains theirs to free.
   The PMIx_Load_topology man page documents this ownership split
 - Group construct fault handling. When a member is lost before
   contributing to a PMIx_Group_construct, the server now consults the
   PMIX_GROUP_FT_COLLECTIVE directive (previously ignored): by default
   the construct aborts with PMIX_GROUP_CONSTRUCT_ABORT rather than
   silently forming a reduced group, while with the directive set it
   completes on the survivors and notifies each of them via a
   PMIX_GROUP_MEMBER_FAILED event naming the lost member. The server
   synthesizes PMIX_GROUP_MEMBER_FAILED itself, so this works with any
   host environment
 - Group destruct fault handling. When a member is lost before
   contributing to a PMIx_Group_destruct, the server now consults the
   PMIX_GROUP_NOTIFY_TERMINATION directive (previously ignored, so the
   documented behavior never occurred): by default the teardown completes
   on the survivors but is reported as an error, while with the directive
   set each survivor receives a PMIX_GROUP_MEMBER_FAILED event naming the
   lost member and the destruct returns PMIX_SUCCESS - the event serving
   in place of an error. The directive is supplied at construct time but
   governs the destruct; the library remembers it and re-applies it
   automatically (it may also be passed directly to PMIx_Group_destruct)
 - Group leader failure. A process that accepts an invitation with
   PMIx_Group_join_nb now watches the group leader; if the leader
   terminates before the construct completes, the library delivers a
   PMIX_GROUP_LEADER_FAILED event to the waiting process (naming the
   leader) so the application can drive reselection, rather than
   blocking indefinitely
 - Group construct timeout. A PMIX_TIMEOUT supplied to
   PMIx_Group_construct is now enforced by the server for the local
   phase: if a live participant never contributes, the construct
   completes with PMIX_ERR_TIMEOUT instead of hanging
 - Added the PMIX_GROUP_CANCEL group operation (a host callback used to
   abort an in-flight cross-server construct) and the PMIX_CAP_GROUP_FT
   capability flag advertising the group fault-tolerance feature set
 - PMIx_Group_leave / PMIx_Group_leave_nb are now implemented. Previously
   the client sent a command the server never handled, so the call
   returned PMIX_ERR_NOT_SUPPORTED and the documented PMIX_GROUP_LEFT
   event was never generated. A voluntary leave now generates a
   PMIX_GROUP_LEFT event notifying the remaining members of the caller's
   departure (returning success once the event is locally generated, per
   the API contract), updates group membership on the members and in the
   host environment, and completes any in-flight group construct/destruct
   collective on the survivors rather than hanging
 - Resolve a status-code value collision: PMIX_ERR_LOST_PRECISION and
   PMIX_ERR_CHANGE_SIGN shared the values -400 and -401 with the
   PMIX_ERR_PROC_KILLED_BY_CMD and PMIX_ERR_PROC_FAILED_TO_START event
   codes, making the two indistinguishable to event handlers, status
   comparisons, and PMIx_Error_string. The two conversion-error codes
   have been renumbered to -202 and -203
 - gds/shmem2: make fixed-address segment attach reliable, fixing an
   intermittent spawn-time PMIx_Init failure (PMIX_ERR_PACK_MISMATCH)
   seen under AddressSanitizer and during MPI_Comm_spawn
 - gds: when a client cannot attach a gds/shmem2 fixed-address segment
   at runtime, it now gracefully falls back to the next available GDS
   module (typically hash) and re-requests its job data, instead of
   failing PMIx_Init. GDS module selection is now tracked per-client
   rather than per-namespace, so one client falling back does not affect
   its peers
 - plog/stdfd: implement the output-formatting directives for the
   stdout/stderr log channels. PMIx_Log requests that carry
   PMIX_LOG_TAG_OUTPUT, PMIX_LOG_TIMESTAMP_OUTPUT, and/or
   PMIX_LOG_XML_OUTPUT now have their output tagged, timestamped, and/or
   wrapped in XML, matching the formatting used for forwarded stdio.
   These directives were previously accepted but silently ignored
 - iof: fix XML output escaping. An ampersand was emitted as the invalid
   entity "&ap;" instead of "&amp;", and each escaped character left a
   trailing zero byte in the output due to an allocation-size overcount.
   This affected all XML-formatted stdout/stderr output
 - plog/smtp: fix the "body_suffix" MCA parameter, which was mistakenly
   registered under the name "body_prefix" and so could not be set

6.0.0 -- TBD
------------
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
