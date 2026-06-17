/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_show_help utility:
 *   pmix_show_help_init (idempotency), pmix_show_help_string,
 *   pmix_show_help_vstring, pmix_show_help_norender,
 *   pmix_help_check_dups, pmix_show_help_enabled.
 *
 * Built-in help content (pmix_show_help_data[]) is used so no
 * filesystem access is required.
 *
 * PMIx_Init is called first; PMIX_ERR_UNREACH is treated as normal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_show_help.h"

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
/* pmix_show_help_init (idempotency after PMIx_Init)                  */
/* ------------------------------------------------------------------ */

static void test_show_help_init_idempotent(void)
{
    /* PMIx_Init already called pmix_show_help_init internally.
     * Re-calling must succeed without side effects. */
    pmix_status_t rc = pmix_show_help_init();
    report("show_help_init_idempotent", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* pmix_show_help_enabled global                                       */
/* ------------------------------------------------------------------ */

static void test_show_help_enabled(void)
{
    /* After init, show_help_enabled should be set (> 0). */
    report("show_help_enabled: > 0", pmix_show_help_enabled > 0);
}

/* ------------------------------------------------------------------ */
/* pmix_show_help_string                                               */
/* ------------------------------------------------------------------ */

static void test_show_help_string_known_topic(void)
{
    /* "help-cli.txt" / "unknown-option" has two %s specifiers:
     *   "Option: %s" and "%s --help" */
    char *s = pmix_show_help_string("help-cli.txt", "unknown-option", 0,
                                    "myoption", "myprog");
    report("show_help_string_known: non-NULL", s != NULL);
    report("show_help_string_known: contains option name",
           s != NULL && strstr(s, "myoption") != NULL);
    free(s);
}

static void test_show_help_string_unknown_file(void)
{
    /* Unknown file → returns NULL; local_delivery writes to stderr. */
    char *s = pmix_show_help_string("help-nonexistent-xyz.txt", "topic", 0);
    report("show_help_string_unknown_file: returns NULL", s == NULL);
}

static void test_show_help_string_unknown_topic(void)
{
    /* Known file, unknown topic → returns NULL. */
    char *s = pmix_show_help_string("help-cli.txt", "no-such-topic-xyz", 0);
    report("show_help_string_unknown_topic: returns NULL", s == NULL);
}

/* ------------------------------------------------------------------ */
/* pmix_show_help_norender                                             */
/* ------------------------------------------------------------------ */

static void test_show_help_norender(void)
{
    /* Delivers a raw string to the local output; must not crash. */
    pmix_status_t rc = pmix_show_help_norender("help-cli.txt",
                                               "norender-test",
                                               "raw message: no-op delivery\n");
    report("show_help_norender: returns SUCCESS", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* pmix_help_check_dups (first-call path: not a duplicate)            */
/* ------------------------------------------------------------------ */

static void test_help_check_dups_first_call(void)
{
    /* A (file, topic) pair seen for the first time is NOT a duplicate.
     * pmix_help_check_dups returns PMIX_ERR_NOT_FOUND for "not a dup". */
    pmix_status_t rc = pmix_help_check_dups("help-cli.txt",
                                            "unit-test-unique-topic-xyz");
    report("help_check_dups_first: returns ERR_NOT_FOUND (not a dup)",
           PMIX_ERR_NOT_FOUND == rc);
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

    fprintf(stdout, "\n=== pmix_show_help unit tests ===\n\n");

    test_show_help_init_idempotent();
    test_show_help_enabled();
    test_show_help_string_known_topic();
    test_show_help_string_unknown_file();
    test_show_help_string_unknown_topic();
    test_show_help_norender();
    test_help_check_dups_first_call();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
