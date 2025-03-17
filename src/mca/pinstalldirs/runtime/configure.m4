# -*- shell-script -*-
#
# Copyright (c) 2025      NVIDIA Corporation.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
AC_DEFUN([MCA_pmix_pinstalldirs_runtime_PRIORITY], [5])

AC_DEFUN([MCA_pmix_pinstalldirs_runtime_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $2:$3 compile mode])
    $4="static"
    AC_MSG_RESULT([$$4])
])

# MCA_pinstalldirs_config_CONFIG(action-if-can-compile,
#                        [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pinstalldirs_runtime_CONFIG], [
    # Check if we are building a shared library or not. Disable if static
    AC_MSG_CHECKING([if shared libraries are enabled])
    AS_IF([test "$enable_shared" != "yes"],
          [pinstalldirs_runtime_happy="no"],
          [pinstalldirs_runtime_happy="yes"])
    AC_MSG_RESULT([$pinstalldirs_runtime_happy])

    # Check if dladdr is available
    AS_IF([test "$pinstalldirs_runtime_happy" = "yes"],
          [AC_CHECK_HEADERS([dlfcn.h],
                            [],
                            [pinstalldirs_runtime_happy="no"])])
    AS_IF([test "$pinstalldirs_runtime_happy" = "yes"],
          [AC_CHECK_LIB([dl], [dladdr],
                        [],
                        [pinstalldirs_runtime_happy="no"])
          ])
    #
    AS_IF([test "$pinstalldirs_runtime_happy" = "yes"],
          [AC_CONFIG_FILES([src/mca/pinstalldirs/runtime/Makefile])
           $1], [$2])
])

