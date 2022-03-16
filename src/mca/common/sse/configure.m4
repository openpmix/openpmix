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

    common_sse_CPPFLAGS="${pmix_check_curl_CPPFLAGS} ${pmix_check_jansson_CPPFLAGS}"
    common_sse_LDFLAGS="${pmix_check_curl_LDFLAGS} ${pmix_check_jansson_LDFLAGS}"
    common_sse_STATIC_LDFLAGS="${pmix_check_curl_STATIC_LDFLAGS} ${pmix_check_jansson_STATIC_LDFLAGS}"
    common_sse_LIBS="${pmix_check_curl_LIBS} ${pmix_check_jansson_LIBS}"
    common_sse_STATIC_LIBS="${pmix_check_curl_STATIC_LIBS} ${pmix_check_jansson_STATIC_LIBS}"
    common_sse_PC_MODULES="${pmix_check_curl_PC_MODULES} ${pmix_check_jansson_PC_MODULES}"

    AC_SUBST([common_sse_CPPFLAGS])
    AC_SUBST([common_sse_LDFLAGS])
    AC_SUBST([common_sse_LIBS])
])dnl
