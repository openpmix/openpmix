# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2020      Amazon.com, Inc. or its affiliates.  All Rights
#                         reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

#
# We have two modes for building hwloc.
#
# First is as a co-built hwloc.  In this case, PMIx's CPPFLAGS
# will be set before configure to include the right -Is to pick up
# hwloc headers and LIBS will point to where the .la file for
# hwloc will exist.  When co-building, hwloc's configure will be
# run already, but the library will not yet be built.  It is ok to run
# any compile-time (not link-time) tests in this mode.  This mode is
# used when the --with-hwloc=cobuild option is specified.
#
# Second is an external package.  In this case, all compile and link
# time tests can be run.  This macro must do any CPPFLAGS/LDFLAGS/LIBS
# modifications it desires in order to compile and link against
# hwloc.  This mode is used whenever the other modes are not used.
#
# MCA_ploc_hwloc_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([MCA_pmix_ploc_hwloc_CONFIG],[
    AC_CONFIG_FILES([src/mca/ploc/hwloc/Makefile])

    PMIX_VAR_SCOPE_PUSH([hwloc_build_mode])

    AC_ARG_WITH([hwloc],
                [AC_HELP_STRING([--with-hwloc=DIR],
                                [Search for hwloc headers and libraries in DIR ])])

    AC_ARG_WITH([hwloc-libdir],
                [AC_HELP_STRING([--with-hwloc-libdir=DIR],
                                [Search for hwloc libraries in DIR ])])

    pmix_hwloc_support=0
    pmix_hwloc_source=""
    pmix_hwloc_support_will_build=no
    pmix_have_topology_dup=0

    # figure out our mode...
    AS_IF([test "$with_hwloc" = "cobuild"],
          [_PMIX_HWLOC_EMBEDDED_MODE],
          [_PMIX_HWLOC_EXTERNAL])

    AS_IF([test $pmix_hwloc_support -eq 1],
          [AC_MSG_CHECKING([hwloc header])
           AC_MSG_RESULT([$PMIX_HWLOC_HEADER])])

    AC_DEFINE_UNQUOTED([PMIX_HWLOC_HEADER], [$PMIX_HWLOC_HEADER],
                   [Location of hwloc.h])
    AC_DEFINE_UNQUOTED([PMIX_HAVE_HWLOC], [$pmix_hwloc_support],
                   [Whether or not we have hwloc support])
    AM_CONDITIONAL([PMIX_HAVE_HWLOC], [test $pmix_hwloc_support -eq 1])
    AC_DEFINE_UNQUOTED([PMIX_HAVE_HWLOC_TOPOLOGY_DUP], [$pmix_have_topology_dup],
                   [Whether or not we have hwloc_topology_dup support])

    PMIX_SUMMARY_ADD([[External Packages]],[[HWLOC]], [pmix_hwloc], [$pmix_hwloc_support_will_build ($pmix_hwloc_source)])

    AS_IF([test $pmix_hwloc_support -eq 1],
          [$1], [$2])
])

AC_DEFUN([_PMIX_HWLOC_EMBEDDED_MODE],[
    AC_MSG_CHECKING([for hwloc])
    AC_MSG_RESULT([$1])

    AS_IF([test -z "$with_hwloc_header" || test "$with_hwloc_header" = "yes"],
          [PMIX_HWLOC_HEADER="<hwloc.h>"],
          [PMIX_HWLOC_HEADER="$with_hwloc_header"])

    AC_MSG_CHECKING([if external hwloc version is 1.5 or greater])
    AC_COMPILE_IFELSE(
              [AC_LANG_PROGRAM([[#include <hwloc.h>]],
              [[
    #if HWLOC_API_VERSION < 0x00010500
    #error "hwloc API version is less than 0x00010500"
    #endif
              ]])],
              [AC_MSG_RESULT([yes])],
              [AC_MSG_RESULT([no])
               AC_MSG_ERROR([Cannot continue])])

    AC_MSG_CHECKING([if external hwloc version is 1.8 or greater])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([[#include <hwloc.h>]],
          [[
    #if HWLOC_API_VERSION < 0x00010800
    #error "hwloc API version is less than 0x00010800"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])
           pmix_have_topology_dup=1],
          [AC_MSG_RESULT([no])])

    pmix_hwloc_support=1
    pmix_hwloc_source=cobuild
    pmix_hwloc_support_will_build=yes
])

AC_DEFUN([_PMIX_HWLOC_EXTERNAL],[
    PMIX_VAR_SCOPE_PUSH([pmix_hwloc_dir pmix_hwloc_libdir pmix_hwloc_standard_lib_location pmix_hwloc_standard_header_location pmix_check_hwloc_save_CPPFLAGS pmix_check_hwloc_save_LDFLAGS pmix_check_hwloc_save_LIBS])

    pmix_hwloc_support=0
    pmix_hwloc_have_dup=0
    pmix_check_hwloc_save_CPPFLAGS="$CPPFLAGS"
    pmix_check_hwloc_save_LDFLAGS="$LDFLAGS"
    pmix_check_hwloc_save_LIBS="$LIBS"
    pmix_hwloc_standard_header_location=yes
    pmix_hwloc_standard_lib_location=yes

    AS_IF([test "$with_hwloc" = "internal" || test "$with_hwloc" = "external"],
          [with_hwloc=])

    if test "$with_hwloc" != "no"; then
        AC_MSG_CHECKING([for hwloc in])
        if test ! -z "$with_hwloc" && test "$with_hwloc" != "yes"; then
            pmix_hwloc_dir=$with_hwloc/include
            pmix_hwloc_standard_header_location=no
            pmix_hwloc_standard_lib_location=no
            AS_IF([test -z "$with_hwloc_libdir" || test "$with_hwloc_libdir" = "yes"],
                  [if test -d $with_hwloc/lib; then
                       pmix_hwloc_libdir=$with_hwloc/lib
                   elif test -d $with_hwloc/lib64; then
                       pmix_hwloc_libdir=$with_hwloc/lib64
                   else
                       AC_MSG_RESULT([Could not find $with_hwloc/lib or $with_hwloc/lib64])
                       AC_MSG_ERROR([Can not continue])
                   fi
                   AC_MSG_RESULT([$pmix_hwloc_dir and $pmix_hwloc_libdir])],
                  [AC_MSG_RESULT([$with_hwloc_libdir])])
        else
            pmix_hwloc_dir=/usr/include
            if test -d /usr/lib64; then
                pmix_hwloc_libdir=/usr/lib64
            elif test -d /usr/lib; then
                pmix_hwloc_libdir=/usr/lib
            else
                AC_MSG_RESULT([not found])
                AC_MSG_WARN([Could not find /usr/lib or /usr/lib64 - you may])
                AC_MSG_WARN([need to specify --with-hwloc_libdir=<path>])
                AC_MSG_ERROR([Can not continue])
            fi
            AC_MSG_RESULT([(default search paths)])
            pmix_hwloc_standard_header_location=yes
            pmix_hwloc_standard_lib_location=yes
        fi
        AS_IF([test ! -z "$with_hwloc_libdir" && test "$with_hwloc_libdir" != "yes"],
              [pmix_hwloc_libdir="$with_hwloc_libdir"
               pmix_hwloc_standard_lib_location=no])

        PMIX_CHECK_PACKAGE([pmix_hwloc],
                           [hwloc.h],
                           [hwloc],
                           [hwloc_topology_init],
                           [-lhwloc],
                           [$pmix_hwloc_dir],
                           [$pmix_hwloc_libdir],
                           [pmix_hwloc_support=1],
                           [pmix_hwloc_support=0])

        AS_IF([test "$pmix_hwloc_standard_header_location" != "yes"],
              [PMIX_FLAGS_APPEND_UNIQ(CPPFLAGS, $pmix_hwloc_CPPFLAGS)])

        AS_IF([test "$pmix_hwloc_standard_lib_location" != "yes"],
              [PMIX_FLAGS_APPEND_UNIQ(LDFLAGS, $pmix_hwloc_LDFLAGS)])
        PMIX_FLAGS_APPEND_UNIQ(LIBS, $pmix_hwloc_LIBS)
    fi

    if test ! -z "$with_hwloc" && test "$with_hwloc" != "no" && test "$pmix_hwloc_support" != "1"; then
        AC_MSG_WARN([HWLOC SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    if test $pmix_hwloc_support = "1"; then
        AC_MSG_CHECKING([if external hwloc version is 1.5 or greater])
        AC_COMPILE_IFELSE(
              [AC_LANG_PROGRAM([[#include <hwloc.h>]],
              [[
    #if HWLOC_API_VERSION < 0x00010500
    #error "hwloc API version is less than 0x00010500"
    #endif
              ]])],
              [AC_MSG_RESULT([yes])],
              [AC_MSG_RESULT([no])
               AC_MSG_ERROR([Cannot continue])])

        AC_MSG_CHECKING([if external hwloc version is 1.8 or greater])
        AC_COMPILE_IFELSE(
              [AC_LANG_PROGRAM([[#include <hwloc.h>]],
              [[
    #if HWLOC_API_VERSION < 0x00010800
    #error "hwloc API version is less than 0x00010800"
    #endif
              ]])],
              [AC_MSG_RESULT([yes])
               pmix_have_topology_dup=1],
              [AC_MSG_RESULT([no])])
    fi

    CPPFLAGS=$pmix_check_hwloc_save_CPPFLAGS
    LDFLAGS=$pmix_check_hwloc_save_LDFLAGS
    LIBS=$pmix_check_hwloc_save_LIBS

    AC_MSG_CHECKING([will hwloc support be built])
    if test "$pmix_hwloc_support" != "1"; then
        AC_MSG_RESULT([no])
        pmix_hwloc_source=none
        pmix_hwloc_support_will_build=no
    else
        AC_MSG_RESULT([yes])
        pmix_hwloc_source=$pmix_hwloc_dir
        pmix_hwloc_support_will_build=yes
        AS_IF([test "$pmix_hwloc_standard_header_location" != "yes"],
              [PMIX_FLAGS_APPEND_UNIQ(PMIX_FINAL_CPPFLAGS, $pmix_hwloc_CPPFLAGS)
               PMIX_WRAPPER_FLAGS_ADD(CPPFLAGS, $pmix_hwloc_CPPFLAGS)])

        AS_IF([test "$pmix_hwloc_standard_lib_location" != "yes"],
              [PMIX_FLAGS_APPEND_UNIQ(PMIX_FINAL_LDFLAGS, $pmix_hwloc_LDFLAGS)
               PMIX_WRAPPER_FLAGS_ADD(LDFLAGS, $pmix_hwloc_LDFLAGS)])
        PMIX_FLAGS_APPEND_UNIQ(PMIX_FINAL_LIBS, $pmix_hwloc_LIBS)
        PMIX_WRAPPER_FLAGS_ADD(LIBS, $pmix_hwloc_LIBS)
    fi

    # Set output variables
    PMIX_HWLOC_HEADER="<hwloc.h>"

    PMIX_VAR_SCOPE_POP
])dnl
