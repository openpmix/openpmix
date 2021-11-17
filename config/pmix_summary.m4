dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2016      Los Alamos National Security, LLC. All rights
dnl                         reserved.
dnl Copyright (c) 2016-2018 Cisco Systems, Inc.  All rights reserved
dnl Copyright (c) 2016      Research Organization for Information Science
dnl                         and Technology (RIST). All rights reserved.
dnl Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
dnl Copyright (c) 2021      IBM Corporation.  All rights reserved.
dnl Copyright (c) 2021      Nanook Consulting.  All rights reserved.
dnl $COPYRIGHT$
dnl
dnl Additional copyrights may follow
dnl
dnl $HEADER$
dnl
AC_DEFUN([PMIX_SUMMARY_ADD],[
    PMIX_VAR_SCOPE_PUSH([pmix_summary_section pmix_summary_line pmix_summary_section_current])

    dnl need to replace spaces in the section name with somethis else. _ seems like a reasonable
    dnl choice. if this changes remember to change PMIX_PRINT_SUMMARY as well.
    pmix_summary_section=$(echo $1 | tr ' ' '_')
    pmix_summary_line="$2: $4"
    pmix_summary_section_current=$(eval echo \$pmix_summary_values_$pmix_summary_section)

    if test -z "$pmix_summary_section_current" ; then
        if test -z "$pmix_summary_sections" ; then
            pmix_summary_sections=$pmix_summary_section
        else
            pmix_summary_sections="$pmix_summary_sections $pmix_summary_section"
        fi
        eval pmix_summary_values_$pmix_summary_section=\"$pmix_summary_line\"
    else
        eval pmix_summary_values_$pmix_summary_section=\"$pmix_summary_section_current,$pmix_summary_line\"
    fi

    PMIX_VAR_SCOPE_POP
])

AC_DEFUN([PMIX_SUMMARY_PRINT],[
    PMIX_VAR_SCOPE_PUSH([pmix_summary_section pmix_summary_section_name])
    cat <<EOF >&2

PMIx configuration:
-----------------------
Version: $PMIX_MAJOR_VERSION.$PMIX_MINOR_VERSION.$PMIX_RELEASE_VERSION$PMIX_GREEK_VERSION
PMIx Standard Version: $PMIX_STD_VERSION
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

    for pmix_summary_section in $(echo $pmix_summary_sections) ; do
        pmix_summary_section_name=$(echo $pmix_summary_section | tr '_' ' ')
        echo "$pmix_summary_section_name" >&2
        echo "-----------------------" >&2
        echo "$(eval echo \$pmix_summary_values_$pmix_summary_section)" | tr ',' $'\n' | sort -f >&2
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
