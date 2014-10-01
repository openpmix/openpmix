dnl
dnl Copyright (c) 2004-2009 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2007 High Performance Computing Center Stuttgart, 
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2006-2013 Cisco Systems, Inc.  All rights reserved.
dnl Copyright (c) 2006-2008 Sun Microsystems, Inc.  All rights reserved.
dnl Copyright (c) 2006-2007 Los Alamos National Security, LLC.  All rights
dnl                         reserved. 
dnl Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
dnl Copyright (c) 2014      Intel, Inc. All rights reserved.
dnl $COPYRIGHT$
dnl 
dnl Additional copyrights may follow
dnl 
dnl $HEADER$
dnl

AC_DEFUN([PMIX_SETUP_LIBLTDL],[
    PMIX_VAR_SCOPE_PUSH([HAPPY])

    pmix_show_subtitle "GNU libltdl setup"

    # AC_CONFIG_SUBDIRS appears to be broken for non-gcc compilers (i.e.,
    # passing precious variables down to the sub-configure).
    #
    # Finally, make ltdl follow the same shared/static convention that was
    # user for the main OMPI libraries.  So manually examine
    # $enable_shared and $enable_static and pass down the corresponding
    # flags.

    LIBLTDL_SUBDIR=
    PMIX_HAVE_LTDL_ADVISE=0
    PMIX_LIBLTDL_INTERNAL=0

    AS_IF([test "$PMIX_ENABLE_DLOPEN_SUPPORT" = "0"],
          [AC_MSG_WARN([libltdl support disabled (by --disable-dlopen)])
           LIBLTDL=
           LDTLINCL=
          ],
          [AS_IF([test -z "$libltdl_location" -o "$libltdl_location" = "yes"],
                 [libltdl_location=],
		 [libltdl_location=$with_libltdl])

           PMIX_CHECK_PACKAGE([libltdl],
                              [ltdl.h],
                              [ltdl],
                              [lt_dlopen],
                              [],
                              [$libltdl_location],
                              [],
                              [],
                              [AC_MSG_WARN([External libltdl installation not found])
                               AC_MSG_WARN([or not usable.])
                               AC_MSG_ERROR([Cannot continue.])])
           CPPFLAGS="$CPPFLAGS $libltdl_CPPFLAGS"
           LDFLAGS="$LDFLAGS $libltdl_LDFLAGS"
           LIBS="$LIBS $libltdl_LIBS"

           # Check for lt_dladvise_init; warn if we don't have it
           AC_CHECK_FUNC([lt_dladvise_init],
		         [PMIX_HAVE_LTDL_ADVISE=1],
                         [AC_MSG_WARN([*********************************************])
                          AC_MSG_WARN([Could not find lt_dladvise_init in the])
                          AC_MSG_WARN([external libltdl installation.])
                          AC_MSG_WARN([This could mean that your libltdl version])
                          AC_MSG_WARN([is old.  We recommend that you re-configure])
                          AC_MSG_WARN([Open MPI with --with-libltdl=internal to])
                          AC_MSG_WARN([use the internal libltdl copy in Open MPI.])
                          AC_MSG_WARN([])
                          AC_MSG_WARN([Sleeping 10 seconds to give you a])
                          AC_MSG_WARN([chance to read this message.])
                          AC_MSG_WARN([*********************************************])
                          sleep 10
                          ])
            ])

    AC_SUBST(LTDLINCL)
    AC_SUBST(LIBLTDL)
    AC_SUBST(LIBLTDL_SUBDIR)

    AC_DEFINE_UNQUOTED(PMIX_HAVE_LTDL_ADVISE, $PMIX_HAVE_LTDL_ADVISE,
        [Whether libltdl appears to have the lt_dladvise interface])

    AC_DEFINE_UNQUOTED(PMIX_WANT_LIBLTDL, $PMIX_ENABLE_DLOPEN_SUPPORT,
        [Whether to include support for libltdl or not])
    AC_DEFINE_UNQUOTED(PMIX_LIBLTDL_INTERNAL, $PMIX_LIBLTDL_INTERNAL,
        [Whether we are using the internal libltdl or not])

    AM_CONDITIONAL(PMIX_HAVE_DLOPEN, 
                   [test "$PMIX_ENABLE_DLOPEN_SUPPORT" = "1"])
    PMIX_VAR_SCOPE_POP([HAPPY])
])dnl
