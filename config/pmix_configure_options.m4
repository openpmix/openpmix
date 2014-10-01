dnl -*- shell-script -*-
dnl
dnl PMIx copyrights:
dnl Copyright (c) 2013-2014 Intel, Inc. All rights reserved
dnl
dnl########################
dnl This code has been adapted from pmix_configure_options.m4 in the Open MPI
dnl code base - per the Open MPI license, all copyrights are retained below.
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
dnl Copyright (c) 2006-2010 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
dnl Copyright (c) 2009      IBM Corporation.  All rights reserved.
dnl Copyright (c) 2009      Los Alamos National Security, LLC.  All rights
dnl                         reserved.
dnl Copyright (c) 2009-2011 Oak Ridge National Labs.  All rights reserved.
dnl Copyright (c) 2011-2013 NVIDIA Corporation.  All rights reserved.
dnl Copyright (c) 2013      Intel, Inc. All rights reserved
dnl########################
dnl
dnl $COPYRIGHT$
dnl 
dnl Additional copyrights may follow
dnl 
dnl $HEADER$
dnl

AC_DEFUN([PMIX_CONFIGURE_OPTIONS],[
pmix_show_subtitle "PMIX Configuration options"


#
# Is this a developer copy?
#

if test -d .git; then
    PMIX_DEVEL=1
else
    PMIX_DEVEL=0
fi


#
# Developer picky compiler options
#

AC_MSG_CHECKING([if want developer-level compiler pickyness])
AC_ARG_ENABLE(picky, 
    AC_HELP_STRING([--enable-picky],
                   [enable developer-level compiler pickyness when building PMIx (default: disabled)]))
if test "$enable_picky" = "yes"; then
    AC_MSG_RESULT([yes])
    WANT_PICKY_COMPILER=1
else
    AC_MSG_RESULT([no])
    WANT_PICKY_COMPILER=0
fi
#################### Early development override ####################
if test "$WANT_PICKY_COMPILER" = "0" -a -z "$enable_picky" -a "$PMIX_DEVEL" = 1; then
    WANT_PICKY_COMPILER=1
    echo "--> developer override: enable picky compiler by default"
fi
#################### Early development override ####################

#
# Developer debugging
#

AC_MSG_CHECKING([if want developer-level debugging code])
AC_ARG_ENABLE(debug, 
    AC_HELP_STRING([--enable-debug],
                   [enable developer-level debugging code (not for general PMIx users!) (default: disabled)]))
if test "$enable_debug" = "yes"; then
    AC_MSG_RESULT([yes])
    WANT_DEBUG=1
else
    AC_MSG_RESULT([no])
    WANT_DEBUG=0
fi
#################### Early development override ####################
if test "$WANT_DEBUG" = "0" -a -z "$enable_debug" -a "$PMIX_DEVEL" = 1; then
    WANT_DEBUG=1
    echo "--> developer override: enable debugging code by default"
fi
#################### Early development override ####################
if test "$WANT_DEBUG" = "0"; then
    CFLAGS="-DNDEBUG $CFLAGS"
    CXXFLAGS="-DNDEBUG $CXXFLAGS"
fi
AC_DEFINE_UNQUOTED(PMIX_ENABLE_DEBUG, $WANT_DEBUG,
    [Whether we want developer-level debugging code or not])

AC_ARG_ENABLE(debug-symbols,
    AC_HELP_STRING([--disable-debug-symbols],
        [Disable adding compiler flags to enable debugging symbols if --enable-debug is specified.  For non-debugging builds, this flag has no effect.]))

#
# Do we want the pretty-print stack trace feature?
#

AC_MSG_CHECKING([if want pretty-print stacktrace])
AC_ARG_ENABLE([pretty-print-stacktrace],
    [AC_HELP_STRING([--enable-pretty-print-stacktrace],
                    [Pretty print stacktrace on process signal (default: enabled)])])
if test "$enable_pretty_print_stacktrace" = "no" ; then
    AC_MSG_RESULT([no])
    WANT_PRETTY_PRINT_STACKTRACE=0
else
    AC_MSG_RESULT([yes])
    WANT_PRETTY_PRINT_STACKTRACE=1
fi
AC_DEFINE_UNQUOTED([PMIX_WANT_PRETTY_PRINT_STACKTRACE],
                   [$WANT_PRETTY_PRINT_STACKTRACE],
                   [if want pretty-print stack trace feature])

#
# Do we want to allow DLOPEN?
#

AC_MSG_CHECKING([if want dlopen support])
AC_ARG_ENABLE([dlopen],
    [AC_HELP_STRING([--enable-dlopen],
                    [Whether build should attempt to use dlopen (or
                     similar) to dynamically load components.
                     Disabling dlopen implies --disable-mca-dso.
                     (default: enabled)])])
if test "$enable_dlopen" = "no" ; then
    PMIX_ENABLE_DLOPEN_SUPPORT=0
    AC_MSG_RESULT([no])
else
    PMIX_ENABLE_DLOPEN_SUPPORT=1
    AC_MSG_RESULT([yes])
fi


#
# Package/brand string
#
AC_MSG_CHECKING([if want package/brand string])
AC_ARG_WITH([package-string],
     [AC_HELP_STRING([--with-package-string=STRING],
                     [Use a branding string throughout PMIx])])
if test "$with_package_string" = "" -o "$with_package_string" = "no"; then
    with_package_string="Open MPI $PMIX_CONFIGURE_USER@$PMIX_CONFIGURE_HOST Distribution"
fi
AC_DEFINE_UNQUOTED([PMIX_PACKAGE_STRING], ["$with_package_string"],
     [package/branding string for Open MPI])
AC_MSG_RESULT([$with_package_string])

#
# Ident string
#
AC_MSG_CHECKING([if want ident string])
AC_ARG_WITH([ident-string],
     [AC_HELP_STRING([--with-ident-string=STRING],
                     [Embed an ident string into PMIx object files])])
if test "$with_ident_string" = "" -o "$with_ident_string" = "no"; then
    with_ident_string="%VERSION%"
fi
# This is complicated, because $PMIX_VERSION may have spaces in it.
# So put the whole sed expr in single quotes -- i.e., directly
# substitute %VERSION% for (not expanded) $PMIX_VERSION.
with_ident_string="`echo $with_ident_string | sed -e 's/%VERSION%/$PMIX_VERSION/'`"

# Now eval an echo of that so that the "$PMIX_VERSION" token is
# replaced with its value.  Enclose the whole thing in "" so that it
# ends up as 1 token.
with_ident_string="`eval echo $with_ident_string`"

AC_DEFINE_UNQUOTED([PMIX_IDENT_STRING], ["$with_ident_string"],
     [ident string for Open MPI])
AC_MSG_RESULT([$with_ident_string])

# How to build libltdl
AC_ARG_WITH([libltdl],
    [AC_HELP_STRING([--with-libltdl(=DIR)],
         [Where to find libltdl (this option is ignored if --disable-dlopen is used). Supplying a valid directory name adds DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries.])])


])dnl
