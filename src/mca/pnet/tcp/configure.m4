# -*- shell-script -*-
#
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

    AS_IF([test "yes" = "no"],
          [$1
           pmix_pnet_tcp_happy=yes],
          [$2
           pmix_pnet_tcp_happy=no])

    PMIX_SUMMARY_ADD([[Transports]],[[TCP]],[[pnet_tcp]],[$pmix_pnet_tcp_happy])

])dnl
