# -*- shell-script -*-
#
# Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2020      IBM Corporation.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PMIX_LIBEVENT_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
# Attempt to find a libevent package.  If found, evaluate
# action-if-found.  Otherwise, evaluate action-if-not-found.
#
# Modifies the following in the environment:
#  * pmix_libevent_CPPFLAGS
#  * pmix_libevent_LDFLAGS
#  * pmix_libevent_LIBS
#
# Adds the following to the wrapper compilers:
#  * CPPFLAGS: none
#  * LDLFGAS: add pmix_libevent_LDFLAGS
#  * LIBS: add pmix_libevent_LIBS
AC_DEFUN([PMIX_LIBEVENT_CONFIG],[
    PMIX_VAR_SCOPE_PUSH([pmix_event_dir pmix_event_libdir pmix_check_libevent_save_CPPFLAGS pmix_check_libevent_save_LDFLAGS pmix_check_libevent_save_LIBS])

    AC_ARG_WITH([libevent],
                [AS_HELP_STRING([--with-libevent=DIR],
                                [Search for libevent headers and libraries in DIR ])])

    AC_ARG_WITH([libevent-libdir],
                [AS_HELP_STRING([--with-libevent-libdir=DIR],
                                [Search for libevent libraries in DIR ])])

    pmix_libevent_support=1

    AS_IF([test "$with_libevent" = "no"],
          [AC_MSG_NOTICE([Libevent support disabled by user.])
           pmix_libevent_support=0])

    AS_IF([test $pmix_libevent_support -eq 1],
          [PMIX_CHECK_WITHDIR([libevent], [$with_libevent], [include/event.h])
           PMIX_CHECK_WITHDIR([libevent-libdir], [$with_libevent_libdir], [libevent.*])

           pmix_check_libevent_save_CPPFLAGS="$CPPFLAGS"
           pmix_check_libevent_save_LDFLAGS="$LDFLAGS"
           pmix_check_libevent_save_LIBS="$LIBS"

           # get rid of any trailing slash(es)
           libevent_prefix=$(echo $with_libevent | sed -e 'sX/*$XXg')
           libeventdir_prefix=$(echo $with_libevent_libdir | sed -e 'sX/*$XXg')

           AS_IF([test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"],
                 [pmix_event_dir="$libevent_prefix"],
                 [pmix_event_dir=""])

           AS_IF([test ! -z "$libeventdir_prefix" -a "$libeventdir_prefix" != "yes"],
                 [pmix_event_libdir="$libeventdir_prefix"],
                 [AS_IF([test ! -z "$libevent_prefix" && test "$libevent_prefix" != "yes"],
                        [if test -d $libevent_prefix/lib64; then
                            pmix_event_libdir=$libevent_prefix/lib64
                         elif test -d $libevent_prefix/lib; then
                            pmix_event_libdir=$libevent_prefix/lib
                         else
                            AC_MSG_WARN([Could not find $libevent_prefix/lib or $libevent_prefix/lib64])
                            AC_MSG_ERROR([Can not continue])
                         fi
                        ],
                        [pmix_event_libdir=""])])

           PMIX_CHECK_PACKAGE([pmix_libevent],
                              [event.h],
                              [event_core],
                              [event_config_new],
                              [-levent_pthreads],
                              [$pmix_event_dir],
                              [$pmix_event_libdir],
                              [],
                              [pmix_libevent_support=0],
                              [])])

    # Check to see if the above check failed because it conflicted with LSF's libevent.so
    # This can happen if LSF's library is in the LDFLAGS envar or default search
    # path. The 'event_getcode4name' function is only defined in LSF's libevent.so and not
    # in Libevent's libevent.so
    if test $pmix_libevent_support -eq 0; then
        AC_CHECK_LIB([event], [event_getcode4name],
                     [AC_MSG_WARN([===================================================================])
                      AC_MSG_WARN([Possible conflicting libevent.so libraries detected on the system.])
                      AC_MSG_WARN([])
                      AC_MSG_WARN([LSF provides a libevent.so that is not from Libevent in its])
                      AC_MSG_WARN([library path. It is possible that you have installed Libevent])
                      AC_MSG_WARN([on the system, but the linker is picking up the wrong version.])
                      AC_MSG_WARN([])
                      AC_MSG_WARN([You will need to address this linker path issue. One way to do so is])
                      AC_MSG_WARN([to make sure the libevent system library path occurs before the])
                      AC_MSG_WARN([LSF library path.])
                      AC_MSG_WARN([===================================================================])
                      ])
    fi

    if test $pmix_libevent_support -eq 1; then
        # need to add resulting flags to global ones so we can
        # test for thread support
        PMIX_FLAGS_PREPEND_UNIQ([CPPFLAGS], [$pmix_libevent_CPPFLAGS])
        PMIX_FLAGS_PREPEND_UNIQ([LDFLAGS], [$pmix_libevent_LDFLAGS])
        PMIX_FLAGS_PREPEND_UNIQ([LIBS], [$pmix_libevent_LIBS])

        # Check for general threading support
        AC_MSG_CHECKING([if libevent threads enabled])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
#include <event.h>
#include <event2/thread.h>
          ], [[
#if !(EVTHREAD_LOCK_API_VERSION >= 1)
#  error "No threads!"
#endif
          ]])],
          [AC_MSG_RESULT([yes])],
          [AC_MSG_RESULT([no])
           AC_MSG_WARN([PMIX rquires libevent to be compiled with thread support enabled])
           pmix_libevent_support=0])
    fi

    if test $pmix_libevent_support -eq 1; then
        AC_MSG_CHECKING([for libevent pthreads support])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
#include <event.h>
#include <event2/thread.h>
          ], [[
#if !defined(EVTHREAD_USE_PTHREADS_IMPLEMENTED) || !EVTHREAD_USE_PTHREADS_IMPLEMENTED
#  error "No pthreads!"
#endif
          ]])],
          [AC_MSG_RESULT([yes])],
          [AC_MSG_RESULT([no])
           AC_MSG_WARN([PMIX requires libevent to be compiled with pthread support enabled])
           pmix_libevent_support=0])
    fi

    if test $pmix_libevent_support -eq 1; then
        # Pin the "oldest supported" version to 2.0.21
        AC_MSG_CHECKING([if libevent version is 2.0.21 or greater])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <event2/event.h>]],
                                           [[
                                             #if defined(_EVENT_NUMERIC_VERSION) && _EVENT_NUMERIC_VERSION < 0x02001500
                                             #error "libevent API version is less than 0x02001500"
                                             #elif defined(EVENT__NUMERIC_VERSION) && EVENT__NUMERIC_VERSION < 0x02001500
                                             #error "libevent API version is less than 0x02001500"
                                             #endif
                                           ]])],
                          [AC_MSG_RESULT([yes])],
                          [AC_MSG_RESULT([no])
                           AC_MSG_WARN([libevent version is too old (2.0.21 or later required)])
                           pmix_libevent_support=0])
    fi
    if test -z "$pmix_event_dir"; then
        pmix_libevent_source="Standard locations"
    else
        pmix_libevent_source=$pmix_event_dir
    fi

    # restore global flags
    CPPFLAGS="$pmix_check_libevent_save_CPPFLAGS"
    LDFLAGS="$pmix_check_libevent_save_LDFLAGS"
    LIBS="$pmix_check_libevent_save_LIBS"

    AC_MSG_CHECKING([will libevent support be built])
    if test $pmix_libevent_support -eq 1; then
        AC_MSG_RESULT([yes])
        PMIX_FLAGS_APPEND_UNIQ([PMIX_FINAL_CPPFLAGS], [$pmix_libevent_CPPFLAGS])
        PMIX_WRAPPER_FLAGS_ADD([CPPFLAGS], [$pmix_libevent_CPPFLAGS])

        PMIX_FLAGS_APPEND_UNIQ([PMIX_FINAL_LDFLAGS], [$pmix_libevent_LDFLAGS])
        PMIX_WRAPPER_FLAGS_ADD([LDFLAGS], [$pmix_libevent_LDFLAGS])

        PMIX_FLAGS_APPEND_UNIQ([PMIX_FINAL_LIBS], [$pmix_libevent_LIBS])
        PMIX_WRAPPER_FLAGS_ADD([LIBS], [$pmix_libevent_LIBS])
        # Set output variables
        PMIX_SUMMARY_ADD([[Required Packages]],[[Libevent]], [pmix_libevent], [yes ($pmix_libevent_source)])

        $1
    else
        AC_MSG_RESULT([no])

        $2
    fi

    PMIX_VAR_SCOPE_POP
])
