# -*- shell-script -*-
#
# Copyright (C) 2015-2017 Mellanox Technologies, Inc.
#                         All rights reserved.
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pgpu_amd_CONFIG(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([MCA_pmix_pgpu_amd_CONFIG],[
    AC_CONFIG_FILES([src/mca/pgpu/amd/Makefile])

    # No real AMD/ROCm-runtime detection exists yet, so this component
    # only builds under --enable-test-build for compile coverage.
    AS_IF([test "$pmix_testbuild" = "1"],
          [$1
           pmix_pgpu_amd_happy=yes],
          [$2
           pmix_pgpu_amd_happy=no])

    PMIX_SUMMARY_ADD([GPUs], [AMD], [],[$pmix_pgpu_amd_happy])
])

