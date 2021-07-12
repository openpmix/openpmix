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

# MCA_pmix_common_sse_CONFIG([action-if-can-compile],
#                            [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_common_sse_CONFIG], [
    AC_CONFIG_FILES([src/mca/common/sse/Makefile])

    AC_REQUIRE([PMIX_CHECK_CURL])
    AC_REQUIRE([PMIX_CHECK_JANSSON])

    AS_IF([test "$pmix_check_jansson_happy" = "yes" && test "$pmix_check_curl_happy" = "yes"],
          [$1
           pmix_common_sse_happy=yes],
          [$2
           pmix_common_sse_happy=no])

    PMIX_SUMMARY_ADD([[Optional Support]],[[SSE]], [common_sse], [$pmix_check_curl_happy])

])dnl
