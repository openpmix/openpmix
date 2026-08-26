/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "pmix_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

#include "pmix.h"

#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/util/pmix_parse_options.h"

/*
 * Read one whole token as an int.
 *
 * strtol() answers 0 for a string with no digits in it and saturates at
 * LONG_MAX/LONG_MIN for one that is too big, and neither of those is
 * distinguishable from a real value once it has been narrowed to an
 * int - "4294967295" and "99999999999999999999" both narrow to -1,
 * which this file reads as the wildcard meaning "every value", so a
 * mistyped port range silently discarded the caller's whole list and
 * replaced it with "any". Insist on digits, on nothing after them, and
 * on a value an int can actually hold.
 */
static bool parse_whole_int(const char *str, int *val)
{
    char *endptr;
    long tmp;

    if (NULL == str || '\0' == *str) {
        return false;
    }
    errno = 0;
    tmp = strtol(str, &endptr, 10);
    if (endptr == str || 0 != errno || tmp < INT_MIN || tmp > INT_MAX) {
        return false;
    }
    /* strtol skips leading whitespace on its own; allow trailing
     * whitespace too, so a list written as "1, 3" keeps working */
    while (isspace((unsigned char) *endptr)) {
        ++endptr;
    }
    if ('\0' != *endptr) {
        return false;
    }
    *val = (int) tmp;
    return true;
}

void pmix_util_parse_range_options(char *inp, char ***output)
{
    char **r1 = NULL, **r2 = NULL;
    int i, vint;
    int start, end, n;
    char nstr[32];
    char *input, *bang;
    bool bang_option = false;

    /* protect against null input */
    if (NULL == inp) {
        return;
    }

    /* protect the provided input */
    input = strdup(inp);
    if (NULL == input) {
        return;
    }

    /* check for the special '!' operator */
    if (NULL != (bang = strchr(input, '!'))) {
        bang_option = true;
        *bang = '\0';
    }

    /* split on commas */
    r1 = PMIx_Argv_split(input, ',');
    /* for each resulting element, check for range */
    for (i = 0; i < PMIx_Argv_count(r1); i++) {
        r2 = PMIx_Argv_split(r1[i], '-');
        if (1 < PMIx_Argv_count(r2)) {
            /* given range - get start and end */
            if (!parse_whole_int(r2[0], &start) || !parse_whole_int(r2[1], &end)) {
                pmix_output(0, "Unknown parse error on string: %s(%s)", inp, r1[i]);
                PMIx_Argv_free(r2);
                continue;
            }
        } else {
            /* check for wildcard - have to do this here because
             * the -1 would have been caught in the split
             */
            if (parse_whole_int(r1[i], &vint) && -1 == vint) {
                PMIx_Argv_free(*output);
                *output = NULL;
                PMIx_Argv_append_nosize(output, "-1");
                PMIx_Argv_free(r2);
                goto cleanup;
            }
            /* a token like "-" (or "--") splits to all-empty tokens, so
             * PMIx_Argv_split returns NULL - guard against dereferencing
             * r2[0] in that case rather than crashing */
            if (0 == PMIx_Argv_count(r2)) {
                PMIx_Argv_free(r2);
                continue;
            }
            if (!parse_whole_int(r2[0], &start)) {
                pmix_output(0, "Unknown parse error on string: %s(%s)", inp, r1[i]);
                PMIx_Argv_free(r2);
                continue;
            }
            end = start;
        }
        for (n = start; n <= end; n++) {
            pmix_snprintf(nstr, 32, "%d", n);
            PMIx_Argv_append_nosize(output, nstr);
        }
        PMIx_Argv_free(r2);
    }

cleanup:
    if (bang_option) {
        PMIx_Argv_append_nosize(output, "BANG");
    }
    free(input);
    PMIx_Argv_free(r1);
}

void pmix_util_get_ranges(char *inp, char ***startpts, char ***endpts)
{
    char **r1 = NULL, **r2 = NULL;
    int i;
    char *input;

    /* protect against null input */
    if (NULL == inp) {
        return;
    }

    /* protect the provided input */
    input = strdup(inp);
    if (NULL == input) {
        return;
    }

    /* split on commas */
    r1 = PMIx_Argv_split(input, ',');
    /* for each resulting element, check for range */
    for (i = 0; i < PMIx_Argv_count(r1); i++) {
        r2 = PMIx_Argv_split(r1[i], '-');
        if (2 == PMIx_Argv_count(r2)) {
            /* given range - get start and end */
            PMIx_Argv_append_nosize(startpts, r2[0]);
            PMIx_Argv_append_nosize(endpts, r2[1]);
        } else if (1 == PMIx_Argv_count(r2)) {
            /* only one value provided, so it is both the start
             * and the end
             */
            PMIx_Argv_append_nosize(startpts, r2[0]);
            PMIx_Argv_append_nosize(endpts, r2[0]);
        } else {
            /* no idea how to parse this */
            pmix_output(0, "Unknown parse error on string: %s(%s)", inp, r1[i]);
        }
        PMIx_Argv_free(r2);
    }

    free(input);
    PMIx_Argv_free(r1);
}
