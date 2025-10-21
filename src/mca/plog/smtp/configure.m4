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
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2009-2010 Cisco Systems, Inc. All rights reserved.
# Copyright (c) 2014-2018 Intel, Inc. All rights reserved.
# Copyright (c) 2025      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_plog_smtp_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_pmix_plog_smtp_CONFIG], [
    AC_CONFIG_FILES([src/mca/plog/smtp/Makefile])

    AC_ARG_WITH([smtp],
                [AS_HELP_STRING([--with-smtp=DIR],
                                [Search for esmtp headers and libraries in DIR ])])
    AC_ARG_WITH([smtp-libdir],
                [AS_HELP_STRING([--with-smtp-libdir=DIR],
                                [Search for esmtp libraries in DIR ])])

    pmix_smtp_support=0

    OAC_CHECK_PACKAGE([smtp],
                      [plog_smtp],
                      [libesmtp.h],
                      [esmtp],
                      [smtp_create_session],
                      [pmix_smtp_support=1],
                      [pmix_smtp_support=0])

    if test ! -z "$with_smtp" && test "$with_smtp" != "no" && test "$pmix_smtp_support" != "1"; then
        AC_MSG_WARN([SMTP SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    AC_MSG_CHECKING([will smtp support be built])
    AS_IF([test "$pmix_smtp_support" = "1"],
          [$1
           AC_MSG_RESULT([yes])],
          [$2
           AC_MSG_RESULT([no])])

    PMIX_SUMMARY_ADD([External Packages], [ESMTP], [], [${plog_smtp_SUMMARY}])

    # substitute in the things needed to build pcompress/zlib
    AC_SUBST([plog_smtp_CPPFLAGS])
    AC_SUBST([plog_smtp_LDFLAGS])
    AC_SUBST([plog_smtp_LIBS])

    PMIX_EMBEDDED_LIBS="$PMIX_EMBEDDED_LIBS $plog_smtp_LIBS"
    PMIX_EMBEDDED_LDFLAGS="$PMIX_EMBEDDED_LDFLAGS $plog_smtp_LDFLAGS"
    PMIX_EMBEDDED_CPPFLAGS="$PMIX_EMBEDDED_CPPFLAGS $plog_smtp_CPPFLAGS"

])dnl
