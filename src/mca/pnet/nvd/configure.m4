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

# MCA_pmix_pnet_nvd_CONFIG(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if UCX support can be found.  runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([MCA_pmix_pnet_nvd_CONFIG],[
    AC_CONFIG_FILES([src/mca/pnet/nvd/Makefile])

    PMIX_VAR_SCOPE_PUSH([pmix_check_ucx_dir])

    AS_IF([test -z "$pmix_check_ucx_happy"],
          [AC_ARG_WITH([ucx],
		       [AS_HELP_STRING([--with-ucx(=DIR)],
				       [Build with Unified Communication X library support])])

    AS_IF([test ! -z "$with_ucx" && test "$with_ucx" != "yes"],
          [pmix_ucx_dir="$with_ucx"],
          [pmix_ucx_dir=""])

    dnl Just check for the presence of the header for now

    _PMIX_CHECK_PACKAGE_HEADER([pmix_ucx], [include/ucp/api/ucp.h], [pmix_ucx_dir],
                               [pmix_ucx_happy=yes],
                               [pmix_ucx_happy=no])

    AS_IF([test "$pmix_ucx_happy" = "yes"],
          [$1],
          [AS_IF([test ! -z "$with_ucx" && test "$with_ucx" != "no"],
                 [AC_MSG_ERROR([NVIDIA support requested but UCX not found.  Aborting])])
           $2])

    PMIX_SUMMARY_ADD([[Transports]],[[NVIDIA]],[pnet_nvd],[$pmix_ucx_happy])])])

    PMIX_VAR_SCOPE_POP
])

