dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2018 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2008-2018 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
dnl Copyright (c) 2015-2018 Research Organization for Information Science
dnl                         and Technology (RIST).  All rights reserved.
dnl Copyright (c) 2014-2018 Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017-2021 Amazon.com, Inc. or its affiliates.  All Rights
dnl                         reserved.
dnl Copyright (c) 2020      Google, LLC. All rights reserved.
dnl Copyright (c) 2020      Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl


AC_DEFUN([PMIX_CHECK_GCC_ATOMIC_BUILTINS], [
  if test -z "$pmix_cv_have___atomic" ; then
    AC_MSG_CHECKING([for 32-bit GCC built-in atomics])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <stdint.h>
]],[[
uint32_t tmp, old = 0;
uint64_t tmp64, old64 = 0;
__atomic_thread_fence(__ATOMIC_SEQ_CST);
__atomic_compare_exchange_n(&tmp, &old, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
__atomic_add_fetch(&tmp, 1, __ATOMIC_RELAXED);
__atomic_compare_exchange_n(&tmp64, &old64, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
__atomic_add_fetch(&tmp64, 1, __ATOMIC_RELAXED);]])],
        [pmix_cv_have___atomic=yes],
        [pmix_cv_have___atomic=no])
    AC_MSG_RESULT([$pmix_cv_have___atomic])

    if test "$pmix_cv_have___atomic" = "yes" ; then
      AC_MSG_CHECKING([for 64-bit GCC built-in atomics])
      AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <stdint.h>
]],[[
uint64_t tmp64, old64 = 0;
__atomic_compare_exchange_n(&tmp64, &old64, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
__atomic_add_fetch(&tmp64, 1, __ATOMIC_RELAXED);]])],
            [pmix_cv_have___atomic_64=yes],
            [pmix_cv_have___atomic_64=no])
      AC_MSG_RESULT([$pmix_cv_have___atomic_64])
      if test "$pmix_cv_have___atomic_64" = "yes" ; then
        AC_MSG_CHECKING([if 64-bit GCC built-in atomics are lock-free])
        AC_RUN_IFELSE([AC_LANG_PROGRAM([], [if (!__atomic_is_lock_free (8, 0)) { return 1; }])],
              [AC_MSG_RESULT([yes])],
              [AC_MSG_RESULT([no])
               pmix_cv_have___atomic_64=no],
              [AC_MSG_RESULT([cannot test -- assume yes (cross compiling)])])
      fi
    else
        pmix_cv_have___atomic_64=no
    fi
  fi
])


dnl #################################################################
dnl
dnl PMIX_CONFIG_ASM()
dnl
dnl DEFINE PMIX_ASSEMBLY_ARCH to something in sys/architecture.h
dnl
dnl #################################################################
AC_DEFUN([PMIX_CONFIG_ASM],[
    AC_REQUIRE([PMIX_SETUP_CC])

    AC_ARG_ENABLE([c11-atomics],[AS_HELP_STRING([--enable-c11-atomics],
                  [Enable use of C11 atomics if available (default: enabled)])])
    AC_ARG_ENABLE([builtin-atomics],
      [AS_HELP_STRING([--enable-builtin-atomics],
         [Enable use of GCC built-in atomics (default: autodetect)])])

    pmix_atomic_c11=0
    pmix_atomic_gcc_builtin=0

    PMIX_CHECK_GCC_ATOMIC_BUILTINS
    if test "x$enable_c11_atomics" != "xno" && test "$pmix_cv_c11_supported" = "yes" ; then
        pmix_atomic_style="c11"
        pmix_atomic_c11=1
    elif test "x$enable_c11_atomics" = "xyes"; then
        AC_MSG_WARN([C11 atomics were requested but are not supported])
        AC_MSG_ERROR([Cannot continue])
    elif test "$enable_builtin_atomics" != "no" && test "$pmix_cv_have___atomic" = "yes" ; then
        pmix_atomic_style="gcc"
        pmix_atomic_gcc_builtin=1
    elif test "$enable_builtin_atomics" = "yes" ; then
        AC_MSG_WARN([GCC built-in atomics requested but not found.])
        AC_MSG_ERROR([Cannot continue])
    fi

    AC_MSG_CHECKING([for x86_64 architecture])
    case "${host}" in
    x86_64-*x32|i?86-*|x86_64*|amd64*)
        AC_MSG_RESULT([yes])
        AC_DEFINE([PMIX_ATOMIC_X86_64], [1], [whether building on x86_64 platform])
        ;;
    *)
        AC_MSG_RESULT([no])
        ;;
    esac

    AC_MSG_CHECKING([for atomics style])
    AC_MSG_RESULT([$pmix_atomic_style])

    AC_DEFINE_UNQUOTED([PMIX_ATOMIC_C11], [$pmix_atomic_c11],
        [Use C11 style atomics])

    AC_DEFINE_UNQUOTED([PMIX_ATOMIC_GCC_BUILTIN], [$pmix_atomic_gcc_builtin],
        [Use GCC builtin style atomics])
])dnl
