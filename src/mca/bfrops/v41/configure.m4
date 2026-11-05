# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# The wire format of an earlier release: spoken only to a peer of one, so
# built only when this PMIx supports such peers in either direction
# (--enable-legacy-clients or --enable-legacy-servers - see config/pmix.m4)
AC_DEFUN([MCA_pmix_bfrops_v41_CONFIG], [
    AC_CONFIG_FILES([src/mca/bfrops/v41/Makefile])
    AS_IF([test "$pmix_legacy_support" = "1"], [$1], [$2])
])dnl
