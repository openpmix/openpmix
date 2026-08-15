# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pcompress_lz4_CONFIG([action-if-can-compile],
#                           [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pcompress_lz4_CONFIG],[
    AC_CONFIG_FILES([src/mca/pcompress/lz4/Makefile])

    AC_ARG_WITH([lz4],
                [AS_HELP_STRING([--with-lz4=DIR],
                                [Search for lz4 headers and libraries in DIR ])])
    AC_ARG_WITH([lz4-libdir],
                [AS_HELP_STRING([--with-lz4-libdir=DIR],
                                [Search for lz4 libraries in DIR ])])

    pmix_lz4_support=0

    OAC_CHECK_PACKAGE([lz4],
                      [pcompress_lz4],
                      [lz4frame.h],
                      [lz4],
                      [LZ4F_compressFrame],
                      [pmix_lz4_support=1],
                      [pmix_lz4_support=0])

    if test ! -z "$with_lz4" && test "$with_lz4" != "no" && test "$pmix_lz4_support" != "1"; then
        AC_MSG_WARN([LZ4 SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    # --enable-test-build force-builds this component (against the
    # non-functional testbuild_lz4.h shim if the real headers are
    # absent) so it can be compile-checked.
    AC_MSG_CHECKING([will lz4 support be built])
    AS_IF([test "$pmix_lz4_support" = "1" || test "$pmix_testbuild" = "1"],
          [$1
           AC_MSG_RESULT([yes])],
          [$2
           AC_MSG_RESULT([no])])

    PMIX_SUMMARY_ADD([External Packages], [LZ4], [], [${pcompress_lz4_SUMMARY}])

    # substitute in the things needed to build pcompress/lz4
    AC_SUBST([pcompress_lz4_CPPFLAGS])
    AC_SUBST([pcompress_lz4_LDFLAGS])
    AC_SUBST([pcompress_lz4_LIBS])

    PMIX_EMBEDDED_LIBS="$PMIX_EMBEDDED_LIBS $pcompress_lz4_LIBS"
    PMIX_EMBEDDED_LDFLAGS="$PMIX_EMBEDDED_LDFLAGS $pcompress_lz4_LDFLAGS"
    PMIX_EMBEDDED_CPPFLAGS="$PMIX_EMBEDDED_CPPFLAGS $pcompress_lz4_CPPFLAGS"

])dnl
