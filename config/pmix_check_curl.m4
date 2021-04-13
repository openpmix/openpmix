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

# PMIX_CHECK_CURL
# --------------------------------------------------------
# check if CURL support can be found.  sets curl_{CPPFLAGS,
# LDFLAGS, LIBS}
AC_DEFUN([PMIX_CHECK_CURL],[

    PMIX_VAR_SCOPE_PUSH(pmix_check_curl_save_CPPFLAGS pmix_check_curl_save_LDFLAGS pmix_check_curl_save_LIBS)
	AC_ARG_WITH([curl],
		    [AS_HELP_STRING([--with-curl(=DIR)],
				    [Build curl support (default=no), optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])],
            [], [with_curl=no])

    AC_ARG_WITH([curl-libdir],
            [AS_HELP_STRING([--with-curl-libdir=DIR],
                    [Search for Curl libraries in DIR])])

    pmix_check_curl_happy=no
    pmix_curl_source=unknown
    pmix_check_curl_dir=unknown
    pmix_check_curl_libdir=
    pmix_check_curl_basedir=

    if test "$with_curl" != "no"; then
        AC_MSG_CHECKING([where to find curl.h])
        AS_IF([test "$with_curl" = "yes" || test -z "$with_curl"],
              [AS_IF([test -f /usr/include/curl.h],
                     [pmix_check_curl_dir=/usr
                      pmix_check_curl_basedir=/usr
                      pmix_curl_source=standard],
                     [AS_IF([test -f /usr/include/curl/curl.h],
                            [pmix_check_curl_dir=/usr/include/curl
                             pmix_check_curl_basedir=/usr
                             pmix_curl_source=standard])])],
    	      [AS_IF([test -f $with_curl/include/curl.h],
                     [pmix_check_curl_dir=$with_curl/include
                      pmix_check_curl_basedir=$with_curl
                      pmix_curl_source=$with_curl],
                      [AS_IF([test -f $with_curl/include/curl/curl.h],
                             [pmix_check_curl_dir=$with_curl/include/curl
                              pmix_check_curl_basedir=$with_curl
                              pmix_curl_source=$with_curl])])])
        AC_MSG_RESULT([$pmix_check_curl_dir])

        AS_IF([test -z "$with_curl_libdir" || test "$with_curl_libdir" = "yes"],
              [pmix_check_curl_libdir=$pmix_check_curl_basedir],
    	      [PMIX_CHECK_WITHDIR([curl-libdir], [$with_curl_libdir], [libcurl.*])
               pmix_check_curl_libdir=$with_curl_libdir])

    	pmix_check_curl_save_CPPFLAGS="$CPPFLAGS"
    	pmix_check_curl_save_LDFLAGS="$LDFLAGS"
    	pmix_check_curl_save_LIBS="$LIBS"

        PMIX_CHECK_PACKAGE([pmix_check_curl],
		   [curl.h],
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

    PMIX_SUMMARY_ADD([[External Packages]],[[Curl]], [pmix_curl], [$pmix_check_curl_happy ($pmix_curl_source)])

    AC_SUBST(pmix_check_curl_CPPFLAGS)
    AC_SUBST(pmix_check_curl_LDFLAGS)
    AC_SUBST(pmix_check_curl_LIBS)

    PMIX_VAR_SCOPE_POP
])
