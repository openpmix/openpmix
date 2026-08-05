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
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* The bitmap words are uint64_t, so the mask shift runs to 63. It used to
 * be built as "1UL << offset", which is undefined above 31 wherever
 * unsigned long is 32 bits (32-bit Linux, Windows) and in practice folded
 * bit N back onto bit N-32. On an LP64 host this passes either way; it is
 * here so the 32-bit builds have something that fails when it regresses. */
static void test_high_bit_shifts(void)
{
    pmix_bitmap_t bm;
    int bit;
    bool ok = true;

    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 128);

    for (bit = 32; bit < 64; ++bit) {
        if (PMIX_SUCCESS != pmix_bitmap_set_bit(&bm, bit)) {
            ok = false;
        }
    }
    report("high bits: set 32..63 all succeed", ok);

    ok = true;
    for (bit = 32; bit < 64; ++bit) {
        if (!pmix_bitmap_is_set_bit(&bm, bit)) {
            ok = false;
        }
    }
    report("high bits: 32..63 all read back set", ok);

    /* If the shift had wrapped, setting bit 32+ would have lit bit 0+ */
    ok = true;
    for (bit = 0; bit < 32; ++bit) {
        if (pmix_bitmap_is_set_bit(&bm, bit)) {
            ok = false;
        }
    }
    report("high bits: 0..31 left untouched (no shift wrap)", ok);

    report("high bits: 32 set bits counted", 32 == pmix_bitmap_num_set_bits(&bm, 64));

    /* clear_bit builds the same mask */
    pmix_bitmap_clear_bit(&bm, 63);
    report("high bits: clear_bit 63 clears 63", !pmix_bitmap_is_set_bit(&bm, 63));
    report("high bits: clear_bit 63 spared 31", !pmix_bitmap_is_set_bit(&bm, 31));
    report("high bits: 31 set bits remain", 31 == pmix_bitmap_num_set_bits(&bm, 64));

    PMIX_DESTRUCT(&bm);
}

/* num_set_bits masks the final partial word with (1 << (len % 64)) - 1.
 * With len == 63 that shift distance is 63, which overflowed the sign bit
 * of the signed 1LL the code used to shift. */
static void test_num_set_bits_boundaries(void)
{
    pmix_bitmap_t bm;

    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 128);
    pmix_bitmap_set_all_bits(&bm);

    report("num_set_bits len=1: 1", 1 == pmix_bitmap_num_set_bits(&bm, 1));
    report("num_set_bits len=32: 32", 32 == pmix_bitmap_num_set_bits(&bm, 32));
    report("num_set_bits len=63: 63", 63 == pmix_bitmap_num_set_bits(&bm, 63));
    report("num_set_bits len=64: 64", 64 == pmix_bitmap_num_set_bits(&bm, 64));
    report("num_set_bits len=65: 65", 65 == pmix_bitmap_num_set_bits(&bm, 65));
    report("num_set_bits len=127: 127", 127 == pmix_bitmap_num_set_bits(&bm, 127));
    report("num_set_bits len=128: 128", 128 == pmix_bitmap_num_set_bits(&bm, 128));
    report("num_unset_bits len=63: 0", 0 == pmix_bitmap_num_unset_bits(&bm, 63));

    PMIX_DESTRUCT(&bm);
}

/* pmix_bitmap_copy() now reports what it did instead of returning void and
 * memcpy'ing through whatever calloc handed back. */
static void test_copy_status(void)
{
    pmix_bitmap_t src, dst;

    PMIX_CONSTRUCT(&src, pmix_bitmap_t);
    PMIX_CONSTRUCT(&dst, pmix_bitmap_t);
    pmix_bitmap_init(&src, 256);
    pmix_bitmap_init(&dst, 64); /* deliberately smaller: forces the grow path */

    pmix_bitmap_set_bit(&src, 200);
    report("copy: grow path returns PMIX_SUCCESS", PMIX_SUCCESS == pmix_bitmap_copy(&dst, &src));
    report("copy: grown dst holds bit 200", pmix_bitmap_is_set_bit(&dst, 200));
    report("copy: grown dst size matches src",
           pmix_bitmap_size(&dst) == pmix_bitmap_size(&src));
    report("copy: bitmaps compare equal", !pmix_bitmap_are_different(&dst, &src));

    report("copy: NULL dst is PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_copy(NULL, &src));
    report("copy: NULL src is PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_copy(&dst, NULL));

    PMIX_DESTRUCT(&src);
    PMIX_DESTRUCT(&dst);
}

/* Every accessor has to tolerate a bitmap that was constructed but never
 * init'd (no words at all) and a NULL bitmap. */
static void test_uninitialized_and_null(void)
{
    pmix_bitmap_t bm;

    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);

    report("un-init'd: size is 0", 0 == pmix_bitmap_size(&bm));
    report("un-init'd: is_clear is true", pmix_bitmap_is_clear(&bm));
    report("un-init'd: clear_all_bits succeeds", PMIX_SUCCESS == pmix_bitmap_clear_all_bits(&bm));
    report("un-init'd: set_all_bits succeeds", PMIX_SUCCESS == pmix_bitmap_set_all_bits(&bm));
    report("un-init'd: is_set_bit(0) is false", !pmix_bitmap_is_set_bit(&bm, 0));
    report("un-init'd: clear_bit(0) is PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_clear_bit(&bm, 0));
    report("un-init'd: set_bit(0) grows and succeeds",
           PMIX_SUCCESS == pmix_bitmap_set_bit(&bm, 0));
    report("un-init'd: bit 0 now reads set", pmix_bitmap_is_set_bit(&bm, 0));

    report("NULL: is_clear is true", pmix_bitmap_is_clear(NULL));
    report("NULL: clear_all_bits is PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_clear_all_bits(NULL));
    report("NULL: set_all_bits is PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_set_all_bits(NULL));
    report("NULL: set_bit is PMIX_ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == pmix_bitmap_set_bit(NULL, 0));
    report("NULL: is_set_bit is false", !pmix_bitmap_is_set_bit(NULL, 0));
    /* bool-returning comparison has no error code: "different" is the
     * answer that stops a caller acting on a comparison never made */
    report("NULL: are_different reports true", pmix_bitmap_are_different(NULL, &bm));
    report("NULL: get_string returns NULL", NULL == pmix_bitmap_get_string(NULL));

    PMIX_DESTRUCT(&bm);
}

/* Re-init has to release the old words and leave size and contents
 * describing the new allocation, with no stale bits showing through. */
static void test_reinit(void)
{
    pmix_bitmap_t bm;

    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
    pmix_bitmap_init(&bm, 256);
    pmix_bitmap_set_all_bits(&bm);
    report("reinit: 256 bits before", 256 == pmix_bitmap_size(&bm));

    report("reinit: second init succeeds", PMIX_SUCCESS == pmix_bitmap_init(&bm, 128));
    report("reinit: size is now 128", 128 == pmix_bitmap_size(&bm));
    report("reinit: comes back cleared", pmix_bitmap_is_clear(&bm));
    report("reinit: no set bits", 0 == pmix_bitmap_num_set_bits(&bm, 128));

    PMIX_DESTRUCT(&bm);
}

/* ------------------------------------------------------------------ */

/* The NULL/negative-length guard in pmix_bitmap_num_set_bits() used to sit
 * inside "#if PMIX_ENABLE_DEBUG", so the only builds that had it were the
 * ones nobody ships: an optimized library read bm->array_size straight off
 * the NULL pointer. pmix_bitmap_num_unset_bits() inherited it, and worse,
 * answered "len bits are unset" for a bitmap that does not exist.
 *
 * (all builds -- in a debug build the old code returned 0 too, so this
 * case only distinguishes old from new when compiled --disable-debug) */
static void test_count_bits_null_guard(void)
{
    report("num_set_bits(NULL): returns 0 rather than faulting",
           0 == pmix_bitmap_num_set_bits(NULL, 64));
    report("num_unset_bits(NULL): returns 0, not len",
           0 == pmix_bitmap_num_unset_bits(NULL, 64));
    report("num_set_bits(NULL, 0): returns 0", 0 == pmix_bitmap_num_set_bits(NULL, 0));

    {
        pmix_bitmap_t bm;
        PMIX_CONSTRUCT(&bm, pmix_bitmap_t);
        pmix_bitmap_init(&bm, 64);
        pmix_bitmap_set_bit(&bm, 0);
        report("num_set_bits(bm, negative len): returns 0",
               0 == pmix_bitmap_num_set_bits(&bm, -1));
        report("num_unset_bits(bm, negative len): returns 0",
               0 == pmix_bitmap_num_unset_bits(&bm, -1));
        PMIX_DESTRUCT(&bm);
    }
}

/* pmix_bitmap_t was one of two classes with no PMIX_*_STATIC_INIT. The
 * value of having one is that it agrees with the constructor: max_size in
 * particular is INT_MAX there, and a zero would have made the resulting
 * bitmap reject every init() and every set_bit(). */
static void test_static_init_matches_constructor(void)
{
    pmix_bitmap_t stat = PMIX_BITMAP_STATIC_INIT;
    pmix_bitmap_t ctor;
    int pos = -1;

    PMIX_CONSTRUCT(&ctor, pmix_bitmap_t);

    report("static init: bitmap matches the constructor", stat.bitmap == ctor.bitmap);
    report("static init: array_size matches the constructor",
           stat.array_size == ctor.array_size);
    report("static init: max_size matches the constructor", stat.max_size == ctor.max_size);

    /* reads on the empty bitmap answer rather than faulting */
    report("static init: is_set_bit(0) is false", !pmix_bitmap_is_set_bit(&stat, 0));
    report("static init: is_clear is true", pmix_bitmap_is_clear(&stat));
    report("static init: size is 0", 0 == pmix_bitmap_size(&stat));

    /* and it is usable: set_bit auto-expands from nothing */
    report("static init: set_bit(5) succeeds", PMIX_SUCCESS == pmix_bitmap_set_bit(&stat, 5));
    report("static init: bit 5 reads back set", pmix_bitmap_is_set_bit(&stat, 5));
    report("static init: find_and_set finds bit 0",
           PMIX_SUCCESS == pmix_bitmap_find_and_set_first_unset_bit(&stat, &pos) && 0 == pos);

    /* A STATIC_INIT object carries the BASE class, so PMIX_DESTRUCT on
     * it runs pmix_object_t's (empty) chain and never the derived
     * destructor - which means everything the calls above made it
     * allocate would leak. That is the whole point of the invariant in
     * src/class/AGENTS.md: STATIC_INIT gives a defined state, and a
     * PMIX_CONSTRUCT is what makes an object a live instance of its own
     * class. Here the object has been used without one, deliberately, so
     * name its class before tearing it down. */
    stat.super.obj_class = PMIX_CLASS(pmix_bitmap_t);
    PMIX_DESTRUCT(&stat);
    PMIX_DESTRUCT(&ctor);
}

/* Regression: pmix_bitmap_set_max_size() was the one entry point in this
 * file that did not validate its argument. The cap is stored as a word
 * count, converted with "((size_t) max_size + 63) / 64" -- so a negative
 * bit count became (size_t) -1, whose "+ 63" wrapped round to 62, which
 * divided down to a stored cap of *zero words*. The call reported success
 * and every later init() and set_bit() then failed with
 * PMIX_ERR_BAD_PARAM, a long way from the call that actually got it
 * wrong. Zero was accepted the same way.
 *
 * (all builds -- this was never gated on PMIX_ENABLE_DEBUG, just absent) */
static void test_set_max_size_argument_checking(void)
{
    pmix_bitmap_t bm;

    PMIX_CONSTRUCT(&bm, pmix_bitmap_t);

    report("set_max_size(-1): PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_set_max_size(&bm, -1));
    report("set_max_size(0): PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_set_max_size(&bm, 0));
    report("set_max_size(NULL): PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_set_max_size(NULL, 64));

    /* the rejected calls must not have touched the cap: the bitmap is
     * still uncapped and a plain init still works. Before the fix the
     * negative call left max_size at 0 and this init returned BAD_PARAM. */
    report("rejected set_max_size left the bitmap usable",
           PMIX_SUCCESS == pmix_bitmap_init(&bm, 128));
    report("rejected set_max_size left the size right", 128 <= pmix_bitmap_size(&bm));
    report("rejected set_max_size left growth working",
           PMIX_SUCCESS == pmix_bitmap_set_bit(&bm, 4095));

    /* a real cap still behaves */
    report("set_max_size(256): PMIX_SUCCESS", PMIX_SUCCESS == pmix_bitmap_set_max_size(&bm, 256));
    report("capped: a bit inside the cap is accepted",
           PMIX_SUCCESS == pmix_bitmap_set_bit(&bm, 255));
    report("capped: a bit beyond the cap is refused",
           PMIX_ERR_BAD_PARAM == pmix_bitmap_set_bit(&bm, 4096));

    PMIX_DESTRUCT(&bm);
}

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
    test_high_bit_shifts();
    test_num_set_bits_boundaries();
    test_copy_status();
    test_uninitialized_and_null();
    test_reinit();
    test_count_bits_null_guard();
    test_static_init_matches_constructor();
    test_set_max_size_argument_checking();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
