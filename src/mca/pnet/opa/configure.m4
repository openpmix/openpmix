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
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pnet_opa_CONFIG([action-if-can-compile],
#                     [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pnet_opa_CONFIG],[
    AC_CONFIG_FILES([src/mca/pnet/opa/Makefile])

    PMIX_CHECK_PSM2([pnet_psm2],
                    [pnet_psm2_happy="yes"],
                    [pnet_psm2_happy="no"])

    pnet_opa_CFLAGS=
    pnet_opa_CPPFLAGS=
    pnet_opa_LDFLAGS=
    pnet_opa_LIBS=

    AS_IF([test "$pnet_psm2_happy" = "yes"],
          [PMIX_SUMMARY_ADD([[Transports]],[[PSM2]], [pnet_opa], [yes ($pmix_check_psm2_dir)])
           pnet_opa_CFLAGS="$pnet_opa_CFLAGS $pnet_psm2_CFLAGS"
           pnet_opa_CPPFLAGS="$pnet_opa_CPPFLAGS $pnet_psm2_CPPFLAGS"
           pnet_opa_LDFLAGS="$pnet_opa_LDFLAGS $pnet_psm2_LDFLAGS"
           pnet_opa_LIBS="$pnet_opa_LIBS $pnet_psm2_LIBS"],
          [PMIX_SUMMARY_ADD([[Transports]],[[PSM2]], [pnet_opa], [no])
           $2])

    AS_IF([test "$pmix_ofi_happy" = "yes"],
          [PMIX_SUMMARY_ADD([[Transports]],[[OFI]], [pnet_opa], [yes ($pmix_ofi_dir)])
           pnet_opa_CPPFLAGS="$pnet_opa_CPPFLAGS $pmix_ofi_CPPFLAGS"
           pnet_opa_LDFLAGS="$pnet_opa_LDFLAGS $pmix_ofi_LDFLAGS"
           pnet_opa_LIBS="$pnet_opa_LIBS $pmix_ofi_LIBS"],
          [PMIX_SUMMARY_ADD([[Transports]],[[OFI]], [pnet_opa], [no])
           $2])

    # substitute in the things needed to build opa
    AC_SUBST([pnet_opa_CFLAGS])
    AC_SUBST([pnet_opa_CPPFLAGS])
    AC_SUBST([pnet_opa_LDFLAGS])
    AC_SUBST([pnet_opa_LIBS])
])dnl
