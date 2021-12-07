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
# Copyright (c) 2004-2006 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006      QLogic Corp. All rights reserved.
# Copyright (c) 2009-2021 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016      Intel Corporation. All rights reserved.
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
#  Copyright (c) 2021     Triad National Security, LLC. All rights
#                         reserved.
#
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# OMPI_CHECK_PSM2(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if PSM2 support can be found. runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([OMPI_CHECK_PSM2],[

	AC_ARG_WITH([psm2],
		    [AS_HELP_STRING([--with-psm2(=DIR)],
				    [Build PSM2 (Intel PSM2) support, optionally adding DIR/include to the search path for headers])])
    AS_IF([test ! -z "$with_psm2" && test "$with_psm2" != "yes"],
          [pmix_psm2_dir="$with_psm2"],
          [pmix_psm2_dir=""])

    dnl Just check for the presence of the header for now

    _PMIX_CHECK_PACKAGE_HEADER([pmix_psm2], [psm2.h], [pmix_psm2_dir],
                               [pmix_psm2_happy=yes],
                               [pmix_psm2_happy=no])

    AC_MSG_CHECKING([if have psm2 support])
    AC_MSG_RESULT([$pmix_psm2_happy])

    PMIX_SUMMARY_ADD([[External Packages]],[[PSM2]],[pmix_psm2],[$pmix_psm2_happy])])])
])
