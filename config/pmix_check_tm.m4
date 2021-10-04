dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2015-2017 Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PMIX_CHECK_TM_LIBS_FLAGS(prefix, [LIBS or LDFLAGS])
# ---------------------------------------------------
AC_DEFUN([PMIX_CHECK_TM_LIBS_FLAGS],[
    PMIX_VAR_SCOPE_PUSH([pmix_check_tm_flags])
    pmix_check_tm_flags=`$pmix_check_tm_pbs_config --libs`
    for pmix_check_tm_val in $pmix_check_tm_flags; do
        if test "`echo $pmix_check_tm_val | cut -c1-2`" = "-l"; then
            if test "$2" = "LIBS"; then
                $1_$2="$$1_$2 $pmix_check_tm_val"
            fi
        else
            if test "$2" = "LDFLAGS"; then
                $1_$2="$$1_$2 $pmix_check_tm_val"
            fi
        fi
    done
    PMIX_VAR_SCOPE_POP
])


# PMIX_CHECK_TM(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PMIX_CHECK_TM],[
    if test -z $pmix_check_tm_happy ; then
	PMIX_VAR_SCOPE_PUSH([pmix_check_tm_found pmix_check_tm_dir pmix_check_tm_libdir pmix_check_tm_pbs_config pmix_check_tm_LDFLAGS_save pmix_check_tm_CPPFLAGS_save pmix_check_tm_LIBS_save])

	AC_ARG_WITH([tm],
                    [AS_HELP_STRING([--with-tm(=DIR)],
                                    [Build TM (Torque, PBSPro, and compatible) support, optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])])
	PMIX_CHECK_WITHDIR([tm], [$with_tm], [include/tm.h])

	pmix_check_tm_found=no
	AS_IF([test "$with_tm" = "no"],
              [pmix_check_tm_happy="no"],
              [pmix_check_tm_happy="yes"
               AS_IF([test ! -z "$with_tm" && test "$with_tm" != "yes"],
                     [pmix_check_tm_dir="$with_tm"
                      AC_MSG_CHECKING([for TM/PBS library in])
                      if test -d $with_tm/lib64; then
                        pmix_check_tm_libdir="$with_tm/lib64"
                      elif test -d $with_tm/lib; then
                        pmix_check_tm_libdir="$with_tm/lib"
                      else
                         AC_MSG_RESULT([Could not find $with_tm/lib or $with_tm/lib64])
                         AC_MSG_ERROR([Can not continue])
                     fi
                     AC_MSG_RESULT([($pmix_check_tm_libdir)])],
                     [pmix_check_tm_dir=""
                      pmix_check_tm_libdir=""])])

	AS_IF([test "$pmix_check_tm_happy" = "yes"],
              [AC_MSG_CHECKING([for pbs-config])
               pmix_check_tm_pbs_config="not found"
               AS_IF([test "$pmix_check_tm_dir" != "" && test -d "$pmix_check_tm_dir" && test -x "$pmix_check_tm_dir/bin/pbs-config"],
                     [pmix_check_tm_pbs_config="$pmix_check_tm_dir/bin/pbs-config"],
                     [AS_IF([pbs-config --prefix >/dev/null 2>&1],
                            [pmix_check_tm_pbs_config="pbs-config"])])
               AC_MSG_RESULT([$pmix_check_tm_pbs_config])])

	# If we have pbs-config, get the flags we need from there and then
	# do simplistic tests looking for the tm headers and symbols

	AS_IF([test "$pmix_check_tm_happy" = "yes" && test "$pmix_check_tm_pbs_config" != "not found"],
              [pmix_check_tm_CPPFLAGS=`$pmix_check_tm_pbs_config --cflags`
               PMIX_LOG_MSG([pmix_check_tm_CPPFLAGS from pbs-config: $pmix_check_tm_CPPFLAGS], 1)

               PMIX_CHECK_TM_LIBS_FLAGS([pmix_check_tm], [LDFLAGS])
               PMIX_LOG_MSG([pmix_check_tm_LDFLAGS from pbs-config: $pmix_check_tm_LDFLAGS], 1)

               PMIX_CHECK_TM_LIBS_FLAGS([pmix_check_tm], [LIBS])
               PMIX_LOG_MSG([pmix_check_tm_LIBS from pbs-config: $pmix_check_tm_LIBS], 1)

               # Now that we supposedly have the right flags, try them out.

               pmix_check_tm_CPPFLAGS_save="$CPPFLAGS"
               pmix_check_tm_LDFLAGS_save="$LDFLAGS"
               pmix_check_tm_LIBS_save="$LIBS"

               CPPFLAGS="$CPPFLAGS $pmix_check_tm_CPPFLAGS"
               LIBS="$LIBS $pmix_check_tm_LIBS"
               LDFLAGS="$LDFLAGS $pmix_check_tm_LDFLAGS"

               AC_CHECK_HEADER([tm.h],
			       [AC_CHECK_FUNC([tm_finalize],
					      [pmix_check_tm_found="yes"])])

               CPPFLAGS="$pmix_check_tm_CPPFLAGS_save"
               LDFLAGS="$pmix_check_tm_LDFLAGS_save"
               LIBS="$pmix_check_tm_LIBS_save"])

	# If we don't have pbs-config, then we have to look around
	# manually.

	# Note that Torque 2.1.0 changed the name of their back-end
	# library to "libtorque".  So we have to check for both libpbs and
	# libtorque.  First, check for libpbs.

	pmix_check_package_$1_save_CPPFLAGS="$CPPFLAGS"
	pmix_check_package_$1_save_LDFLAGS="$LDFLAGS"
	pmix_check_package_$1_save_LIBS="$LIBS"

	AS_IF([test "$pmix_check_tm_found" = "no"],
              [AS_IF([test "$pmix_check_tm_happy" = "yes"],
                     [_PMIX_CHECK_PACKAGE_HEADER([pmix_check_tm],
						 [tm.h],
						 [$pmix_check_tm_dir],
						 [pmix_check_tm_found="yes"],
						 [pmix_check_tm_found="no"])])

               AS_IF([test "$pmix_check_tm_found" = "yes"],
                     [_PMIX_CHECK_PACKAGE_LIB([pmix_check_tm],
					      [pbs],
					      [tm_init],
					      [],
					      [$pmix_check_tm_dir],
					      [$pmix_check_tm_libdir],
					      [pmix_check_tm_found="yes"],
                                              [_PMIX_CHECK_PACKAGE_LIB([pmix_check_tm],
					                               [pbs],
					                               [tm_init],
					                               [-lcrypto -lz],
					                               [$pmix_check_tm_dir],
					                               [$pmix_check_tm_libdir],
					                               [pmix_check_tm_found="yes"],
					                               [_PMIX_CHECK_PACKAGE_LIB([pmix_check_tm],
								                                [torque],
								                                [tm_init],
								                                [],
								                                [$pmix_check_tm_dir],
								                                [$pmix_check_tm_libdir],
								                                [pmix_check_tm_found="yes"],
								                                [pmix_check_tm_found="no"])])])])])

	CPPFLAGS="$pmix_check_package_$1_save_CPPFLAGS"
	LDFLAGS="$pmix_check_package_$1_save_LDFLAGS"
	LIBS="$pmix_check_package_$1_save_LIBS"

	if test "$pmix_check_tm_found" = "no" ; then
	    pmix_check_tm_happy=no
	fi

	PMIX_SUMMARY_ADD([[Resource Managers]],[[Torque]],[$1],[$pmix_check_tm_happy])

	PMIX_VAR_SCOPE_POP
    fi

    # Did we find the right stuff?
    AS_IF([test "$pmix_check_tm_happy" = "yes"],
          [$1_LIBS="[$]$1_LIBS $pmix_check_tm_LIBS"
	       $1_LDFLAGS="[$]$1_LDFLAGS $pmix_check_tm_LDFLAGS"
	       $1_CPPFLAGS="[$]$1_CPPFLAGS $pmix_check_tm_CPPFLAGS"
	       # add the TM libraries to static builds as they are required
	       $1_WRAPPER_EXTRA_LDFLAGS=[$]$1_LDFLAGS
	       $1_WRAPPER_EXTRA_LIBS=[$]$1_LIBS
	       $2],
          [AS_IF([test ! -z "$with_tm" && test "$with_tm" != "no"],
                 [AC_MSG_ERROR([TM support requested but not found.  Aborting])])
	       pmix_check_tm_happy="no"
	       $3])
])
