/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for PMIX_IOF_FILE_PATTERN name expansion:
 * pmix_iof_check_pattern() and pmix_iof_expand_pattern().
 *
 * These two entry points parse a pattern supplied by the user on a launcher
 * command line, so they must agree with each other and with their documented
 * contract in src/common/pmix_iof.h:
 *   - the recognized conversions (%n, %r, %R, %h, %%) expand as advertised,
 *     and the stream suffix is always appended;
 *   - anything else after a '%' - including a '%' at the end of the string -
 *     is rejected, and the check reports the offending conversion so a
 *     launcher can tell the user what to fix;
 *   - the check and the expansion share one walker, so a pattern accepted by
 *     the check can never be rejected when the file is actually opened. That
 *     invariant is what keeps a bad pattern from turning into a failed job
 *     rather than a command-line diagnostic, so it is asserted directly.
 *
 * The walker touches no library state other than pmix_globals.hostname (for
 * %h), so these tests run without initializing PMIx.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fixed expansion inputs used by every case below */
#define MY_NSPACE  "myns"
#define MY_RANK    3
#define MY_NUMDIGS 4
#define MY_HOST    "node07"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* a NULL expectation means "the pattern is rejected" */
typedef struct {
    const char *pattern;
    const char *suffix;
    const char *expected; /* expanded result, or NULL if rejected */
    const char *bad;      /* conversion the check must name, if rejected */
} pattern_test_t;

static pattern_test_t tests[] = {
    /* a pattern with no conversions at all is simply a fixed name */
    {"myfile", ".out", "myfile.out", NULL},
    {"", ".out", ".out", NULL},
    /* every recognized conversion, alone */
    {"%n", ".out", MY_NSPACE ".out", NULL},
    {"%r", ".out", "3.out", NULL},
    {"%R", ".out", "0003.out", NULL},
    {"%h", ".out", MY_HOST ".out", NULL},
    {"%%", ".out", "%.out", NULL},
    /* ... and all of them together, with literal text interleaved */
    {"%n-%r-%R-%h-%%done", ".err", MY_NSPACE "-3-0003-" MY_HOST "-%done.err", NULL},
    /* conversions are legal in the directory part of the name */
    {"%h/rank-%R", ".out", MY_HOST "/rank-0003.out", NULL},
    /* adjacent conversions, no separator */
    {"%n%r", ".out", MY_NSPACE "3.out", NULL},
    /* a doubled '%' must not be mistaken for the start of a conversion */
    {"%%n", ".out", "%n.out", NULL},
    /* no suffix at all is legal - the walker leaves the name alone */
    {"%n.%r", NULL, MY_NSPACE ".3", NULL},
    /* unknown conversions are rejected, and named */
    {"%q", ".out", NULL, "%q"},
    {"%n.%q", ".out", NULL, "%q"},
    {"out%zfile", ".out", NULL, "%z"},
    /* a '%' at the very end has no conversion character to report; the
     * walker must say so rather than read past the terminator */
    {"%", ".out", NULL, "%"},
    {"myfile%", ".out", NULL, "%"},
    /* case matters: %N and %H are not %n and %h */
    {"%N", ".out", NULL, "%N"},
    {"%H", ".out", NULL, "%H"},
};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    size_t n;
    char *result, *bad;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* %h reports the local hostname; the walker reads it straight out of
     * pmix_globals, so plant a known value rather than depending on the
     * machine the test happens to run on */
    pmix_globals.hostname = strdup(MY_HOST);

    fprintf(stdout, "\n=== IOF output-file pattern unit tests ===\n\n");

    for (n = 0; n < sizeof(tests) / sizeof(tests[0]); n++) {
        pattern_test_t *t = &tests[n];
        char label[256];

        /* the expansion produces exactly the documented name */
        result = NULL;
        rc = pmix_iof_expand_pattern(t->pattern, MY_NSPACE, MY_RANK, MY_NUMDIGS,
                                     t->suffix, &result);
        snprintf(label, sizeof(label), "expand \"%s\" -> %s", t->pattern,
                 (NULL == t->expected) ? "rejected" : t->expected);
        if (NULL == t->expected) {
            /* a rejected pattern must not hand back a name to open */
            report(label, PMIX_ERR_BAD_PARAM == rc && NULL == result);
        } else {
            report(label, PMIX_SUCCESS == rc && NULL != result
                              && 0 == strcmp(result, t->expected));
        }
        if (NULL != result) {
            free(result);
        }

        /* the check agrees with the expansion - the shared-walker invariant */
        bad = NULL;
        rc = pmix_iof_check_pattern(t->pattern, &bad);
        snprintf(label, sizeof(label), "check \"%s\" agrees with expand", t->pattern);
        report(label, (NULL == t->expected) ? (PMIX_ERR_BAD_PARAM == rc)
                                            : (PMIX_SUCCESS == rc));

        /* and names the offending conversion for the diagnostic */
        snprintf(label, sizeof(label), "check \"%s\" reports bad=%s", t->pattern,
                 (NULL == t->bad) ? "(none)" : t->bad);
        if (NULL == t->bad) {
            report(label, NULL == bad);
        } else {
            report(label, NULL != bad && 0 == strcmp(bad, t->bad));
        }
        if (NULL != bad) {
            free(bad);
        }
    }

    /* a NULL pattern is a bad parameter, not a crash, in both directions */
    result = NULL;
    rc = pmix_iof_expand_pattern(NULL, MY_NSPACE, MY_RANK, MY_NUMDIGS, ".out", &result);
    report("expand rejects a NULL pattern", PMIX_ERR_BAD_PARAM == rc && NULL == result);

    bad = (char *) 1; /* must be overwritten, even on the error path */
    rc = pmix_iof_check_pattern(NULL, &bad);
    report("check rejects a NULL pattern", PMIX_ERR_BAD_PARAM == rc && NULL == bad);

    /* expansion has nowhere to put a name without a result pointer, and must
     * say so rather than dereference it */
    rc = pmix_iof_expand_pattern("%n.%r", MY_NSPACE, MY_RANK, MY_NUMDIGS, ".out", NULL);
    report("expand rejects a NULL result", PMIX_ERR_BAD_PARAM == rc);

    /* the check has no name to hand back, so it must tolerate no bad ptr */
    rc = pmix_iof_check_pattern("%q", NULL);
    report("check tolerates a NULL bad ptr", PMIX_ERR_BAD_PARAM == rc);

    /* an absent namespace expands to nothing rather than dereferencing NULL */
    result = NULL;
    rc = pmix_iof_expand_pattern("%n%r", NULL, MY_RANK, MY_NUMDIGS, ".out", &result);
    report("expand tolerates a NULL nspace",
           PMIX_SUCCESS == rc && NULL != result && 0 == strcmp(result, "3.out"));
    if (NULL != result) {
        free(result);
    }

    /* %R with no padding width behaves like %r */
    result = NULL;
    rc = pmix_iof_expand_pattern("%R", MY_NSPACE, MY_RANK, 0, ".out", &result);
    report("expand pads %R to the requested width only",
           PMIX_SUCCESS == rc && NULL != result && 0 == strcmp(result, "3.out"));
    if (NULL != result) {
        free(result);
    }

    free(pmix_globals.hostname);
    pmix_globals.hostname = NULL;

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
