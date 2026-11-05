# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Tests reach the transport's helpers by name - so, when it is built, it
# is part of the library, never a plugin
AC_DEFUN([MCA_pmix_ptl_tcp_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $1:$2 compile mode])
    $3="static"
    AC_MSG_RESULT([$$3])
])

# TCP carries only the connections with peers of earlier releases - a
# process of this release reaches its server on the local transport - so
# it is built only when this PMIx supports such peers in either direction
# (--enable-legacy-clients or --enable-legacy-servers - see config/pmix.m4)
AC_DEFUN([MCA_pmix_ptl_tcp_CONFIG], [
    AC_CONFIG_FILES([src/mca/ptl/tcp/Makefile])
    AS_IF([test "$pmix_legacy_support" = "1"], [$1], [$2])
])dnl
