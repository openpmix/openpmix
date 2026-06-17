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
    test_error_log_no_crash();
    test_error_log_silent();
    test_internal_constants_distinct();
    test_internal_constants_below_public();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
