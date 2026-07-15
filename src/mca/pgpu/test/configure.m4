# -*- shell-script -*-
#
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pgpu_test_CONFIG([action-if-can-compile],
#                           [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pgpu_test_CONFIG], [
    AC_CONFIG_FILES([src/mca/pgpu/test/Makefile])

    AC_ARG_WITH([pgpu-test],
                [AS_HELP_STRING([--with-pgpu-test],
                                [Build the pgpu test component used to exercise the GPU setup path (default: no)])],
                [pmix_want_pgpu_test=yes], [pmix_want_pgpu_test=no])

    AS_IF([test "$pmix_want_pgpu_test" = "yes"],
          [$1],
          [$2])

    PMIX_SUMMARY_ADD([GPUs], [Test], [], [$pmix_want_pgpu_test])
])dnl
