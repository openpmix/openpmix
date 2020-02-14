# -*- shell-script -*-
#
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
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

    AS_IF([test "$pmix_check_jansson_happy" = "yes" && test "$pmix_check_curl_happy" = "yes"],
          [$1], [$2])

])dnl
