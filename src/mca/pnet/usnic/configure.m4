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
# Copyright (c) 2006      Sandia National Laboratories. All rights
#                         reserved.
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2017      Los Alamos National Security, LLC.  All rights
#                         reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pnet_usnic_CONFIG([action-if-can-compile],
#                            [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pnet_usnic_CONFIG],[
    AC_CONFIG_FILES([src/mca/pnet/usnic/Makefile])

    AC_ARG_WITH([usnic],
                [AS_HELP_STRING([--with-usnic],
                                [If specified, cause an error if usNIC
                                 support cannot be built])])

    # If --without-usnic was specified, then gracefully exit.
    # Otherwise, do the rest of the config.
    AS_IF([test "x$with_usnic" = "xno"],
          [AC_MSG_WARN([--without-usnic specified; skipping usnic transport])
           $2],
          [_PMIX_PNET_USNIC_DO_CONFIG($1, $2)])
])

AC_DEFUN([_PMIX_PNET_USNIC_DO_CONFIG],[
    PMIX_VAR_SCOPE_PUSH([pmix_pnet_usnic_CPPFLAGS_save])

    pmix_pnet_usnic_happy=yes

    # We only want to build on 64 bit Linux.
    AC_CHECK_SIZEOF([void *])
    AC_MSG_CHECKING([for 64 bit Linux])
    case $host_os in
       *linux*)
           AS_IF([test $ac_cv_sizeof_void_p -eq 8],
                 [],
                 [pmix_pnet_usnic_happy=no])
           ;;
       *)
           pmix_pnet_usnic_happy=no
           ;;
    esac
    AC_MSG_RESULT([$pmix_pnet_usnic_happy])

    AS_IF([test "$pmix_pnet_usnic_happy" = "yes"],
          [ # The usnic PNET component requires OFI libfabric support
           pmix_pnet_usnic_happy=$pmix_ofi_happy])

    # Make sure we can find the OFI libfabric usnic extensions header
    AS_IF([test "$pmix_pnet_usnic_happy" = "yes" ],
          [pmix_pnet_usnic_CPPFLAGS_save=$CPPFLAGS
           CPPFLAGS="$pmix_ofi_CPPFLAGS $CPPFLAGS"
           AC_CHECK_HEADER([rdma/fi_ext_usnic.h],
                            [],
                            [pmix_pnet_usnic_happy=no])
           CPPFLAGS=$pmix_pnet_usnic_CPPFLAGS_save
          ])

    # All done
    AS_IF([test "$pmix_pnet_usnic_happy" = "yes"],
          [$1],
          [AS_IF([test "$with_usnic" = "yes"],
                 [AC_MSG_WARN([--with-usnic was specified, but Cisco usNIC support cannot be built])
                  AC_MSG_ERROR([Cannot continue])],
                 [$2])
          ])

    PMIX_SUMMARY_ADD([[Transports]],[[Cisco usNIC]],[[pnet_usnic]],[$pmix_pnet_usnic_happy])
    PMIX_VAR_SCOPE_POP
])dnl
