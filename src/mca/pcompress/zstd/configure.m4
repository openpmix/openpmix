# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pcompress_zstd_CONFIG([action-if-can-compile],
#                           [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pcompress_zstd_CONFIG],[
    AC_CONFIG_FILES([src/mca/pcompress/zstd/Makefile])

    AC_ARG_WITH([zstd],
                [AS_HELP_STRING([--with-zstd=DIR],
                                [Search for zstd headers and libraries in DIR ])])
    AC_ARG_WITH([zstd-libdir],
                [AS_HELP_STRING([--with-zstd-libdir=DIR],
                                [Search for zstd libraries in DIR ])])

    pmix_zstd_support=0

    OAC_CHECK_PACKAGE([zstd],
                      [pcompress_zstd],
                      [zstd.h],
                      [zstd],
                      [ZSTD_compress],
                      [pmix_zstd_support=1],
                      [pmix_zstd_support=0])

    if test ! -z "$with_zstd" && test "$with_zstd" != "no" && test "$pmix_zstd_support" != "1"; then
        AC_MSG_WARN([ZSTD SUPPORT REQUESTED AND NOT FOUND])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    # --enable-test-build force-builds this component (against the
    # non-functional testbuild_zstd.h shim if the real headers are
    # absent) so it can be compile-checked.
    AC_MSG_CHECKING([will zstd support be built])
    AS_IF([test "$pmix_zstd_support" = "1" || test "$pmix_testbuild" = "1"],
          [$1
           AC_MSG_RESULT([yes])],
          [$2
           AC_MSG_RESULT([no])])

    PMIX_SUMMARY_ADD([External Packages], [ZSTD], [], [${pcompress_zstd_SUMMARY}])

    # substitute in the things needed to build pcompress/zstd
    AC_SUBST([pcompress_zstd_CPPFLAGS])
    AC_SUBST([pcompress_zstd_LDFLAGS])
    AC_SUBST([pcompress_zstd_LIBS])

    PMIX_EMBEDDED_LIBS="$PMIX_EMBEDDED_LIBS $pcompress_zstd_LIBS"
    PMIX_EMBEDDED_LDFLAGS="$PMIX_EMBEDDED_LDFLAGS $pcompress_zstd_LDFLAGS"
    PMIX_EMBEDDED_CPPFLAGS="$PMIX_EMBEDDED_CPPFLAGS $pcompress_zstd_CPPFLAGS"

])dnl
