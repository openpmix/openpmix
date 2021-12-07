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
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pgpu_amd_CONFIG(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if CUDA support can be found.  runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([MCA_pmix_pgpu_amd_CONFIG],[
    AC_CONFIG_FILES([src/mca/pgpu/amd/Makefile])

    AS_IF([test "$pmix_cuda_happy" = "yes"],
          [$1],
          [$2])

    PMIX_SUMMARY_ADD([[GPUs]],[[AMD]],[pgpu_amd],[$pmix_cuda_happy])])])
])

