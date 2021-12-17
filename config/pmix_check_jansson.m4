# -*- shell-script -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2006 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006      QLogic Corp. All rights reserved.
# Copyright (c) 2009-2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PMIX_CHECK_JANSSON
# --------------------------------------------------------
# check if JANSSON support can be found.  sets jansson_{CPPFLAGS,
# LDFLAGS, LIBS}
AC_DEFUN([PMIX_CHECK_JANSSON],[

    PMIX_VAR_SCOPE_PUSH(pmix_check_jansson_save_CPPFLAGS pmix_check_jansson_save_LDFLAGS pmix_check_jansson_save_LIBS)

    dnl Intentionally disable Jansson unless explicitly requested
    AC_ARG_WITH([jansson],
                [AS_HELP_STRING([--with-jansson(=DIR)],
                   [Build jansson support (default=no), optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])],
                [], [with_jansson=no])

    AC_ARG_WITH([jansson-libdir],
            [AS_HELP_STRING([--with-jansson-libdir=DIR],
                    [Search for Jansson libraries in DIR])])

    pmix_check_jansson_happy=no
    pmix_jansson_source=
    pmix_check_jansson_dir=
    pmix_check_jansson_libdir=

    if test "$with_jansson" != "no"; then
        PMIX_CHECK_WITHDIR([jansson-libdir], [$with_jansson_libdir], [libjansson.*])

        AS_IF([test "$with_jansson" = "yes" -o -z "$with_jansson"],
              [pmix_jansson_source="Standard locations"],
              [pmix_check_jansson_dir="$with_jansson"])
        AS_IF([test "$with_jansson_libdir" != "yes" -a "$with_jansson_libdir" != "no"],
              [pmix_check_jansson_libdir="$with_jansson_libdir"])

    	pmix_check_jansson_save_CPPFLAGS="$CPPFLAGS"
    	pmix_check_jansson_save_LDFLAGS="$LDFLAGS"
    	pmix_check_jansson_save_LIBS="$LIBS"

        PMIX_CHECK_PACKAGE([pmix_check_jansson],
		                   [jansson.h],
	 	                   [jansson],
		                   [json_loads],
		                   [],
		                   [$pmix_check_jansson_dir],
		                   [$pmix_check_jansson_libdir],
		                   [pmix_check_jansson_happy="yes"],
		                   [pmix_check_jansson_happy="no"])

        if test "$pmix_check_jansson_happy" == "yes"; then
            AC_MSG_CHECKING([if libjansson version is 2.11 or greater])
            AS_IF([test "$pmix_jansson_source" != "standard"],
                  [PMIX_FLAGS_APPEND_UNIQ(CPPFLAGS, $pmix_check_jansson_CPPFLAGS)])
            AC_COMPILE_IFELSE(
                  [AC_LANG_PROGRAM([[#include <jansson.h>]],
                  [[
        #if JANSSON_VERSION_HEX < 0x00020b00
        #error "jansson API version is less than 2.11"
        #endif
                  ]])],
                  [AC_MSG_RESULT([yes])],
                  [AC_MSG_RESULT([no])
                   pmix_check_jansson_happy=no])
        fi

    	CPPFLAGS="$pmix_check_jansson_save_CPPFLAGS"
    	LDFLAGS="$pmix_check_jansson_save_LDFLAGS"
    	LIBS="$pmix_check_jansson_save_LIBS"
    fi

    AS_IF([test "$pmix_check_jansson_happy" = "no" && test ! -z "$with_jansson" && test "$with_jansson" != "no"],
          [AC_MSG_ERROR([Jansson support requested but not found.  Aborting])])

    AC_SUBST(pmix_check_jansson_CPPFLAGS)
    AC_SUBST(pmix_check_jansson_LDFLAGS)
    AC_SUBST(pmix_check_jansson_LIBS)

    AC_MSG_CHECKING([Jansson support available])
    AC_MSG_RESULT([$pmix_check_jansson_happy])

    AM_CONDITIONAL([HAVE_JANSSON], [test "$pmix_check_jansson_happy" = "yes"])

    if test -z "$pmix_jansson_source"; then
        PMIX_SUMMARY_ADD([[External Packages]],[[Jansson]], [pmix_jansson], [$pmix_check_jansson_happy])
    else
        PMIX_SUMMARY_ADD([[External Packages]],[[Jansson]], [pmix_jansson], [$pmix_check_jansson_happy ($pmix_jansson_source)])
    fi

    PMIX_VAR_SCOPE_POP
])
