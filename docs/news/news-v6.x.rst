PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

6.1.1 -- xx May 2026
--------------------
Detailed changes since v6.1.0:
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
