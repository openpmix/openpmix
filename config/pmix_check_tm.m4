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
dnl Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
dnl Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
dnl                         All Rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PMIX_CHECK_TM_PBS_CONFIG_RUN([pbs-config args], [assignment variable],
#                              [action-if-successful], [action-if-not-successful])
# --------------------------------------------------------------------------------
AC_DEFUN([PMIX_CHECK_TM_PBS_CONFIG_RUN], [
    # bozo check
    AS_IF([test -z "${pmix_check_tm_cv_pbs_config_path}"],
          [AC_MSG_ERROR([Internal error.  pbs-config not properly configured.])])

    PMIX_LOG_COMMAND([pmix_check_tm_pbs_config_run_results=`${pmix_check_tm_cv_pbs_config_path} $1 2>&1`],
                     [AS_VAR_COPY([$2], [pmix_check_tm_pbs_config_run_results])
                      $3], [$4])
    AS_UNSET([pmix_check_tm_pbs_config_run_results])
])


# PMIX_CHECK_TM_LIBS_FLAGS([flags-name-prefix], [input-flags])
# ------------------------------------------------------------
AC_DEFUN([PMIX_CHECK_TM_SPLIT_LIBS_OUTPUT], [
    for pmix_check_tm_val in $2; do
        AS_IF([test "`echo $pmix_check_tm_val | cut -c1-2`" = "-l"],
              [PMIX_APPEND([$1_LIBS], [${pmix_check_tm_val}])],
              [PMIX_APPEND([$1_LDFLAGS], [${pmix_check_tm_val}])])
    done
    AS_UNSET([pmix_check_tm_val])
])


# PMIX_CHECK_TM_PBS_CONFIG(prefix, [action-if-found], [action-if-not-found],
#                          [action-if-found-but-does-not-work])
# --------------------------------------------------------------------------
AC_DEFUN([PMIX_CHECK_TM_PBS_CONFIG], [
    PMIX_VAR_SCOPE_PUSH([pbs_config_happy])

    pbs_config_happy="yes"

    AC_CACHE_CHECK([for pbs-config path],
                   [pmix_check_tm_cv_pbs_config_path],
                   [AS_IF([test -z "${with_tm}" -o "${with_tm}" = "yes"],
                          [pmix_check_tm_cv_pbs_config_path="pbs-config"],
                          [pmix_check_tm_cv_pbs_config_path="${with_tm}/bin/pbs-config"])])

    AC_CACHE_CHECK([if pbs-config works],
                   [pmix_check_tm_cv_pbs_config_works],
                   [PMIX_CHECK_TM_PBS_CONFIG_RUN([--prefix], [pmix_check_tm_dummy],
                                                 [pmix_check_tm_cv_pbs_config_works="yes"],
                                                 [pmix_check_tm_cv_pbs_config_works="no"])])
    AS_IF([test "${pmix_check_tm_cv_pbs_config_works}" = "no"], [pbs_config_happy="no"])

    AS_IF([test "${pbs_config_happy}" = "yes"],
          [AC_CACHE_CHECK([for pbs-config cflags],
              [pmix_check_tm_cv_pbs_config_cflags_output],
              [PMIX_CHECK_TM_PBS_CONFIG_RUN([--cflags], [pmix_check_tm_cv_pbs_config_cflags_output], [],
                   [AC_MSG_ERROR([An error occurred retrieving cflags from pbs-config])])])

           AC_CACHE_CHECK([for pbs-config libs],
              [pmix_check_tm_cv_pbs_config_libs_output],
              [PMIX_CHECK_TM_PBS_CONFIG_RUN([--libs], [pmix_check_tm_cv_pbs_config_libs_output], [],
                   [AC_MSG_ERROR([An error occurred retrieving libs from pbs-config])])])

           $1_CPPFLAGS="${pmix_check_tm_cv_pbs_config_cflags_output}"
           PMIX_CHECK_TM_SPLIT_LIBS_OUTPUT([$1], [${pmix_check_tm_cv_pbs_config_libs_output}])

           PMIX_LOG_MSG([pbs-config CPPFLAGS: ${$1_CPPFLAGS}], 1)
           PMIX_LOG_MSG([pbs-config LDFLAGS: ${$1_LDFLAGS}], 1)
           PMIX_LOG_MSG([pbs-config LIBS: ${$1_LIBS}], 1)

           # Now that we supposedly have the right flags, try them out.
           pmix_check_tm_CPPFLAGS_save="${CPPFLAGS}"
           pmix_check_tm_LDFLAGS_save="${LDFLAGS}"
           pmix_check_tm_LIBS_save="${LIBS}"

           CPPFLAGS="${CPPFLAGS} ${$1_CPPFLAGS}"
           LIBS="${LIBS} ${$1_LIBS}"
           LDFLAGS="${LDFLAGS} ${$1_LDFLAGS}"

           pbs_config_happy=no
           AC_CHECK_HEADER([tm.h],
               [AC_CHECK_FUNC([tm_finalize],
                              [pbs_config_happy="yes"])])

           CPPFLAGS="${pmix_check_tm_CPPFLAGS_save}"
           LDFLAGS="${pmix_check_tm_LDFLAGS_save}"
           LIBS="${pmix_check_tm_LIBS_save}"])

    AS_IF([test "${pbs_config_happy}" = "yes"],
          [$1_SUMMARY="yes (${pmix_check_tm_cv_pbs_config_path})"
           $2],
          [test "${pmix_check_tm_cv_pbs_config_works}" = "yes"],
          [$4],
          [$1_SUMMARY="no"
           $3])

    PMIX_VAR_SCOPE_POP
])


# PMIX_CHECK_TM(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([PMIX_CHECK_TM],[
    PMIX_VAR_SCOPE_PUSH([pmix_check_tm_happy pmix_check_tm_found])

    AC_ARG_WITH([tm],
                [AS_HELP_STRING([--with-tm(=DIR)],
                                [Build TM (Torque, PBSPro, and compatible) support, optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])])
    AC_ARG_WITH([tm-libdir],
                [AS_HELP_STRING([--with-tm-libdir=DIR],
                                [Search for Torque libraries in DIR])])

    AS_IF([test "${with_tm}" = "no"],
          [pmix_check_tm_happy="no"],
          [pmix_check_tm_happy="yes"])

    pmix_check_tm_found=0

    # Note: If we found pbs-config, got flags from it, but those flags don't work, consider that a hard fail
    # for a working TM.  Don't try to search with check package in that case.
    AS_IF([test "${pmix_check_tm_happy}" = "yes"],
          [PMIX_CHECK_TM_PBS_CONFIG([$1], [pmix_check_tm_found=1], [], [pmix_check_tm_happy="no"])])

    # Note that Torque 2.1.0 changed the name of their back-end
    # library to "libtorque".  So we have to check for both libpbs and
    # libtorque.  First, check for libpbs.
    AS_IF([test "${pmix_check_tm_happy}" = "yes" -a ${pmix_check_tm_found} -eq 0],
          [AS_VAR_SET_IF([pmix_cv_check_tm_libs],
              [OAC_CHECK_PACKAGE([tm],
                                 [$1],
                                 [tm.h],
                                 [${pmix_cv_check_tm_libs}],
                                 [tm_init],
                                 [pmix_check_tm_found=1])],
              [OAC_CHECK_PACKAGE([tm],
                                 [$1],
                                 [tm.h],
                                 [pbs crypto z],
                                 [tm_init],
                                 [pmix_cv_check_tm_libs="pbs crypto z"
                                  pmix_check_tm_found=1])])
               AS_IF([test ${pmix_check_tm_found} -eq 0],
                     [OAC_CHECK_PACKAGE_INVALIDATE_GENERIC_CACHE([tm], [$1], [tm_init])
                      OAC_CHECK_PACKAGE([tm],
                                        [$1],
                                        [tm.h],
                                        [pbs crypto z],
                                        [tm_init],
                                        [pmix_cv_check_tm_libs="pbs crypto z"
                                         pmix_check_tm_found=1])])
               AS_IF([test ${pmix_check_tm_found} -eq 0],
                     [OAC_CHECK_PACKAGE_INVALIDATE_GENERIC_CACHE([tm], [$1], [tm_init])
                      OAC_CHECK_PACKAGE([tm],
                                        [$1],
                                        [tm.h],
                                        [torque],
                                        [tm_init],
                                        [pmix_cv_check_tm_libs="torque"
                                         pmix_check_tm_found=1])])])

    AS_IF([test ${pmix_check_tm_found} -eq 0], [pmix_check_tm_happy="no"])


    # Did we find the right stuff?
    AS_IF([test "${pmix_check_tm_happy}" = "yes"],
          [$2],
          [AS_IF([test ! -z "${with_tm}" && test "${with_tm}" != "no"],
                 [AC_MSG_ERROR([TM support requested but not found.  Aborting])])
           $3])

    PMIX_SUMMARY_ADD([Resource Managers], [Torque], [], [${$1_SUMMARY}])

    PMIX_VAR_SCOPE_POP
])
