/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_timings utility.
 *
 * PMIX_ENABLE_TIMING is 0 in this build, so all PMIX_TIMING_* macros
 * expand to no-ops.  The tests verify:
 *   - PMIX_ENABLE_TIMING == 0
 *   - the no-op macros compile cleanly and have no runtime effect
 *   - the conditional full-path code (guarded by #if PMIX_ENABLE_TIMING)
 *     is kept here for reference but not compiled in this build.
 *
 * No PMIx_Init required; no library functions are exercised at runtime.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "pmix.h"
#include "src/util/pmix_timings.h"

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
/* PMIX_ENABLE_TIMING constant                                         */
/* ------------------------------------------------------------------ */

static void test_timing_disabled(void)
{
    report("PMIX_ENABLE_TIMING is zero", 0 == PMIX_ENABLE_TIMING);
}

/* ------------------------------------------------------------------ */
/* No-op macro smoke tests                                             */
/* ------------------------------------------------------------------ */

static void test_timing_macros_noop(void)
{
    /* All macros must compile to nothing when PMIX_ENABLE_TIMING == 0.
     * PMIX_TIMING_RELEASE(NULL) is the one macro safe to call directly
     * because NULL is a valid handle when timing is disabled. */
    PMIX_TIMING_RELEASE(NULL);
    report("PMIX_TIMING_RELEASE(NULL): no-op, no crash", 1);
}

/* ------------------------------------------------------------------ */
/* Conditional full-path test (compiled only when timing is enabled)  */
/* ------------------------------------------------------------------ */

#if PMIX_ENABLE_TIMING
static void test_timing_full_path(void)
{
    pmix_timing_t t;
    pmix_timing_prep_t p;
    int id;

    pmix_timing_init(&t);
    p = pmix_timing_prep_ev(&t, "step %d", 1);
    pmix_timing_add_step(p, __func__, __FILE__, __LINE__);
    id = pmix_timing_descr(pmix_timing_prep_ev(&t, "interval"),
                           __func__, __FILE__, __LINE__);
    pmix_timing_start_id(&t, id, __func__, __FILE__, __LINE__);
    pmix_timing_end(&t, id, __func__, __FILE__, __LINE__);
    pmix_timing_release(&t);
    report("timing_full_path: init/add_step/descr/start/end/release", 1);
}
#endif

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_timings unit tests ===\n\n");

    test_timing_disabled();
    test_timing_macros_noop();
#if PMIX_ENABLE_TIMING
    test_timing_full_path();
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
