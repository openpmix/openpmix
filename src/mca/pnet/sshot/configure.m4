# -*- shell-script -*-
#
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pnet_sshot_CONFIG([action-if-can-compile],
#                            [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pnet_sshot_CONFIG], [
    AC_CONFIG_FILES([src/mca/pnet/sshot/Makefile])

    AC_ARG_WITH([slingshot], [AS_HELP_STRING([--with-slingshot], [Include Slingshot fabric support])],
                [pmix_want_sshot=yes], [pmix_want_sshot=no])

    AC_ARG_WITH([cxi], [AS_HELP_STRING([--with-cxi(=DIR)],
                                       [Include CXI service library support, optionally adding DIR/include, DIR/include/cxi, DIR/lib, and DIR/lib64 to the search path for headers and libraries])],
                [], [with_cxi=no])

    AC_ARG_WITH([cxi-libdir],
                [AS_HELP_STRING([--with-cxi-libdir=DIR],
                                [Search for CXI libraries in DIR])])

    AS_IF([test "$with_cxi" = "no"],
          [AC_MSG_RESULT([no])
           pmix_check_cxi_happy=no],
          [AC_MSG_RESULT([yes])
           PMIX_CHECK_WITHDIR([cxi-libdir], [$with_cxi_libdir], [libcxi.*])
           AS_IF([test ! -z "$with_cxi" && test "$with_cxi" != "yes"],
                 [pmix_check_cxi_dir="$with_cxi"
                  AS_IF([test ! -d "$pmix_check_cxi_dir" || test ! -f "$pmix_check_cxi_dir/cxi.h"],
                         [$pmix_check_cxi_dir=$pmix_check_cxi_dir/include
                          AS_IF([test ! -d "$pmix_check_cxi_dir" || test ! -f "$pmix_check_cxi_dir/cxi.h"],
                                [$pmix_check_cxi_dir=$pmix_check_cxi_dir/cxi
                                 AS_IF([test ! -d "$pmix_check_cxi_dir" || test ! -f "$pmix_check_cxi_dir/cxi.h"],
                                       [AC_MSG_WARN([CXI library support requested, but])
                                        AC_MSG_WARN([required header file cxi.h not found. Locations tested:])
                                        AC_MSG_WARN([    $with_cxi])
                                        AC_MSG_WARN([    $with_cxi/include])
                                        AC_MSG_WARN([    $with_cxi/include/cxi])
                                        AC_MSG_ERROR([Cannot continue])])])])],
                 [pmix_check_cxi_dir="/usr/include/cxi"])

           AS_IF([test ! -z "$with_cxi_libdir" && test "$with_cxi_libdir" != "yes"],
                 [pmix_check_cxi_libdir="$with_cxi_libdir"])

           PMIX_CHECK_PACKAGE([pnet_cxi],
                              [cxi.h],
                              [cxi],
                              [CXI_FUNCTION],
                              [],
                              [$pmix_check_cxi_dir],
                              [$pmix_check_cxi_libdir],
                              [pmix_check_cxi_happy="yes"
                               pnet_cxi_CFLAGS="$pnet_cxi_CFLAGS $pnet_cxi_CFLAGS"
                               pnet_cxi_CPPFLAGS="$pnet_cxi_CPPFLAGS $pnet_cxi_CPPFLAGS"
                               pnet_cxi_LDFLAGS="$pnet_cxi_LDFLAGS $pnet_cxi_LDFLAGS"
                               pnet_cxi_LIBS="$pnet_cxi_LIBS $pnet_cxi_LIBS"],
                              [pmix_check_cxi_happy="no"])
           ])

    # for NOW, hardwire cxi support to be happy
    pmix_check_cxi_happy=yes

    AS_IF([test "$pmix_want_sshot" = "yes" && test "$pmix_common_sse_happy" = "yes" && test "$pmix_check_cxi_happy" = "yes"],
          [$1
           pnet_sshot_happy=yes],
          [$2
           pnet_sshot_happy=no])

    PMIX_SUMMARY_ADD([[Transports]],[[HPE Slingshot]],[[pnet_sshot]],[$pnet_sshot_happy])

])dnl
