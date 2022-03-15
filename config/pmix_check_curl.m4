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
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# PMIX_CHECK_CURL
# --------------------------------------------------------
# check if CURL support can be found.  sets curl_{CPPFLAGS,
# LDFLAGS, LIBS}
AC_DEFUN([PMIX_CHECK_CURL],[

    PMIX_VAR_SCOPE_PUSH(pmix_check_curl_save_CPPFLAGS pmix_check_curl_save_LDFLAGS pmix_check_curl_save_LIBS)

    dnl Intentionally disable CURL unless explicitly requested
    AC_ARG_WITH([curl],
                [AS_HELP_STRING([--with-curl(=DIR)],
                   [Build curl support (default=no), optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])],
                [], [with_curl=no])

    AC_ARG_WITH([curl-libdir],
            [AS_HELP_STRING([--with-curl-libdir=DIR],
                    [Search for Curl libraries in DIR])])

    pmix_check_curl_happy=no
    pmix_curl_source=
    pmix_check_curl_dir=
    pmix_check_curl_libdir=

    if test "$with_curl" != "no"; then
        PMIX_CHECK_WITHDIR([curl-libdir], [$with_curl_libdir], [libcurl.*])

        AS_IF([test "$with_curl" = "yes" -o -z "$with_curl"],
              [pmix_curl_source="Standard locations"],
              [pmix_check_curl_dir="$with_curl"])
        AS_IF([test "$with_curl_libdir" != "yes" -a "$with_curl_libdir" != "no"],
              [pmix_check_curl_libdir="$with_curl_libdir"])

    	pmix_check_curl_save_CPPFLAGS="$CPPFLAGS"
    	pmix_check_curl_save_LDFLAGS="$LDFLAGS"
    	pmix_check_curl_save_LIBS="$LIBS"

        PMIX_CHECK_PACKAGE([pmix_check_curl],
		   [curl/curl.h],
		   [curl],
		   [curl_easy_getinfo],
		   [],
		   [$pmix_check_curl_dir],
		   [$pmix_check_curl_libdir],
		   [pmix_check_curl_happy="yes"],
		   [pmix_check_curl_happy="no"])

    	CPPFLAGS="$pmix_check_curl_save_CPPFLAGS"
    	LDFLAGS="$pmix_check_curl_save_LDFLAGS"
    	LIBS="$pmix_check_curl_save_LIBS"
    fi

    AS_IF([test "$pmix_check_curl_happy" = "no" && test ! -z "$with_curl" && test "$with_curl" != "no"],
          [AC_MSG_ERROR([curl support requested but not found.  Aborting])])

    AC_MSG_CHECKING([libcurl support available])
    AC_MSG_RESULT([$pmix_check_curl_happy])

    if test -z "$pmix_curl_source"; then
        PMIX_SUMMARY_ADD([External Packages], [Curl], [], [$pmix_check_curl_happy])
    else
        PMIX_SUMMARY_ADD([External Packages], [Curl], [], [$pmix_check_curl_happy ($pmix_curl_source)])
    fi

    AC_SUBST(pmix_check_curl_CPPFLAGS)
    AC_SUBST(pmix_check_curl_LDFLAGS)
    AC_SUBST(pmix_check_curl_LIBS)

    PMIX_VAR_SCOPE_POP
])
