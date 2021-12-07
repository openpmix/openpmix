# -*- shell-script -*-
#
# Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017-2019 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PMIX_LIBEV_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
# Attempt to find a libev package.  If found, evaluate
# action-if-found.  Otherwise, evaluate action-if-not-found.
#
# Modifies the following in the environment:
#  * pmix_libev_CPPFLAGS
#  * pmix_libev_LDFLAGS
#  * pmix_libev_LIBS
#
# Adds the following to the wrapper compilers:
#  * CPPFLAGS: none
#  * LDLFGAS: add pmix_libev_LDFLAGS
#  * LIBS: add pmix_libev_LIBS
AC_DEFUN([PMIX_LIBEV_CONFIG],[
    PMIX_VAR_SCOPE_PUSH([pmix_libev_dir pmix_libev_libdir pmix_check_libev_save_CPPFLAGS pmix_check_libev_save_LDFLAGS pmix_check_libev_save_LIBS])

    AC_ARG_WITH([libev],
                [AS_HELP_STRING([--with-libev=DIR],
                                [Search for libev headers and libraries in DIR ])])
    AC_ARG_WITH([libev-libdir],
                [AS_HELP_STRING([--with-libev-libdir=DIR],
                                [Search for libev libraries in DIR ])])

    pmix_libev_support=1

    AS_IF([test "$with_libev" = "no"],
          [AC_MSG_NOTICE([Libev support disabled by user.])
           pmix_libev_support=0])

    AS_IF([test $pmix_libev_support -eq 1],
          [PMIX_CHECK_WITHDIR([libev], [$with_libev], [include/event.h])
           PMIX_CHECK_WITHDIR([libev-libdir], [$with_libev_libdir], [libev.*])

           AC_MSG_CHECKING([for libev in])
           pmix_check_libev_save_CPPFLAGS="$CPPFLAGS"
           pmix_check_libeve_save_LDFLAGS="$LDFLAGS"
           pmix_check_libev_save_LIBS="$LIBS"
           if test -n "$with_libev" -a "$with_libev" != "yes"; then
               pmix_libev_dir=$with_libev/include
               AS_IF([test -z "$with_libev_libdir" || test "$with_libev_libdir" = "yes"],
                     [if test -d $with_libev/lib; then
                          pmix_libev_libdir=$with_libev/lib
                      elif test -d $with_libev/lib64; then
                          pmix_libev_libdir=$with_libev/lib64
                      else
                          AC_MSG_RESULT([Could not find $with_libev/lib or $with_libev/lib64])
                          AC_MSG_ERROR([Can not continue])
                      fi
                      AC_MSG_RESULT([$pmix_libev_dir and $pmix_libev_libdir])],
                     [AC_MSG_RESULT([$with_libev_libdir])])
           else
               AC_MSG_RESULT([(default search paths)])
           fi
           AS_IF([test ! -z "$with_libev_libdir" && test "$with_libev_libdir" != "yes"],
                 [pmix_libev_libdir="$with_libev_libdir"])

           PMIX_CHECK_PACKAGE([pmix_libev],
                              [event.h],
                              [ev],
                              [ev_async_send],
                              [],
                              [$pmix_libev_dir],
                              [$pmix_libev_libdir],
                              [],
                              [pmix_libev_support=0])
           CPPFLAGS="$pmix_check_libev_save_CPPFLAGS"
           LDFLAGS="$pmix_check_libev_save_LDFLAGS"
           LIBS="$pmix_check_libev_save_LIBS"])

    AS_IF([test $pmix_libev_support -eq 1],
          [PMIX_FLAGS_APPEND_UNIQ([PMIX_FINAL_CPPFLAGS], [$pmix_libev_CPPFLAGS])
           PMIX_WRAPPER_FLAGS_ADD([CPPFLAGS], [$pmix_libev_CPPFLAGS])

           PMIX_FLAGS_APPEND_UNIQ([PMIX_FINAL_LDFLAGS], [$pmix_libev_LDFLAGS])
           PMIX_WRAPPER_FLAGS_ADD([LDFLAGS], [$pmix_libev_LDFLAGS])

           PMIX_FLAGS_APPEND_UNIQ([PMIX_FINAL_LIBS], [$pmix_libev_LIBS])
           PMIX_WRAPPER_FLAGS_ADD([LIBS], [$pmix_libev_LIBS])])

    AC_MSG_CHECKING([will libev support be built])
    if test $pmix_libev_support -eq 1; then
        AC_MSG_RESULT([yes])
        $1
        PMIX_SUMMARY_ADD([[Required Packages]], [[Libev]], [pmix_libev], [yes ($pmix_libev_dir)])
    else
        AC_MSG_RESULT([no])
        # if they asked us to use it, then this is an error
        AS_IF([test -n "$with_libev" && test "$with_libev" != "no"],
              [AC_MSG_WARN([LIBEV SUPPORT REQUESTED AND NOT FOUND])
               AC_MSG_ERROR([CANNOT CONTINUE])])
        $2
    fi

    AC_DEFINE_UNQUOTED([PMIX_HAVE_LIBEV], [$pmix_libev_support], [Whether we are building against libev])

    PMIX_VAR_SCOPE_POP
])dnl
