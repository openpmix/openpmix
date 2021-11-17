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
# Copyright (c) 2012-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012      Oracle and/or its affiliates.  All rights reserved.
# Copyright (c) 2014-2018 Intel, Inc. All rights reserved.
# Copyright (c) 2017-2021 Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# _PMIX_CHECK_PACKAGE_HEADER(prefix, header, dir-prefix,
#                            [action-if-found], [action-if-not-found],
#                            includes)
# --------------------------------------------------------------------
AC_DEFUN([_PMIX_CHECK_PACKAGE_HEADER], [
    # This is stolen from autoconf to peek under the covers to get the
    # cache variable for the library check.  one should not copy this
    # code into other places unless you want much pain and suffering
    AS_VAR_PUSHDEF([pmix_Header], [ac_cv_header_$2])

    # so this sucks, but there's no way to get through the progression
    # of header includes without killing off the cache variable and trying
    # again...
    unset pmix_Header
    pmix_check_package_header_happy="no"

    # get rid of the trailing slash(es)
    hdir_prefix=$(echo $3 | sed -e 'sX/*$XXg')

    # We cannot just blindly use AC_CHECK_HEADERS as it will
    # _always_ include the standard locations, even it we try
    # to restrain it by setting CPPFLAGS to point to only our
    # specified location.  Thus, if we use that macro we can
    # (and often will) return 'success' by finding the header
    # in a standard location without ever checking the specified
    # location, leading us down a bad path
    #
    # Instead, we start by simply checking for existence of
    # the specified file as that is the best we can do

    if test "$hdir_prefix" != ""; then
        AC_MSG_CHECKING([for header $2 in $hdir_prefix])
        if test -f $hdir_prefix/$2; then
            AC_MSG_RESULT([yes])
            $1_CPPFLAGS="-I$hdir_prefix"
            pmix_check_package_header_happy="yes"
        else
            AC_MSG_RESULT([no])
            AC_MSG_CHECKING([for header $2 in $hdir_prefix/include])
            if test -f $hdir_prefix/include/$2; then
                AC_MSG_RESULT([yes])
                $1_CPPFLAGS="-I$hdir_prefix/include"
                pmix_check_package_header_happy="yes"
            else
                AC_MSG_RESULT([no])
            fi
        fi
    else
        # try in standard paths
        AC_MSG_CHECKING([looking for header $2 in standard paths])
        AC_MSG_RESULT([])
        AC_CHECK_HEADERS([$2], [pmix_check_package_header_happy="yes"], [])
    fi

    AS_IF([test "$pmix_check_package_header_happy" = "yes"],
          [$4], [$5])

    unset pmix_check_package_header_happy
])


# _PMIX_CHECK_PACKAGE_LIB(prefix, library, function, extra-libraries,
#                         dir-prefix, libdir,
#                         [action-if-found], [action-if-not-found]])
# --------------------------------------------------------------------
AC_DEFUN([_PMIX_CHECK_PACKAGE_LIB], [
    # This is stolen from autoconf to peek under the covers to get the
    # cache variable for the library check.  one should not copy this
    # code into other places unless you want much pain and suffering
    AS_VAR_PUSHDEF([pmix_Lib], [ac_cv_search_$3])

    # see comment above
    unset pmix_Lib
    pmix_check_package_lib_happy="no"

    # get rid of the trailing slash(es)
    libdir_prefix=$(echo $6 | sed -e 'sX/*$XXg')

    if test "$libdir_prefix" != ""; then
        # libdir was specified - unfortunately, AC_SEARCH_LIBS
        # _always_ looks at the default location. You cannot
        # constrain it to just one place. So if the library
        # doesn't exist in the specified location, but does
        # exist in a standard location, then the test will
        # return "success" - and we will have the wrong lib.
        # Thus, we simply check to see if the library even
        # exists in the specified location - not bullet-proof,
        # but better than blindly getting the wrong answer.
        #
        # The vulnerability here is that the specified lib
        # might not be compilable in this environment, but
        # one in a standard location is. This test will pass
        # and set an LDFLAG to include the "bad" version in
        # the library path, possibly causing problems down
        # the road.
        #
        # No perfect solution

        AC_MSG_CHECKING([library $2 in $libdir_prefix])
        checkloc="`ls $libdir_prefix/lib$2.* >& /dev/null`"
        if test "$?" == "0"; then
            AC_MSG_RESULT([found])
            $1_LDFLAGS="-L$libdir_prefix"
            pmix_check_package_lib_happy=yes
            $1_LIBS="-l$2 $4"
        else
            AC_MSG_RESULT([not found])
            pmix_check_package_lib_happy=no
        fi

    else

        # try standard locations
        AC_MSG_CHECKING([looking for library without search path])
        AC_MSG_RESULT([])
        AC_SEARCH_LIBS([$3], [$2],
                       [pmix_check_package_lib_happy="yes"],
                       [pmix_check_package_lib_happy="no"], [$4])
        AS_IF([test "$ac_cv_search_$3" != "no" &&
               test "$ac_cv_search_$3" != "none required"],
              [$1_LIBS="$ac_cv_search_$3 $4"],
              [$1_LIBS="$4"])
    fi

    AS_IF([test "$pmix_check_package_lib_happy" = "yes"],
          [$7],
          [$8])

    AS_VAR_POPDEF([pmix_Lib])dnl
])


# PMIX_CHECK_PACKAGE(prefix,
#                    header,
#                    library,
#                    function,
#                    extra-libraries,
#                    dir-prefix,
#                    libdir-prefix,
#                    [action-if-found], [action-if-not-found],
#                    includes)
# -----------------------------------------------------------
# check for package defined by header and libs, and probably
# located in dir-prefix, possibly with libs in libdir-prefix.
# Both dir-prefix and libdir-prefix can be empty.  Will set
# prefix_{CPPFLAGS, LDFLAGS, LIBS} as needed
AC_DEFUN([PMIX_CHECK_PACKAGE],[
    pmix_check_package_$1_save_CPPFLAGS="$CPPFLAGS"
    pmix_check_package_$1_save_LDFLAGS="$LDFLAGS"
    pmix_check_package_$1_save_LIBS="$LIBS"

    pmix_check_package_$1_orig_CPPFLAGS="$$1_CPPFLAGS"
    pmix_check_package_$1_orig_LDFLAGS="$$1_LDFLAGS"
    pmix_check_package_$1_orig_LIBS="$$1_LIBS"

    _PMIX_CHECK_PACKAGE_HEADER([$1], [$2], [$6],
          [_PMIX_CHECK_PACKAGE_LIB([$1], [$3], [$4], [$5], [$6], [$7],
                [pmix_check_package_happy="yes"],
                [pmix_check_package_happy="no"])],
          [pmix_check_package_happy="no"],
          [$10])

    AS_IF([test "$pmix_check_package_happy" = "yes"],
          [$8],
          [$1_CPPFLAGS="$pmix_check_package_$1_orig_CPPFLAGS"
           $1_LDFLAGS="$pmix_check_package_$1_orig_LDFLAGS"
           $1_LIBS="$pmix_check_package_$1_orig_LIBS"
           $9])

    CPPFLAGS="$pmix_check_package_$1_save_CPPFLAGS"
    LDFLAGS="$pmix_check_package_$1_save_LDFLAGS"
    LIBS="$pmix_check_package_$1_save_LIBS"
])
