# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_psec_ssl_CONFIG([action-if-found], [action-if-not-found])
# --------------------------------------------------------------------
AC_DEFUN([MCA_pmix_psec_ssl_CONFIG],[
    AC_CONFIG_FILES([src/mca/psec/ssl/Makefile])

    AC_ARG_WITH([openssl],
                [AS_HELP_STRING([--with-openssl=DIR],
                                [Build the ssl security component, which authenticates remote peers with X.509 certificates (not built unless requested), searching DIR for the OpenSSL headers and libraries])])
    AC_ARG_WITH([openssl-libdir],
                [AS_HELP_STRING([--with-openssl-libdir=DIR],
                                [Search for the OpenSSL libraries in DIR])])

    # Opt-in, for the same reason as munge: OAC_CHECK_PACKAGE ends its
    # search in pkg-config and the default compiler paths, and OpenSSL is
    # present nearly everywhere - so searching unconditionally would add a
    # libcrypto dependency to every libpmix built on such a host, whether
    # or not anyone wanted certificate authentication.
    psec_ssl_support=0

    AS_IF([test "$with_openssl" = "no"],
          [pmix_openssl_requested=no
           psec_ssl_SUMMARY="no (explicitly disabled)"],
          [test -n "$with_openssl" || test -n "$with_openssl_libdir"],
          [pmix_openssl_requested=yes],
          [pmix_openssl_requested=no
           psec_ssl_SUMMARY="no (not requested)"])

    # EVP_DigestSign is the one-shot signing call, added in OpenSSL
    # 1.1.1 - the oldest release this component supports
    AS_IF([test "$pmix_openssl_requested" = "yes"],
          [OAC_CHECK_PACKAGE([openssl],
                             [psec_ssl],
                             [openssl/evp.h],
                             [crypto],
                             [EVP_DigestSign],
                             [psec_ssl_support=1],
                             [psec_ssl_support=0])])

    if test "$pmix_openssl_requested" = "yes" && test "$psec_ssl_support" != "1"; then
        AC_MSG_WARN([OPENSSL SUPPORT REQUESTED AND NOT FOUND.])
        AC_MSG_ERROR([CANNOT CONTINUE])
    fi

    # --enable-test-build force-builds this component against the
    # non-functional shim in testbuild_ssl.h, so it can be compile-checked
    # on a host without OpenSSL
    AC_MSG_CHECKING([will ssl security support be built])
    AS_IF([test "$psec_ssl_support" = "1" || test "$pmix_testbuild" = "1"],
          [$1
           AS_IF([test "$psec_ssl_support" = "1"], [psec_ssl_SUMMARY="yes"])
           AC_MSG_RESULT([yes])],
          [$2
           AC_MSG_RESULT([no])])

    PMIX_SUMMARY_ADD([External Packages], [OpenSSL (psec/ssl)], [], [${psec_ssl_SUMMARY}])

    # test/unit/psec_ssl links OpenSSL itself to mint certificates, so it
    # is built only against the real library - never against the shim
    AM_CONDITIONAL([PMIX_HAVE_PSEC_SSL], [test "$psec_ssl_support" = "1"])

    # set build flags to use in makefile
    AC_SUBST([psec_ssl_CPPFLAGS])
    AC_SUBST([psec_ssl_LDFLAGS])
    AC_SUBST([psec_ssl_LIBS])
])dnl
