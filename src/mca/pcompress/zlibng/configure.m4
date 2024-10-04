# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pcompress_zlibng_CONFIG([action-if-can-compile],
#                           [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pcompress_zlibng_CONFIG],[
    AC_CONFIG_FILES([src/mca/pcompress/zlibng/Makefile])

    AC_ARG_WITH([zlibng],
                [AS_HELP_STRING([--with-zlibng=DIR],
                                [Search for zlib-ng headers and libraries in DIR ])])
    AC_ARG_WITH([zlibng-libdir],
                [AS_HELP_STRING([--with-zlibng-libdir=DIR],
                                [Search for zlib-ng libraries in DIR ])])

    pmix_zlibng_support=0

    OAC_CHECK_PACKAGE([zlibng],
                      [pcompress_zlibng],
                      [zlib-ng.h],
                      [z-ng],
                      [zng_deflate],
                      [pmix_zlibng_support=1],
                      [pmix_zlibng_support=0])

    if test ! -z "$with_zlibng" && test "$with_zlibng" != "no" && test "$pmix_zlibng_support" != "1"; then
        AC_MSG_WARN([ZLIB-NG SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    AC_MSG_CHECKING([will zlib-ng support be built])
    AS_IF([test "$pmix_zlibng_support" = "1"],
          [$1
           AC_MSG_RESULT([yes])],
          [$2
           AC_MSG_RESULT([no])])

    PMIX_SUMMARY_ADD([External Packages], [ZLIBNG], [], [${pcompress_zlibng_SUMMARY}])

    # substitute in the things needed to build pcompress/zlibng
    AC_SUBST([pcompress_zlibng_CPPFLAGS])
    AC_SUBST([pcompress_zlibng_LDFLAGS])
    AC_SUBST([pcompress_zlibng_LIBS])

    PMIX_EMBEDDED_LIBS="$PMIX_EMBEDDED_LIBS $pcompress_zlibng_LIBS"
    PMIX_EMBEDDED_LDFLAGS="$PMIX_EMBEDDED_LDFLAGS $pcompress_zlibng_LDFLAGS"
    PMIX_EMBEDDED_CPPFLAGS="$PMIX_EMBEDDED_CPPFLAGS $pcompress_zlibng_CPPFLAGS"

])dnl
