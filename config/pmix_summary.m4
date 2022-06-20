dnl -*- autoconf -*-
dnl
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2016-2018 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2016      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021-2022 IBM Corporation.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
dnl                         All Rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl

# PMIX_SUMMARY_ADD(section, topic, unused, result)
#
# queue a summary line in the given section of the form:
#   topic: result
#
# section:topic lines are only added once; first to add wins.
# The key for uniqification is a shell-variable-ified representation
# of section followed by an underscore followed by a
# shell-variable-ified representation of line.
#
# There are no restrictions on the contents of section and topic; they
# can be variable references (although the use case for this is
# dubious) and they can contain most ASCII characters (escape
# characters excluded).  Note that some care must be taken with the
# unique check and this liberal rule, as the unique check is after the
# string has been run through AS_TR_SH.  Basically, any character that
# is not legal in a shell variable name will be turned into an
# underscore.  So the strings "Section_foo" and "Section-foo" would be
# the same as far as the unique check is concerned.
#
# The input strings are evaluated during PMIX_SUMMARY_ADD, not during
# PMIX_SUMMARY_PRINT.  This seems to meet the principle of least
# astonishment.  A common pattern is to clal
# PMIX_SUMMARY_ADD([Resource Type], [Component Name], [], [$results])
# and then unset $results to avoid namespace pollution.  This will
# work properly with the current behavior, but would result in odd
# errors if we delayed evaluation.
#
# As a historical note, the third argument has never been used in
# PMIX_SUMMARY_ADD and its meaning is unclear.  Preferred behavior is
# to leave it empty.
#
# As a final historical note, the initial version of SUMMARY_ADD was
# added with implementation of the callers having all of the section
# and topic headers double quoted.  This was never necessary, and
# certainly is not with the current implementation.  While harmless,
# it is not great practice to over-quote, so we recommend against
# doing so.
# -----------------------------------------------------------
AC_DEFUN([PMIX_SUMMARY_ADD],[
    PMIX_VAR_SCOPE_PUSH([pmix_summary_line pmix_summary_newline pmix_summary_key])

    # The end quote on the next line is intentional!
    pmix_summary_newline="
"
    pmix_summary_line="$2: $4"
    pmix_summary_key="AS_TR_SH([$1])_AS_TR_SH([$2])"

    # Use the section name variable as an indicator for whether or not
    # the section has already been created.
    AS_IF([AS_VAR_TEST_SET([pmix_summary_section_]AS_TR_SH([$1])[_name])],
          [],
          [AS_VAR_SET([pmix_summary_section_]AS_TR_SH([$1])[_name], ["$1"])
           PMIX_APPEND([pmix_summary_sections], [AS_TR_SH([$1])])])

    # Use the summary key as indicator if the section:topic has already
    # been added to the results for the given section.
    AS_IF([AS_VAR_TEST_SET([${pmix_summary_key}])],
          [],
          [AS_VAR_SET([${pmix_summary_key}], [1])
           dnl this is a bit overcomplicated, but we are basically implementing
           dnl a poor mans AS_VAR_APPEND with the ability to specify a separator,
           dnl because we have a newline separator in the string.
           AS_IF([AS_VAR_TEST_SET([pmix_summary_section_]AS_TR_SH([$1])[_value])],
                 [AS_VAR_APPEND([pmix_summary_section_]AS_TR_SH([$1])[_value],
                                ["${pmix_summary_newline}${pmix_summary_line}"])],
                 [AS_VAR_SET([pmix_summary_section_]AS_TR_SH([$1])[_value],
                             ["${pmix_summary_line}"])])])

    PMIX_VAR_SCOPE_POP
])

AC_DEFUN([PMIX_SUMMARY_PRINT],[
    PMIX_VAR_SCOPE_PUSH([pmix_summary_section pmix_summary_section_name])
    cat <<EOF >&2

PMIx configuration:
-----------------------
Version: $PMIX_MAJOR_VERSION.$PMIX_MINOR_VERSION.$PMIX_RELEASE_VERSION$PMIX_GREEK_VERSION
PMIx Standard Version: $PMIX_STD_VERSION
PMIx Standard Stable ABI Version(s): $PMIX_STD_ABI_STABLE_VERSION
PMIx Standard Provisional ABI Version(s): $PMIX_STD_ABI_PROVISIONAL_VERSION
EOF

    if test $WANT_DEBUG = 0 ; then
        echo "Debug build: no" >&2
    else
        echo "Debug build: yes" >&2
    fi

    if test ! -z $with_pmix_platform ; then
        echo "Platform file: $with_pmix_platform" >&2
    else
        echo "Platform file: (none)" >&2
    fi

    echo >&2

    for pmix_summary_section in ${pmix_summary_sections} ; do
        AS_VAR_COPY([pmix_summary_section_name], [pmix_summary_section_${pmix_summary_section}_name])
        AS_VAR_COPY([pmix_summary_section_value], [pmix_summary_section_${pmix_summary_section}_value])
        echo "${pmix_summary_section_name}" >&2
        echo "-----------------------" >&2
        echo "${pmix_summary_section_value}" | sort -f >&2
        echo " " >&2
    done

    if test $WANT_DEBUG = 1 ; then
        cat <<EOF >&2
*****************************************************************************
 THIS IS A DEBUG BUILD!  DO NOT USE THIS BUILD FOR PERFORMANCE MEASUREMENTS!
*****************************************************************************

EOF
    fi

    PMIX_VAR_SCOPE_POP
])
