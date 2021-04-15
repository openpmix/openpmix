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
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Sandia National Laboratories.  All rights reserved.
# Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pstrg_lustre_CONFIG([action-if-can-compile],
#                     [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pstrg_lustre_CONFIG],[
    AC_CONFIG_FILES([src/mca/pstrg/lustre/Makefile])

    PMIX_CHECK_LUSTRE([pstrg_lustre],
                      [pstrg_lustre_happy="yes"],
                      [pstrg_lustre_happy="no"])


    AS_IF([test "$pstrg_lustre_happy" = "yes"],
          [PMIX_SUMMARY_ADD([[Optional Support]],[[Lustre]], [pstrg_lustre], [yes ($pmix_check_lustre_dir)])
           $1],
          [PMIX_SUMMARY_ADD([[Optional Support]],[[Lustre]], [pstrg_lustre], [no])
           $2])

    # substitute in the things needed to build lustre support
    AC_SUBST([pstrg_lustre_CFLAGS])
    AC_SUBST([pstrg_lustre_CPPFLAGS])
    AC_SUBST([pstrg_lustre_LDFLAGS])
    AC_SUBST([pstrg_lustre_LIBS])
])dnl
