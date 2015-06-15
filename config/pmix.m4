dnl -*- shell-script -*-
dnl
dnl This code has been adapted from opal_configure_options.m4 in the Open MPI
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
dnl Copyright (c) 2013-2015 Intel, Inc. All rights reserved
dnl
dnl $COPYRIGHT$
dnl 
dnl Additional copyrights may follow
dnl 
dnl $HEADER$
dnl

# Probably only ever invoked by pmix's configure.ac
AC_DEFUN([PMIX_BUILD_STANDALONE],[
    pmix_mode=standalone
])dnl

AC_DEFUN([PMIX_SETUP_CORE],[

    AC_REQUIRE([AC_USE_SYSTEM_EXTENSIONS])
    AC_REQUIRE([AC_CANONICAL_TARGET])
    AC_REQUIRE([AC_PROG_CC])

    # If no prefix was defined, set a good value
    m4_ifval([$1],
             [m4_define([pmix_config_prefix],[$1/])],
             [m4_define([pmix_config_prefix], [])])

    # Unless previously set to "standalone" mode, default to embedded
    # mode
    AS_IF([test "$pmix_mode" = ""], [pmix_mode=embedded])
    AC_MSG_CHECKING([pmix building mode])
    AC_MSG_RESULT([$pmix_mode])

    # Get pmix's absolute top builddir (which may not be the same as
    # the real $top_builddir, because we may be building in embedded
    # mode).
    PMIX_startdir=`pwd`
    if test x"pmix_config_prefix" != "x" -a ! -d "pmix_config_prefix"; then
        mkdir -p "pmix_config_prefix"
    fi
    if test x"pmix_config_prefix" != "x"; then
        cd "pmix_config_prefix"
    fi
    PMIX_top_builddir=`pwd`
    AC_SUBST(PMIX_top_builddir)

    # Get pmix's absolute top srcdir (which may not be the same as
    # the real $top_srcdir, because we may be building in embedded
    # mode).  First, go back to the startdir incase the $srcdir is
    # relative.

    cd "$PMIX_startdir"
    cd "$srcdir"/pmix_config_prefix
    PMIX_top_srcdir="`pwd`"
    AC_SUBST(PMIX_top_srcdir)

    # Go back to where we started
    cd "$PMIX_startdir"

    AC_MSG_NOTICE([pmix builddir: $PMIX_top_builddir])
    AC_MSG_NOTICE([pmix srcdir: $PMIX_top_srcdir])
    if test "$PMIX_top_builddir" != "$PMIX_top_srcdir"; then
        AC_MSG_NOTICE([Detected VPATH build])
    fi

    # Get the version of pmix that we are installing
    AC_MSG_CHECKING([for pmix version])
    PMIX_VERSION="`$PMIX_top_srcdir/config/pmix_get_version.sh $PMIX_top_srcdir/VERSION`"
    if test "$?" != "0"; then
        AC_MSG_ERROR([Cannot continue])
    fi
    PMIX_RELEASE_DATE="`$PMIX_top_srcdir/config/pmix_get_version.sh $PMIX_top_srcdir/VERSION --release-date`"
    AC_SUBST(PMIX_VERSION)
    AC_DEFINE_UNQUOTED([PMIX_VERSION], ["$PMIX_VERSION"],
                       [The library version, always available, even in embedded mode, contrary to VERSION])
    AC_SUBST(PMIX_RELEASE_DATE)
    AC_MSG_RESULT([$PMIX_VERSION])

    # Debug mode?
    AC_MSG_CHECKING([if want pmix maintainer support])
    pmix_debug=

    # Unconditionally disable debug mode in embedded mode; if someone
    # asks, we can add a configure-time option for it.  Disable it
    # now, however, because --enable-debug is not even added as an
    # option when configuring in embedded mode, and we wouldn't want
    # to hijack the enclosing application's --enable-debug configure
    # switch.
    AS_IF([test "$pmix_mode" = "embedded"],
          [pmix_debug=0
           pmix_debug_msg="disabled (embedded mode)"])
    AS_IF([test "$pmix_debug" = "" -a "$enable_debug" = "yes"],
          [pmix_debug=1
           pmix_debug_msg="enabled"])
    AS_IF([test "$pmix_debug" = ""],
          [pmix_debug=0
           pmix_debug_msg="disabled"])
    # Grr; we use #ifndef for PMIX_DEBUG!  :-(
    AH_TEMPLATE(PMIX_DEBUG, [Whether we are in debugging mode or not])
    AS_IF([test "$pmix_debug" = "1"], [AC_DEFINE([PMIX_DEBUG])])
    AC_MSG_RESULT([$pmix_debug_msg])

    # We need to set a path for header, etc files depending on whether
    # we're standalone or embedded. this is taken care of by PMIX_EMBEDDED.

    AC_MSG_CHECKING([for pmix directory prefix])
    AC_MSG_RESULT(m4_ifval([$1], pmix_config_prefix, [(none)]))

    # Note that private/config.h *MUST* be listed first so that it
    # becomes the "main" config header file.  Any AC-CONFIG-HEADERS
    # after that (pmix/config.h) will only have selective #defines
    # replaced, not the entire file.
    AC_CONFIG_HEADERS(pmix_config_prefix[include/private/autogen/config.h])
    AC_CONFIG_HEADERS(pmix_config_prefix[include/pmix/autogen/config.h])
    AC_CONFIG_HEADERS(pmix_config_prefix[include/pmix/pmix_common.h])
    
    # What prefix are we using?
    AC_MSG_CHECKING([for pmix symbol prefix])
    AS_IF([test "$pmix_symbol_prefix_value" = ""],
          [AS_IF([test "$with_pmix_symbol_prefix" = ""],
                 [pmix_symbol_prefix_value=pmix_],
                 [pmix_symbol_prefix_value=$with_pmix_symbol_prefix])])
    AC_DEFINE_UNQUOTED(PMIX_SYM_PREFIX, [$pmix_symbol_prefix_value],
                       [The pmix symbol prefix])
    # Ensure to [] escape the whole next line so that we can get the
    # proper tr tokens
    [pmix_symbol_prefix_value_caps="`echo $pmix_symbol_prefix_value | tr '[:lower:]' '[:upper:]'`"]
    AC_DEFINE_UNQUOTED(PMIX_SYM_PREFIX_CAPS, [$pmix_symbol_prefix_value_caps],
                       [The pmix symbol prefix in all caps])
    AC_MSG_RESULT([$pmix_symbol_prefix_value])

    # Give an easy #define to know if we need to transform all the
    # pmix names
    AH_TEMPLATE([PMIX_SYM_TRANSFORM], [Whether we need to re-define all the pmix public symbols or not])
    AS_IF([test "$pmix_symbol_prefix_value" = "pmix_"],
          [AC_DEFINE([PMIX_SYM_TRANSFORM], [0])],
          [AC_DEFINE([PMIX_SYM_TRANSFORM], [1])])

    # GCC specifics.
    if test "x$GCC" = "xyes"; then
        PMIX_GCC_CFLAGS="-Wall -Wmissing-prototypes -Wundef"
        PMIX_GCC_CFLAGS="$PMIX_GCC_CFLAGS -Wpointer-arith -Wcast-align"
    fi


])dnl

AC_DEFUN([PMIX_DEFINE_ARGS],[
    # Embedded mode, or standalone?
    AC_ARG_ENABLE([embedded-mode],
                    AC_HELP_STRING([--enable-embedded-mode],
                                   [Using --enable-embedded-mode puts PMIx into "embedded" mode.  The default is --disable-embedded-mode, meaning that PMIx is in "standalone" mode.]))


    # Change the symbol prefix?
    AC_ARG_WITH([pmix-symbol-prefix],
                AC_HELP_STRING([--with-pmix-symbol-prefix=STRING],
                               [STRING can be any valid C symbol name.  It will be prefixed to all public PMIx symbols.  Default: "pmix_"]))

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
if test "$WANT_DEBUG" = "0" -a -z "$enable_debug" -a "$PMIX_DEVEL" = "1"; then
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
     [package/branding string for PMIX])
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
     [ident string for PMIX])
AC_MSG_RESULT([$with_ident_string])

# How to build libltdl
AC_ARG_WITH([libltdl],
    [AC_HELP_STRING([--with-libltdl(=DIR)],
         [Where to find libltdl (this option is ignored if --disable-dlopen is used). Supplying a valid directory name adds DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries.])])

#
# Timing support
#
AC_MSG_CHECKING([if want developer-level timing support])
AC_ARG_ENABLE(timing, 
    AC_HELP_STRING([--enable-timing],
                   [enable developer-level timing code (default: disabled)]))
if test "$enable_timing" = "yes"; then
    AC_MSG_RESULT([yes])
    WANT_TIMING=1
else
    AC_MSG_RESULT([no])
    WANT_TIMING=0
fi

AC_DEFINE_UNQUOTED(PMIX_ENABLE_TIMING, $WANT_TIMING,
    [Whether we want developer-level timing support or not])
AM_CONDITIONAL([PMIX_COMPILE_TIMING], [test "$WANT_TIMING" = "1"])

])dnl

# Specify the symbol prefix
AC_DEFUN([PMIX_SET_SYMBOL_PREFIX],[
    pmix_symbol_prefix_value=$1
])dnl

# This must be a standalone routine so that it can be called both by
# PMIX_INIT and an external caller (if PMIX_INIT is not invoked).
AC_DEFUN([PMIX_DO_AM_CONDITIONALS],[
    AS_IF([test "$pmix_did_am_conditionals" != "yes"],[
        AM_CONDITIONAL([PMIX_BUILD_STANDALONE], [test "$pmix_mode" = "standalone"])

        AM_CONDITIONAL([PMIX_HAVE_GCC], [test "x$GCC" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_PTHREAD],
                       [test "x$pmix_have_pthread" = "xyes"])

        AM_CONDITIONAL([PMIX_HAVE_LINUX], [test "x$pmix_linux" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_BGQ], [test "x$pmix_bgq" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_IRIX], [test "x$pmix_irix" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_DARWIN], [test "x$pmix_darwin" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_FREEBSD], [test "x$pmix_freebsd" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_NETBSD], [test "x$pmix_netbsd" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_SOLARIS], [test "x$pmix_solaris" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_AIX], [test "x$pmix_aix" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_OSF], [test "x$pmix_osf" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_HPUX], [test "x$pmix_hpux" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_WINDOWS], [test "x$pmix_windows" = "xyes"])
        AM_CONDITIONAL([PMIX_HAVE_MINGW32], [test "x$target_os" = "xmingw32"])

        AM_CONDITIONAL([PMIX_EMBEDDED_MODE], [test "x$pmix_mode" = "xembedded"])
    ])
    pmix_did_am_conditionals=yes
])dnl

