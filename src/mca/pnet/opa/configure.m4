# -*- shell-script -*-
#
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pnet_opa_CONFIG([action-if-can-compile],
#                          [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pnet_opa_CONFIG], [
    AC_CONFIG_FILES([src/mca/pnet/opa/Makefile])

    AS_IF([test "yes" = "yes"],
          [$1
           pmix_pnet_opa=yes],
          [$2
           pmix_pnet_opa=no])

    PMIX_SUMMARY_ADD([[Transports]],[[OmniPath]],[[pnet_opa]],[$pmix_pnet_opa])

])dnl
