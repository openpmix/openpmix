# -*- autoconf -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
# Copyright (c) 2021-2022 Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_hwloc_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PMIX_SETUP_HWLOC],[
    PMIX_VAR_SCOPE_PUSH([pmix_hwloc_dir pmix_hwloc_libdir pmix_check_hwloc_save_CPPFLAGS])

    AC_ARG_WITH([hwloc],
                [AS_HELP_STRING([--with-hwloc=DIR],
                                [Search for hwloc headers and libraries in DIR ])])
    AC_ARG_WITH([hwloc-libdir],
                [AS_HELP_STRING([--with-hwloc-libdir=DIR],
                                [Search for hwloc libraries in DIR ])])
    AC_ARG_WITH([hwloc-extra-libs],
                [AS_HELP_STRING([--with-hwloc-extra-libs=LIBS],
                                [Add LIBS as dependencies of hwloc])])
    AC_ARG_ENABLE([hwloc-lib-checks],
                  [AS_HELP_STRING([--disable-hwloc-lib-checks],
                                  [If --disable-hwloc-lib-checks is specified, configure will assume that -lhwloc is available])])

    pmix_hwloc_support=1
    pmix_check_hwloc_save_CPPFLAGS="$CPPFLAGS"
    pmix_have_topology_dup=0

    if test "$with_hwloc" = "no"; then
        AC_MSG_WARN([PMIx requires HWLOC topology library support.])
        AC_MSG_WARN([Please reconfigure so we can find the library.])
        AC_MSG_ERROR([Cannot continue.])
    fi

    AS_IF([test "$with_hwloc_extra_libs" = "yes" -o "$with_hwloc_extra_libs" = "no"],
	  [AC_MSG_ERROR([--with-hwloc-extra-libs requires an argument other than yes or no])])

    AS_IF([test "$enable_hwloc_lib_checks" != "no"],
          [OAC_CHECK_PACKAGE([hwloc],
                             [pmix_hwloc],
                             [hwloc.h],
                             [hwloc $with_hwloc_extra_libs],
                             [hwloc_topology_init],
                             [],
                             [pmix_hwloc_support=0])],
          [PMIX_FLAGS_APPEND_UNIQ([PMIX_DELAYED_LIBS], [$with_hwloc_extra_libs])])

    if test $pmix_hwloc_support -eq 0; then
        AC_MSG_WARN([PMIx requires HWLOC topology library support, but])
        AC_MSG_WARN([an adequate version of that library was not found.])
        AC_MSG_WARN([Please reconfigure and point to a location where])
        AC_MSG_WARN([the HWLOC library can be found.])
        AC_MSG_ERROR([Cannot continue.])
    fi

    # update global flags to test for HWLOC version
    PMIX_FLAGS_PREPEND_UNIQ([CPPFLAGS], [$pmix_hwloc_CPPFLAGS])

    AC_MSG_CHECKING([if hwloc version is 1.5 or greater])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([#include <hwloc.h>],
          [[
    #if HWLOC_API_VERSION < 0x00010500
    #error "hwloc version is less than 0x00010500"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])],
          [AC_MSG_RESULT([no])
           AC_MSG_ERROR([Cannot continue])])

    AC_MSG_CHECKING([if hwloc version is 1.8 or greater])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([#include <hwloc.h>],
          [[
    #if HWLOC_API_VERSION < 0x00010800
    #error "hwloc version is less than 0x00010800"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])
           pmix_have_topology_dup=1],
          [AC_MSG_RESULT([no])])

    AC_MSG_CHECKING([if hwloc version is at least 2.0])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([#include <hwloc.h>],
          [[
    #if HWLOC_VERSION_MAJOR < 2
    #error "hwloc version is less than 2.0"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])
           pmix_version_high=1],
          [AC_MSG_RESULT([no])
           pmix_version_high=0])

    AC_MSG_CHECKING([if hwloc version is greater than 2.x])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([#include <hwloc.h>],
          [[
    #if HWLOC_VERSION_MAJOR > 2
    #error "hwloc version is greater than 2.x"
    #endif
          ]])],
          [AC_MSG_RESULT([no])],
          [AC_MSG_RESULT([yes])
           AC_MSG_WARN([This PMIx version does not support HWLOC])
           AC_MSG_WARN([versions 3.x or higher. Please direct us])
           AC_MSG_WARN([to an HWLOC version in the 1.11-2.x range.])
           AC_MSG_ERROR([Cannot continue])])

    CPPFLAGS=$pmix_check_hwloc_save_CPPFLAGS

    PMIX_FLAGS_APPEND_UNIQ([CPPFLAGS], [$pmix_hwloc_CPPFLAGS])
    PMIX_WRAPPER_FLAGS_ADD([CPPFLAGS], [$pmix_hwloc_CPPFLAGS])

    PMIX_FLAGS_APPEND_UNIQ([LDFLAGS], [$pmix_hwloc_LDFLAGS])
    PMIX_WRAPPER_FLAGS_ADD([LDFLAGS], [$pmix_hwloc_LDFLAGS])
    PMIX_WRAPPER_FLAGS_ADD([STATIC_LDFLAGS], [$pmix_hwloc_STATIC_LDFLAGS])

    PMIX_FLAGS_APPEND_UNIQ([PMIX_DELAYED_LIBS], [$pmix_hwloc_LIBS])
    PMIX_WRAPPER_FLAGS_ADD([LIBS], [$pmix_hwloc_LIBS])
    PMIX_WRAPPER_FLAGS_ADD([STATIC_LIBS], [$pmix_hwloc_STATIC_LIBS])

    PMIX_WRAPPER_FLAGS_ADD([PC_MODULES], [$pmix_hwloc_PC_MODULES])

    AC_DEFINE_UNQUOTED([PMIX_HAVE_HWLOC_TOPOLOGY_DUP], [$pmix_have_topology_dup],
                       [Whether or not hwloc_topology_dup is available])

    AM_CONDITIONAL([PMIX_HWLOC_VERSION_HIGH], [test $pmix_version_high -eq 1])

    PMIX_SUMMARY_ADD([Required Packages], [HWLOC], [], [$pmix_hwloc_SUMMARY])

    PMIX_VAR_SCOPE_POP
])
