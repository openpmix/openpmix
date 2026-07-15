# -*- shell-script -*-
#
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pnet_tcp_CONFIG([action-if-can-compile],
#                          [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pnet_tcp_CONFIG], [
    AC_CONFIG_FILES([src/mca/pnet/tcp/Makefile])

    AC_ARG_WITH([tcp], [AS_HELP_STRING([--with-tcp], [Include TCP/UDP static-port fabric support])],
                [pmix_want_tcp=yes], [pmix_want_tcp=no])

    AS_IF([test "$pmix_want_tcp" = "yes"],
          [$1],
          [$2])

    PMIX_SUMMARY_ADD([Transports], [TCP], [], [$pmix_want_tcp])

])dnl
