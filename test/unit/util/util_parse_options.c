/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_parse_options.c
 *   pmix_util_parse_range_options, pmix_util_get_ranges
 *
 * These are pure argv transformers - no server/tool init required.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"

#include "src/util/pmix_parse_options.h"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* Compare an argv against an expected NULL-terminated list. */
static int argv_equals(char **got, const char *const *exp)
{
    int i;
    int gcount = PMIx_Argv_count(got);
    int ecount = 0;
    while (NULL != exp[ecount]) {
        ecount++;
    }
    if (gcount != ecount) {
        return 0;
    }
    for (i = 0; i < gcount; i++) {
        if (0 != strcmp(got[i], exp[i])) {
            return 0;
        }
    }
    return 1;
}

static void check_range(const char *label, char *input,
                        const char *const *expected)
{
    char **out = NULL;
    pmix_util_parse_range_options(input, &out);
    report(label, argv_equals(out, expected));
    PMIx_Argv_free(out);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_parse_options unit tests ===\n\n");

    {
        const char *const exp[] = {"3", NULL};
        char in[] = "3";
        check_range("single value", in, exp);
    }
    {
        const char *const exp[] = {"3", "4", "5", NULL};
        char in[] = "3-5";
        check_range("range 3-5", in, exp);
    }
    {
        const char *const exp[] = {"1", "2", "3", "4", "7", NULL};
        char in[] = "1,2-4,7";
        check_range("mixed list and range", in, exp);
    }
    {
        const char *const exp[] = {"7", "BANG", NULL};
        char in[] = "7!";
        check_range("bang operator", in, exp);
    }
    {
        const char *const exp[] = {"-1", NULL};
        char in[] = "-1";
        check_range("wildcard -1", in, exp);
    }
    {
        /* reversed range yields nothing (documented behavior) */
        const char *const exp[] = {NULL};
        char in[] = "5-3";
        check_range("reversed range is empty", in, exp);
    }
    {
        /* Regression: a bare "-" token used to dereference a NULL argv
         * and crash. It must now be skipped without producing output. */
        const char *const exp[] = {NULL};
        char in[] = "-";
        check_range("bare dash does not crash", in, exp);
    }
    {
        /* Regression: an embedded bare "-" is skipped, neighbors kept. */
        const char *const exp[] = {"1", "3", NULL};
        char in[] = "1,-,3";
        check_range("embedded bare dash skipped", in, exp);
    }

    /* NULL input is a documented no-op */
    {
        char **out = NULL;
        pmix_util_parse_range_options(NULL, &out);
        report("NULL input leaves output NULL", NULL == out);
        PMIx_Argv_free(out);
    }

    /* pmix_util_get_ranges: parallel start/end arrays */
    {
        char **starts = NULL, **ends = NULL;
        const char *const exp_s[] = {"3", "7", NULL};
        const char *const exp_e[] = {"5", "7", NULL};
        char in[] = "3-5,7";
        pmix_util_get_ranges(in, &starts, &ends);
        report("get_ranges: start points", argv_equals(starts, exp_s));
        report("get_ranges: end points", argv_equals(ends, exp_e));
        PMIx_Argv_free(starts);
        PMIx_Argv_free(ends);
    }

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
