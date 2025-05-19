PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

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
