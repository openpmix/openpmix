# -*- shell-script -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2025      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# MCA_pmix_pstat_pmacos_CONFIG(action-if-can-compile,
#                              [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pstat_pmacos_CONFIG],[
    AC_CONFIG_FILES([src/mca/pstat/pmacos/Makefile])

    AC_REQUIRE([OAC_CHECK_OS_FLAVORS])

    # The macOS reader is built only on Apple (Darwin), where the mach,
    # libproc, sysctl, and IOKit interfaces it uses are guaranteed to be
    # present. It links the IOKit and CoreFoundation frameworks for the
    # per-disk statistics.
    AS_IF([test "$oac_found_apple" = "yes"],
          [pstat_pmacos_happy="yes"],
          [pstat_pmacos_happy="no"])

    AS_IF([test "$pstat_pmacos_happy" = "yes"],
          [pstat_pmacos_LIBS="-framework IOKit -framework CoreFoundation"
           AC_SUBST([pstat_pmacos_LIBS])
           PMIX_EMBEDDED_LIBS="$PMIX_EMBEDDED_LIBS $pstat_pmacos_LIBS"
           $1],
          [$2])
])
