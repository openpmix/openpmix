# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_hwloc_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([PMIX_SETUP_HWLOC],[
    PMIX_VAR_SCOPE_PUSH([pmix_hwloc_dir pmix_hwloc_libdir pmix_check_hwloc_save_CPPFLAGS pmix_check_hwloc_save_LDFLAGS pmix_check_hwloc_save_LIBS])

    AC_ARG_WITH([hwloc],
                [AS_HELP_STRING([--with-hwloc=DIR],
                                [Search for hwloc headers and libraries in DIR ])])

    AC_ARG_WITH([hwloc-libdir],
                [AS_HELP_STRING([--with-hwloc-libdir=DIR],
                                [Search for hwloc libraries in DIR ])])

    pmix_hwloc_support=0
    pmix_check_hwloc_save_CPPFLAGS="$CPPFLAGS"
    pmix_check_hwloc_save_LDFLAGS="$LDFLAGS"
    pmix_check_hwloc_save_LIBS="$LIBS"
    pmix_have_topology_dup=0

    if test "$with_hwloc" == "no"; then
        AC_MSG_WARN([PRRTE requires HWLOC topology library support.])
        AC_MSG_WARN([Please reconfigure so we can find the library.])
        AC_MSG_ERROR([Cannot continue.])
    fi

    # get rid of any trailing slash(es)
    hwloc_prefix=$(echo $with_hwloc | sed -e 'sX/*$XXg')
    hwlocdir_prefix=$(echo $with_hwloc_libdir | sed -e 'sX/*$XXg')

    AS_IF([test ! -z "$hwloc_prefix" && test "$hwloc_prefix" != "yes"],
                 [pmix_hwloc_dir="$hwloc_prefix"],
                 [pmix_hwloc_dir=""])

    AS_IF([test ! -z "$hwlocdir_prefix" && test "$hwlocdir_prefix" != "yes"],
                 [pmix_hwloc_libdir="$hwlocdir_prefix"],
                 [AS_IF([test ! -z "$hwloc_prefix" && test "$hwloc_prefix" != "yes"],
                        [if test -d $hwloc_prefix/lib64; then
                            pmix_hwloc_libdir=$hwloc_prefix/lib64
                         elif test -d $hwloc_prefix/lib; then
                            pmix_hwloc_libdir=$hwloc_prefix/lib
                         else
                            AC_MSG_WARN([Could not find $hwloc_prefix/lib or $hwloc_prefix/lib64])
                            AC_MSG_ERROR([Can not continue])
                         fi
                        ],
                        [pmix_hwloc_libdir=""])])

    PMIX_CHECK_PACKAGE([pmix_hwloc],
                       [hwloc.h],
                       [hwloc],
                       [hwloc_topology_init],
                       [],
                       [$pmix_hwloc_dir],
                       [$pmix_hwloc_libdir],
                       [pmix_hwloc_support=1],
                       [pmix_hwloc_support=0],
                       [])

    if test $pmix_hwloc_support -eq 0; then
        AC_MSG_WARN([PMIx requires HWLOC topology library support, but])
        AC_MSG_WARN([an adequate version of that library was not found.])
        AC_MSG_WARN([Please reconfigure and point to a location where])
        AC_MSG_WARN([the HWLOC library can be found.])
        AC_MSG_ERROR([Cannot continue.])
    fi

   # update global flags to test for HWLOC version
    if test ! -z "$pmix_hwloc_CPPFLAGS"; then
        PMIX_FLAGS_APPEND_UNIQ(CPPFLAGS, $pmix_hwloc_CPPFLAGS)
    fi
    if test ! -z "$pmix_hwloc_LDFLAGS"; then
        PMIX_FLAGS_APPEND_UNIQ(LDFLAGS, $pmix_hwloc_LDFLAGS)
    fi
    if test ! -z "$pmix_hwloc_LIBS"; then
        PMIX_FLAGS_APPEND_UNIQ(LIBS, $pmix_hwloc_LIBS)
    fi

    AC_MSG_CHECKING([if hwloc version is 1.5 or greater])
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

    AC_MSG_CHECKING([if hwloc version is 1.8 or greater])
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

    AC_MSG_CHECKING([if hwloc version is 2.0 or greater])
    AC_COMPILE_IFELSE(
          [AC_LANG_PROGRAM([[#include <hwloc.h>]],
          [[
    #if HWLOC_API_VERSION < 0x00020000
    #error "hwloc API version is less than 0x00020000"
    #endif
          ]])],
          [AC_MSG_RESULT([yes])
           pmix_version_high=1],
          [AC_MSG_RESULT([no])
           pmix_version_high=0])

    # set the header
    PMIX_HWLOC_HEADER="<hwloc.h>"

    CPPFLAGS=$pmix_check_hwloc_save_CPPFLAGS
    LDFLAGS=$pmix_check_hwloc_save_LDFLAGS
    LIBS=$pmix_check_hwloc_save_LIBS

    if test ! -z "$pmix_hwloc_CPPFLAGS"; then
        PMIX_FLAGS_APPEND_UNIQ(PMIX_FINAL_CPPFLAGS, $pmix_hwloc_CPPFLAGS)
        PMIX_WRAPPER_FLAGS_ADD(CPPFLAGS, $pmix_hwloc_CPPFLAGS)
    fi
    if test ! -z "$pmix_hwloc_LDFLAGS"; then
        PMIX_FLAGS_APPEND_UNIQ(PMIX_FINAL_LDFLAGS, $pmix_hwloc_LDFLAGS)
        PMIX_WRAPPER_FLAGS_ADD(LDFLAGS, $pmix_hwloc_LDFLAGS)
    fi
    if test ! -z "$pmix_hwloc_LIBS"; then
        PMIX_FLAGS_APPEND_UNIQ(PMIX_FINAL_LIBS, $pmix_hwloc_LIBS)
        PMIX_WRAPPER_FLAGS_ADD(LIBS, $pmix_hwloc_LIBS)
    fi

    AC_MSG_CHECKING([location of hwloc header])
    AC_DEFINE_UNQUOTED([PMIX_HWLOC_HEADER], [$PMIX_HWLOC_HEADER],
                       [Location of hwloc.h])
    AC_MSG_RESULT([$PMIX_HWLOC_HEADER])

    AC_DEFINE_UNQUOTED([PMIX_HAVE_HWLOC_TOPOLOGY_DUP], [$pmix_have_topology_dup],
                       [Whether or not hwloc_topology_dup is available])

    AM_CONDITIONAL([PMIX_HWLOC_VERSION_HIGH], [test $pmix_version_high -eq 1])

    pmix_hwloc_support_will_build=yes
    if test -z "$pmix_hwloc_dir"; then
        pmix_hwloc_source="Standard locations"
    else
        pmix_hwloc_source=$pmix_hwloc_dir
    fi

    PMIX_SUMMARY_ADD([[Required Packages]],[[HWLOC]], [pmix_hwloc], [$pmix_hwloc_support_will_build ($pmix_hwloc_source)])

    PMIX_VAR_SCOPE_POP
])
