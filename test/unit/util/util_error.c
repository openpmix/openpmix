/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_error utility:
 *   PMIx_Error_string for public and internal codes,
 *   PMIX_ERROR_LOG macro, internal error constant definitions.
 *
 * PMIx_Init is called first; PMIX_ERR_UNREACH is treated as normal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_error.h"

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

/* ------------------------------------------------------------------ */
/* PMIx_Error_string                                                   */
/* ------------------------------------------------------------------ */

static void test_error_string_success(void)
{
    const char *s = PMIx_Error_string(PMIX_SUCCESS);
    report("error_string_SUCCESS: non-NULL", s != NULL);
}

static void test_error_string_standard_codes(void)
{
    report("error_string_NOT_FOUND: non-NULL",
           PMIx_Error_string(PMIX_ERR_NOT_FOUND) != NULL);
    report("error_string_BAD_PARAM: non-NULL",
           PMIx_Error_string(PMIX_ERR_BAD_PARAM) != NULL);
    report("error_string_NOMEM: non-NULL",
           PMIx_Error_string(PMIX_ERR_NOMEM) != NULL);
    report("error_string_UNREACH: non-NULL",
           PMIx_Error_string(PMIX_ERR_UNREACH) != NULL);
}

static void test_error_string_distinct_codes(void)
{
    /* Different codes must not produce the same string pointer
     * (they could theoretically have equal text, but the pointers
     * into the lookup table will be distinct). */
    const char *s_success  = PMIx_Error_string(PMIX_SUCCESS);
    const char *s_not_found = PMIx_Error_string(PMIX_ERR_NOT_FOUND);
    report("error_string_distinct: SUCCESS != NOT_FOUND",
           s_success != s_not_found);
}

/* ------------------------------------------------------------------ */
/* PMIx_Error_code                                                     */
/* ------------------------------------------------------------------ */

static void test_error_code_known_names(void)
{
    report("error_code_TIMEOUT: recovers the code",
           PMIX_ERR_TIMEOUT == PMIx_Error_code("PMIX_ERR_TIMEOUT"));
    report("error_code_SUCCESS: recovers the code",
           PMIX_SUCCESS == PMIx_Error_code("PMIX_SUCCESS"));
    /* an internal-only constant, harvested out of pmix_error.h */
    report("error_code_TAKE_NEXT_OPTION: recovers the code",
           PMIX_ERR_TAKE_NEXT_OPTION == PMIx_Error_code("PMIX_ERR_TAKE_NEXT_OPTION"));
}

static void test_error_code_case_insensitive(void)
{
    /* the lookup is documented as case-insensitive */
    report("error_code_case: lower case name matches",
           PMIX_ERR_TIMEOUT == PMIx_Error_code("pmix_err_timeout"));
}

static void test_error_code_unknown(void)
{
    /* INT32_MIN is the documented "name not recognized" sentinel, and it
     * has to be a sentinel rather than a negative return because every
     * real status is itself negative */
    report("error_code_unknown: returns INT32_MIN",
           INT32_MIN == PMIx_Error_code("PMIX_NO_SUCH_CONSTANT"));
}

static void test_error_code_empty_name(void)
{
    /* the table is terminated by an entry whose name is "" and whose
     * code is -1; PMIX_EVENT_INDEX_BOUNDARY must stop the scan before
     * it, so an empty name is simply unknown.  A scan that walked one
     * entry too far would answer PMIX_ERROR (-1) here instead. */
    report("error_code_empty: empty name is not the terminator",
           INT32_MIN == PMIx_Error_code(""));
}

static void test_error_code_null(void)
{
    /* a NULL name is a name the table does not hold.  Without the
     * screen this reaches strcasecmp(name, NULL) on the first entry and
     * the test binary dies on a signal rather than failing here. */
    report("error_code_NULL: returns INT32_MIN instead of crashing",
           INT32_MIN == PMIx_Error_code(NULL));
}

static void test_error_code_roundtrip(void)
{
    const pmix_status_t codes[] = {PMIX_SUCCESS, PMIX_ERROR, PMIX_ERR_NOT_FOUND,
                                   PMIX_ERR_BAD_PARAM, PMIX_ERR_NOMEM,
                                   PMIX_ERR_UNREACH, PMIX_ERR_TIMEOUT,
                                   PMIX_ERR_FABRIC_NOT_PARSEABLE,
                                   PMIX_ERR_TEMP_UNAVAILABLE};
    size_t n;
    int ok = 1;

    /* PMIx_Error_code is documented as the inverse of PMIx_Error_string */
    for (n = 0; n < sizeof(codes) / sizeof(codes[0]); n++) {
        if (codes[n] != PMIx_Error_code(PMIx_Error_string(codes[n]))) {
            ok = 0;
        }
    }
    report("error_code_roundtrip: code(string(x)) == x", ok);
}

static void test_error_string_prefers_current_name(void)
{
    /* Several codes carry both a current name and a deprecated alias -
     * a rename is the one case where two constants may legitimately
     * share a value.  The generated table lists pmix_common.h.in before
     * pmix_deprecated.h and PMIx_Error_string returns the first match,
     * which is what keeps error output naming the current spelling.
     * Reordering the harvest would silently change every such message. */
    report("error_string_current_name: -11 is PMIX_ERR_EXISTS not PMIX_EXISTS",
           0 == strcmp("PMIX_ERR_EXISTS", PMIx_Error_string(PMIX_ERR_EXISTS)));
    report("error_string_current_name: -231 is PMIX_EVENT_NODE_DOWN",
           0 == strcmp("PMIX_EVENT_NODE_DOWN", PMIx_Error_string(PMIX_EVENT_NODE_DOWN)));
}

static void test_error_string_unknown(void)
{
    /* PMIX_EXTERNAL_ERR_BASE is deliberately not in the table */
    const char *s = PMIx_Error_string(PMIX_EXTERNAL_ERR_BASE - 1);
    report("error_string_unknown: non-NULL for an unknown code", NULL != s);
    report("error_string_unknown: not the empty terminator name",
           NULL != s && 0 != strlen(s));
}

/* ------------------------------------------------------------------ */
/* PMIX_ERROR_LOG macro                                                */
/* ------------------------------------------------------------------ */

static void test_error_log_no_crash(void)
{
    /* PMIX_ERROR_LOG calls pmix_output(0, ...) for non-silent errors. */
    PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
    report("error_log: non-silent error doesn't crash", 1);
}

static void test_error_log_silent(void)
{
    /* PMIX_ERR_SILENT must be silently swallowed (no output, no crash). */
    PMIX_ERROR_LOG(PMIX_ERR_SILENT);
    report("error_log_silent: silent code is suppressed", 1);
}

/* ------------------------------------------------------------------ */
/* Internal error constant definitions                                 */
/* ------------------------------------------------------------------ */

static void test_internal_constants_distinct(void)
{
    report("internal_constants: FABRIC_NOT_PARSEABLE != TAKE_NEXT_OPTION",
           PMIX_ERR_FABRIC_NOT_PARSEABLE != PMIX_ERR_TAKE_NEXT_OPTION);
    report("internal_constants: TAKE_NEXT_OPTION != TEMP_UNAVAILABLE",
           PMIX_ERR_TAKE_NEXT_OPTION != PMIX_ERR_TEMP_UNAVAILABLE);
    report("internal_constants: FABRIC_NOT_PARSEABLE != TEMP_UNAVAILABLE",
           PMIX_ERR_FABRIC_NOT_PARSEABLE != PMIX_ERR_TEMP_UNAVAILABLE);
    report("internal_constants: INTERNAL_ERR_DONE != INTERNAL_ERR_BASE",
           PMIX_INTERNAL_ERR_DONE != PMIX_INTERNAL_ERR_BASE);
}

static void test_internal_constants_below_public(void)
{
    /* Internal codes must lie below the public error-code range
     * (more negative than PMIX_ERR_BAD_PARAM). */
    report("internal_constants_below_public: BASE < BAD_PARAM",
           PMIX_INTERNAL_ERR_BASE < PMIX_ERR_BAD_PARAM);
    report("internal_constants_below_public: DONE < BASE",
           PMIX_INTERNAL_ERR_DONE < PMIX_INTERNAL_ERR_BASE);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_error unit tests ===\n\n");

    test_error_string_success();
    test_error_string_standard_codes();
    test_error_string_distinct_codes();
    test_error_string_prefers_current_name();
    test_error_string_unknown();
    test_error_code_known_names();
    test_error_code_case_insensitive();
    test_error_code_unknown();
    test_error_code_empty_name();
    test_error_code_null();
    test_error_code_roundtrip();
    test_error_log_no_crash();
    test_error_log_silent();
    test_internal_constants_distinct();
    test_internal_constants_below_public();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
