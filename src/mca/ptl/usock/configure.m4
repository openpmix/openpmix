# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Every process needs its local transport, so it is part of the library,
# never a plugin
AC_DEFUN([MCA_pmix_ptl_usock_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $1:$2 compile mode])
    $3="static"
    AC_MSG_RESULT([$$3])
])

AC_DEFUN([MCA_pmix_ptl_usock_CONFIG], [
    AC_CONFIG_FILES([src/mca/ptl/usock/Makefile])
    dnl This component is always available.
    $1
])dnl
