dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2006-2016 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
dnl Copyright (c) 2009      IBM Corporation.  All rights reserved.
dnl Copyright (c) 2009      Los Alamos National Security, LLC.  All rights
dnl                         reserved.
dnl Copyright (c) 2009-2011 Oak Ridge National Labs.  All rights reserved.
dnl Copyright (c) 2011-2015 NVIDIA Corporation.  All rights reserved.
dnl Copyright (c) 2015      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

AC_DEFUN([PMIX_CHECK_CUDA],[
#
# Check to see if user wants CUDA support
#
    AC_ARG_WITH([cuda],
                [AS_HELP_STRING([--with-cuda(=DIR)],
                [Build cuda support, optionally adding DIR/include])])

    AS_IF([test ! -z "$with_cuda" && test "$with_cuda" != "yes"],
          [pmix_cuda_dir="$with_cuda"],
          [pmix_cuda_dir=""])

    dnl Just check for the presence of the header for now

    _PMIX_CHECK_PACKAGE_HEADER([pmix_cuda], [cuda.h], [pmix_cuda_dir],
                               [pmix_cuda_happy=yes],
                               [pmix_cuda_happy=no])

    AC_MSG_CHECKING([if have cuda support])
    AC_MSG_RESULT([$pmix_cuda_happy])

    PMIX_SUMMARY_ADD([[External Packages]],[[CUDA]],[pmix_cuda],[$pmix_cuda_happy])])])

])
