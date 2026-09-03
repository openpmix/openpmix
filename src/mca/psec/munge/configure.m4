# -*- shell-script -*-
#
# Copyright (c) 2015-2016 Intel, Inc. All rights reserved
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_psec_munge_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([MCA_pmix_psec_munge_CONFIG],[
    AC_CONFIG_FILES([src/mca/psec/munge/Makefile])

    AC_ARG_WITH([munge],
                [AS_HELP_STRING([--with-munge=DIR],
                                [Build MUNGE support (not built unless requested), searching DIR for headers and libraries])])
    AC_ARG_WITH([munge-libdir],
                [AS_HELP_STRING([--with-munge-libdir=DIR],
                                [Search for munge libraries in DIR ])])

    # MUNGE support is opt-in: it is built only when the user asks for
    # it. OAC_CHECK_PACKAGE ends its search by looking in pkg-config
    # and the default compiler paths, so running it unconditionally
    # would enable this component on any host that merely happens to
    # have MUNGE installed - and would ignore --without-munge.
    psec_munge_support=0

    AS_IF([test "$with_munge" = "no"],
          [pmix_munge_requested=no
           psec_munge_SUMMARY="no (explicitly disabled)"],
          [test -n "$with_munge" || test -n "$with_munge_libdir"],
          [pmix_munge_requested=yes],
          [pmix_munge_requested=no
           psec_munge_SUMMARY="no (not requested)"])

    AS_IF([test "$pmix_munge_requested" = "yes"],
          [OAC_CHECK_PACKAGE([munge],
                             [psec_munge],
                             [munge.h],
                             [munge],
                             [munge_encode],
                             [psec_munge_support=1],
                             [psec_munge_support=0])])

    if test "$pmix_munge_requested" = "yes" && test "$psec_munge_support" != "1"; then
        AC_MSG_WARN([MUNGE SUPPORT REQUESTED AND NOT FOUND.])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    # --enable-test-build force-builds this component (against the
    # non-functional in-source shim in psec_munge.c if libmunge is
    # absent) so it can be compile-checked.
    AC_MSG_CHECKING([will munge support be built])
    AS_IF([test "$psec_munge_support" = "1" || test "$pmix_testbuild" = "1"],
          [$1
           AC_MSG_RESULT([yes])],
          [$2
           AC_MSG_RESULT([no])])

    PMIX_SUMMARY_ADD([External Packages], [munge], [], [${psec_munge_SUMMARY}])

    # set build flags to use in makefile
    AC_SUBST([psec_munge_CPPFLAGS])
    AC_SUBST([psec_munge_LDFLAGS])
    AC_SUBST([psec_munge_LIBS])
])dnl
