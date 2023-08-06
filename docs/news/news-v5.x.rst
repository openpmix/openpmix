PMIx v5.x series
================

This file contains all the NEWS updates for the PMIx v5.x
series, in reverse chronological order.

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
