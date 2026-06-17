/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_alfg PRNG utility functions:
 *   pmix_srand, pmix_rand, pmix_random.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/util/pmix_alfg.h"

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
/* pmix_srand                                                          */
/* ------------------------------------------------------------------ */

static void test_srand_returns_one(void)
{
    pmix_rng_buff_t buff;
    /* The header comment says this returns 1 on success, not PMIX_SUCCESS. */
    report("srand_returns_one", 1 == pmix_srand(&buff, 12345));
}

static void test_srand_sets_tap1(void)
{
    pmix_rng_buff_t buff;
    pmix_srand(&buff, 0);
    /* TAP1 == 127, so tap1 is initialised to TAP1-1 == 126 */
    report("srand_sets_tap1", 126 == buff.tap1);
}

static void test_srand_sets_tap2(void)
{
    pmix_rng_buff_t buff;
    pmix_srand(&buff, 0);
    /* TAP2 == 97, so tap2 is initialised to TAP2-1 == 96 */
    report("srand_sets_tap2", 96 == buff.tap2);
}

/* ------------------------------------------------------------------ */
/* pmix_rand                                                           */
/* ------------------------------------------------------------------ */

static void test_rand_deterministic(void)
{
    pmix_rng_buff_t b1, b2;
    uint32_t v1, v2;
    int i, same;

    pmix_srand(&b1, 42);
    pmix_srand(&b2, 42);

    same = 1;
    for (i = 0; i < 20; i++) {
        v1 = pmix_rand(&b1);
        v2 = pmix_rand(&b2);
        if (v1 != v2) {
            same = 0;
            break;
        }
    }
    report("rand_deterministic: same seed yields same 20-value sequence", same);
}

static void test_rand_different_seeds(void)
{
    pmix_rng_buff_t b1, b2;
    int i, different;

    pmix_srand(&b1, 1);
    pmix_srand(&b2, 999999);

    different = 0;
    for (i = 0; i < 100; i++) {
        if (pmix_rand(&b1) != pmix_rand(&b2)) {
            different = 1;
            break;
        }
    }
    report("rand_different_seeds: distinct seeds produce different sequences",
           different);
}

static void test_rand_sequence_advances(void)
{
    pmix_rng_buff_t buff;
    uint32_t v0, v1, v2;

    pmix_srand(&buff, 7);
    v0 = pmix_rand(&buff);
    v1 = pmix_rand(&buff);
    v2 = pmix_rand(&buff);
    /* Extremely unlikely that all three consecutive values are equal */
    report("rand_sequence_advances: consecutive values not all equal",
           !(v0 == v1 && v1 == v2));
}

/* ------------------------------------------------------------------ */
/* pmix_random                                                         */
/* ------------------------------------------------------------------ */

static void test_random_nonnegative(void)
{
    pmix_rng_buff_t dummy;
    int i, ok;

    /* Seed the global buffer used by pmix_random(). */
    pmix_srand(&dummy, 777);

    ok = 1;
    for (i = 0; i < 200; i++) {
        if (pmix_random() < 0) {
            ok = 0;
            break;
        }
    }
    report("random_nonnegative: 200 calls all return >= 0", ok);
}

static void test_random_seeded_deterministic(void)
{
    pmix_rng_buff_t dummy;
    int r1, r2;

    pmix_srand(&dummy, 555);
    r1 = pmix_random();

    /* Re-seed with the same value and draw again; must match. */
    pmix_srand(&dummy, 555);
    r2 = pmix_random();

    report("random_seeded_deterministic: same seed gives same first value",
           r1 == r2);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_alfg unit tests ===\n\n");

    test_srand_returns_one();
    test_srand_sets_tap1();
    test_srand_sets_tap2();

    test_rand_deterministic();
    test_rand_different_seeds();
    test_rand_sequence_advances();

    test_random_nonnegative();
    test_random_seeded_deterministic();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
