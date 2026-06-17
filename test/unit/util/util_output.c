/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_output stream facility:
 *   pmix_output_init, pmix_output_open, pmix_output_close,
 *   pmix_output_switch, pmix_output_set/get_verbosity,
 *   pmix_output_set_output_file_info.
 *
 * PMIx_Init is called first so the output subsystem is already up;
 * PMIX_ERR_UNREACH is treated as a normal no-server situation.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_output.h"

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
/* pmix_output_init (idempotency after PMIx_Init)                     */
/* ------------------------------------------------------------------ */

static void test_output_init_idempotent(void)
{
    /* PMIx_Init already called pmix_output_init internally.
     * Re-calling it must return true without side effects. */
    report("output_init_idempotent", pmix_output_init());
}

/* ------------------------------------------------------------------ */
/* pmix_output_open / pmix_output_close                               */
/* ------------------------------------------------------------------ */

static void test_output_open_null_lds(void)
{
    int h = pmix_output_open(NULL);
    report("output_open_null_lds: valid handle",
           h >= 0 && h < PMIX_OUTPUT_MAX_STREAMS);
    pmix_output_close(h);
}

static void test_output_open_stderr(void)
{
    pmix_output_stream_t lds;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    h = pmix_output_open(&lds);
    report("output_open_stderr: valid handle",
           h >= 0 && h < PMIX_OUTPUT_MAX_STREAMS);
    PMIX_DESTRUCT(&lds);
    pmix_output_close(h);
}

static void test_output_open_stdout(void)
{
    pmix_output_stream_t lds;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stdout = true;
    h = pmix_output_open(&lds);
    report("output_open_stdout: valid handle",
           h >= 0 && h < PMIX_OUTPUT_MAX_STREAMS);
    PMIX_DESTRUCT(&lds);
    pmix_output_close(h);
}

static void test_output_close_invalid_no_crash(void)
{
    /* Closing an out-of-range handle must not crash. */
    pmix_output_close(-1);
    pmix_output_close(PMIX_OUTPUT_MAX_STREAMS);
    report("output_close_invalid: no crash", 1);
}

/* ------------------------------------------------------------------ */
/* pmix_output_switch                                                  */
/* ------------------------------------------------------------------ */

static void test_output_switch(void)
{
    pmix_output_stream_t lds;
    bool prev;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    h = pmix_output_open(&lds);
    PMIX_DESTRUCT(&lds);

    /* Newly opened stream is enabled; disabling returns true (was enabled). */
    prev = pmix_output_switch(h, false);
    report("output_switch_disable: was enabled", prev == true);

    /* Re-enabling returns false (was disabled). */
    prev = pmix_output_switch(h, true);
    report("output_switch_reenable: was disabled", prev == false);

    pmix_output_close(h);
}

/* ------------------------------------------------------------------ */
/* pmix_output_set/get_verbosity                                       */
/* ------------------------------------------------------------------ */

static void test_output_verbosity(void)
{
    pmix_output_stream_t lds;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    h = pmix_output_open(&lds);
    PMIX_DESTRUCT(&lds);

    pmix_output_set_verbosity(h, 5);
    report("verbosity_set_get: set 5 → get 5", 5 == pmix_output_get_verbosity(h));

    pmix_output_set_verbosity(h, 0);
    report("verbosity_set_get: set 0 → get 0", 0 == pmix_output_get_verbosity(h));

    pmix_output_close(h);
}

/* ------------------------------------------------------------------ */
/* pmix_output_set_output_file_info                                   */
/* ------------------------------------------------------------------ */

static void test_output_file_info(void)
{
    char *olddir = NULL;
    char *oldprefix = NULL;

    pmix_output_set_output_file_info("/tmp", "unit-test-prefix-", &olddir, &oldprefix);
    report("output_file_info: old dir returned", olddir != NULL);
    report("output_file_info: old prefix returned", oldprefix != NULL);

    /* Restore previous values. */
    pmix_output_set_output_file_info(olddir, oldprefix, NULL, NULL);
    free(olddir);
    free(oldprefix);
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

    fprintf(stdout, "\n=== pmix_output unit tests ===\n\n");

    test_output_init_idempotent();
    test_output_open_null_lds();
    test_output_open_stderr();
    test_output_open_stdout();
    test_output_close_invalid_no_crash();
    test_output_switch();
    test_output_verbosity();
    test_output_file_info();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
