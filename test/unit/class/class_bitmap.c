/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_bitmap_t:
 *   init, set_bit, clear_bit, is_set_bit, find_and_set_first_unset_bit,
 *   clear_all_bits, set_all_bits, is_clear, num_set_bits, num_unset_bits,
 *   bitwise AND/OR/XOR inplace, copy, are_different, get_string.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/class/pmix_bitmap.h"

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
/* init                                                                 */
/* ------------------------------------------------------------------ */

static void test_init(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    int rc = pmix_bitmap_init(&bm, 64);
    report("init: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("init: size >= 64", pmix_bitmap_size(&bm) >= 64);
    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */
/* set_bit / is_set_bit / clear_bit                                     */
/* ------------------------------------------------------------------ */

static void test_set_clear_bit(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 64);

    int rc = pmix_bitmap_set_bit(&bm, 5);
    report("set_bit: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_bit: bit 5 is set", pmix_bitmap_is_set_bit(&bm, 5));
    report("set_bit: bit 4 is not set", !pmix_bitmap_is_set_bit(&bm, 4));

    rc = pmix_bitmap_clear_bit(&bm, 5);
    report("clear_bit: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("clear_bit: bit 5 is now clear", !pmix_bitmap_is_set_bit(&bm, 5));

    PMIX_DESTRUCT(&bm);
}

static void test_set_beyond_size(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 8);

    int rc = pmix_bitmap_set_bit(&bm, 200);
    report("set_bit beyond size: auto-expands (returns PMIX_SUCCESS)", PMIX_SUCCESS == rc);
    report("set_bit beyond size: bit 200 is set", pmix_bitmap_is_set_bit(&bm, 200));
    report("set_bit beyond size: size grew past 200", pmix_bitmap_size(&bm) > 200);

    PMIX_DESTRUCT(&bm);
}

static void test_out_of_range_clear(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 8);

    int rc = pmix_bitmap_clear_bit(&bm, 9999);
    report("clear_bit out of range: returns error", PMIX_SUCCESS != rc);
    report("is_set_bit out of range: returns false", !pmix_bitmap_is_set_bit(&bm, 9999));

    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */
/* set_max_size                                                         */
/* ------------------------------------------------------------------ */

static void test_max_size(void)
{
    pmix_bitmap_t bm;
    int rc;

    /* Cap the bitmap at 128 bits, then initialize within that cap.
     * Regression: max_size is tracked in words internally, so an init or
     * set_bit request expressed in bits must be converted before the
     * comparison. This previously failed spuriously (e.g. init(64) saw
     * 64 > 2 and returned PMIX_ERR_BAD_PARAM). */
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    rc = pmix_bitmap_set_max_size(&bm, 128);
    report("max_size: set_max_size returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    rc = pmix_bitmap_init(&bm, 64);
    report("max_size: init(64) within 128-bit cap succeeds", PMIX_SUCCESS == rc);

    rc = pmix_bitmap_set_bit(&bm, 100);
    report("max_size: set_bit(100) within cap succeeds", PMIX_SUCCESS == rc);
    report("max_size: bit 100 is set", pmix_bitmap_is_set_bit(&bm, 100));

    /* A bit beyond the cap must be rejected, not silently written OOB. */
    rc = pmix_bitmap_set_bit(&bm, 100000);
    report("max_size: set_bit beyond cap is rejected", PMIX_SUCCESS != rc);

    PMIX_DESTRUCT(&bm);

    /* An init larger than the cap must fail. */
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_set_max_size(&bm, 128);
    rc = pmix_bitmap_init(&bm, 256);
    report("max_size: init(256) beyond 128-bit cap is rejected", PMIX_SUCCESS != rc);
    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */
/* find_and_set_first_unset_bit                                         */
/* ------------------------------------------------------------------ */

static void test_find_and_set(void)
{
    pmix_bitmap_t bm;
    int pos;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 64);

    pmix_bitmap_set_bit(&bm, 0);
    pmix_bitmap_set_bit(&bm, 1);
    pmix_bitmap_set_bit(&bm, 2);

    int rc = pmix_bitmap_find_and_set_first_unset_bit(&bm, &pos);
    report("find_and_set: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("find_and_set: first unset bit is 3", 3 == pos);
    report("find_and_set: bit 3 is now set", pmix_bitmap_is_set_bit(&bm, 3));

    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */
/* clear_all_bits / set_all_bits / is_clear                             */
/* ------------------------------------------------------------------ */

static void test_clear_all(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 64);

    pmix_bitmap_set_bit(&bm, 1);
    pmix_bitmap_set_bit(&bm, 10);
    pmix_bitmap_set_bit(&bm, 63);

    int rc = pmix_bitmap_clear_all_bits(&bm);
    report("clear_all: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("clear_all: bit 1 is clear", !pmix_bitmap_is_set_bit(&bm, 1));
    report("clear_all: bit 63 is clear", !pmix_bitmap_is_set_bit(&bm, 63));
    report("clear_all: is_clear returns true", pmix_bitmap_is_clear(&bm));

    PMIX_DESTRUCT(&bm);
}

static void test_set_all(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 64);

    int rc = pmix_bitmap_set_all_bits(&bm);
    report("set_all: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_all: bit 0 is set", pmix_bitmap_is_set_bit(&bm, 0));
    report("set_all: bit 63 is set", pmix_bitmap_is_set_bit(&bm, 63));
    report("set_all: is_clear returns false", !pmix_bitmap_is_clear(&bm));

    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */
/* num_set_bits / num_unset_bits                                        */
/* ------------------------------------------------------------------ */

static void test_count_bits(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 64);

    pmix_bitmap_set_bit(&bm, 0);
    pmix_bitmap_set_bit(&bm, 2);
    pmix_bitmap_set_bit(&bm, 4);

    report("num_set_bits: 3 set in first 8",   3 == pmix_bitmap_num_set_bits(&bm, 8));
    report("num_unset_bits: 5 unset in first 8", 5 == pmix_bitmap_num_unset_bits(&bm, 8));

    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */
/* bitwise OR inplace                                                   */
/* ------------------------------------------------------------------ */

static void test_bitwise_or(void)
{
    pmix_bitmap_t a, b;
    PMIX_CONSTRUCT(&a, pmix_bitmap_t);
    PMIX_CONSTRUCT(&b, pmix_bitmap_t);
    pmix_bitmap_init(&a, 64);
    pmix_bitmap_init(&b, 64);

    pmix_bitmap_set_bit(&a, 0);
    pmix_bitmap_set_bit(&a, 2);
    pmix_bitmap_set_bit(&b, 1);
    pmix_bitmap_set_bit(&b, 2);

    int rc = pmix_bitmap_bitwise_or_inplace(&a, &b);
    report("bitwise_or: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("bitwise_or: bit 0 set (only in a)",  pmix_bitmap_is_set_bit(&a, 0));
    report("bitwise_or: bit 1 set (only in b)",  pmix_bitmap_is_set_bit(&a, 1));
    report("bitwise_or: bit 2 set (in both)",    pmix_bitmap_is_set_bit(&a, 2));

    PMIX_DESTRUCT(&a);
    PMIX_DESTRUCT(&b);
}

/* ------------------------------------------------------------------ */
/* bitwise AND inplace                                                  */
/* ------------------------------------------------------------------ */

static void test_bitwise_and(void)
{
    pmix_bitmap_t a, b;
    PMIX_CONSTRUCT(&a, pmix_bitmap_t);
    PMIX_CONSTRUCT(&b, pmix_bitmap_t);
    pmix_bitmap_init(&a, 64);
    pmix_bitmap_init(&b, 64);

    pmix_bitmap_set_bit(&a, 0);
    pmix_bitmap_set_bit(&a, 2);
    pmix_bitmap_set_bit(&b, 1);
    pmix_bitmap_set_bit(&b, 2);

    int rc = pmix_bitmap_bitwise_and_inplace(&a, &b);
    report("bitwise_and: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("bitwise_and: bit 0 clear (only in a)", !pmix_bitmap_is_set_bit(&a, 0));
    report("bitwise_and: bit 1 clear (only in b)", !pmix_bitmap_is_set_bit(&a, 1));
    report("bitwise_and: bit 2 set (in both)",      pmix_bitmap_is_set_bit(&a, 2));

    PMIX_DESTRUCT(&a);
    PMIX_DESTRUCT(&b);
}

/* ------------------------------------------------------------------ */
/* bitwise XOR inplace                                                  */
/* ------------------------------------------------------------------ */

static void test_bitwise_xor(void)
{
    pmix_bitmap_t a, b;
    PMIX_CONSTRUCT(&a, pmix_bitmap_t);
    PMIX_CONSTRUCT(&b, pmix_bitmap_t);
    pmix_bitmap_init(&a, 64);
    pmix_bitmap_init(&b, 64);

    pmix_bitmap_set_bit(&a, 0);
    pmix_bitmap_set_bit(&a, 2);
    pmix_bitmap_set_bit(&b, 1);
    pmix_bitmap_set_bit(&b, 2);

    int rc = pmix_bitmap_bitwise_xor_inplace(&a, &b);
    report("bitwise_xor: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("bitwise_xor: bit 0 set (only in a)",    pmix_bitmap_is_set_bit(&a, 0));
    report("bitwise_xor: bit 1 set (only in b)",    pmix_bitmap_is_set_bit(&a, 1));
    report("bitwise_xor: bit 2 clear (in both)", !pmix_bitmap_is_set_bit(&a, 2));

    PMIX_DESTRUCT(&a);
    PMIX_DESTRUCT(&b);
}

/* ------------------------------------------------------------------ */
/* copy / are_different                                                 */
/* ------------------------------------------------------------------ */

static void test_copy_and_compare(void)
{
    pmix_bitmap_t src, dst;
    PMIX_CONSTRUCT(&src, pmix_bitmap_t);
    PMIX_CONSTRUCT(&dst, pmix_bitmap_t);
    pmix_bitmap_init(&src, 64);
    pmix_bitmap_init(&dst, 64);

    pmix_bitmap_set_bit(&src, 7);
    pmix_bitmap_set_bit(&src, 42);

    report("are_different before copy: true", pmix_bitmap_are_different(&src, &dst));

    pmix_bitmap_copy(&dst, &src);
    report("copy: dst bit 7 is set",  pmix_bitmap_is_set_bit(&dst, 7));
    report("copy: dst bit 42 is set", pmix_bitmap_is_set_bit(&dst, 42));
    report("are_different after copy: false", !pmix_bitmap_are_different(&src, &dst));

    PMIX_DESTRUCT(&src);
    PMIX_DESTRUCT(&dst);
}

/* ------------------------------------------------------------------ */
/* get_string                                                           */
/* ------------------------------------------------------------------ */

static void test_get_string(void)
{
    pmix_bitmap_t bm;
    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 8);

    char *str = pmix_bitmap_get_string(&bm);
    report("get_string: returns non-NULL for cleared bitmap", NULL != str);
    if (NULL != str) {
        free(str);
    }

    pmix_bitmap_set_bit(&bm, 0);
    pmix_bitmap_set_bit(&bm, 3);
    str = pmix_bitmap_get_string(&bm);
    report("get_string: non-NULL after setting bits", NULL != str);
    if (NULL != str) {
        free(str);
    }

    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_bitmap_t unit tests ===\n\n");

    test_init();
    test_set_clear_bit();
    test_set_beyond_size();
    test_out_of_range_clear();
    test_max_size();
    test_find_and_set();
    test_clear_all();
    test_set_all();
    test_count_bits();
    test_bitwise_or();
    test_bitwise_and();
    test_bitwise_xor();
    test_copy_and_compare();
    test_get_string();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
