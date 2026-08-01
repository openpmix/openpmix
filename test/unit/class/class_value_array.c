/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_value_array_t:
 *   init, get_size, set_size, reserve, get/set item (macros and functions),
 *   append_item, remove_item, PMIX_VALUE_ARRAY_GET_BASE.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_value_array.h"

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
/* init / get_size                                                      */
/* ------------------------------------------------------------------ */

static void test_init(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    int rc = pmix_value_array_init(&va, sizeof(int));
    report("init: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("init: get_size is 0", 0 == pmix_value_array_get_size(&va));
    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* set_size                                                             */
/* ------------------------------------------------------------------ */

static void test_set_size(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));

    int rc = pmix_value_array_set_size(&va, 10);
    report("set_size 10: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_size 10: get_size is 10", 10 == (int) pmix_value_array_get_size(&va));

    rc = pmix_value_array_set_size(&va, 5);
    report("set_size shrink to 5: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_size shrink: get_size is 5", 5 == (int) pmix_value_array_get_size(&va));

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* reserve                                                              */
/* ------------------------------------------------------------------ */

static void test_reserve(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));

    int rc = pmix_value_array_reserve(&va, 100);
    report("reserve 100: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("reserve: get_size still 0 (no items added)", 0 == (int) pmix_value_array_get_size(&va));

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* GET_ITEM / SET_ITEM macros                                           */
/* ------------------------------------------------------------------ */

static void test_get_set_item_macros(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));
    pmix_value_array_set_size(&va, 5);

    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 0, 100);
    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 1, 200);
    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 4, 500);

    report("GET_ITEM macro [0]: 100", 100 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 0));
    report("GET_ITEM macro [1]: 200", 200 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 1));
    report("GET_ITEM macro [4]: 500", 500 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 4));

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* pmix_value_array_set_item / get_item (functions, auto-grow)         */
/* ------------------------------------------------------------------ */

static void test_set_item_fn(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(double));

    double v = 3.14;
    int rc = pmix_value_array_set_item(&va, 2, &v);
    report("set_item fn at [2]: PMIX_SUCCESS (auto-grows)", PMIX_SUCCESS == rc);
    report("set_item fn: size is now 3", 3 == (int) pmix_value_array_get_size(&va));

    double *p = (double *) pmix_value_array_get_item(&va, 2);
    report("get_item fn [2]: non-NULL", NULL != p);
    report("get_item fn [2]: value is 3.14", NULL != p && 3.14 == *p);

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* append_item                                                          */
/* ------------------------------------------------------------------ */

static void test_append_item(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));

    for (int i = 0; i < 5; i++) {
        int rc = pmix_value_array_append_item(&va, &i);
        report("append_item: PMIX_SUCCESS", PMIX_SUCCESS == rc);
    }
    report("append 5 items: size is 5", 5 == (int) pmix_value_array_get_size(&va));

    bool ok = true;
    for (int i = 0; i < 5; i++) {
        if (i != PMIX_VALUE_ARRAY_GET_ITEM(&va, int, i)) {
            ok = false;
        }
    }
    report("append_item: values correct in order", ok);

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* remove_item                                                          */
/* ------------------------------------------------------------------ */

static void test_remove_item(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));

    for (int v = 10; v <= 50; v += 10) {
        pmix_value_array_append_item(&va, &v);
    }
    /* Array: [10, 20, 30, 40, 50] */

    int rc = pmix_value_array_remove_item(&va, 1); /* remove index 1 (value 20) */
    report("remove_item [1]: PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("remove_item: size is 4", 4 == (int) pmix_value_array_get_size(&va));
    report("remove_item: new [0] is 10", 10 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 0));
    report("remove_item: new [1] is 30", 30 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 1));
    report("remove_item: new [3] is 50", 50 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 3));

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* PMIX_VALUE_ARRAY_GET_BASE macro                                      */
/* ------------------------------------------------------------------ */

static void test_get_base(void)
{
    pmix_value_array_t va;
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));
    pmix_value_array_set_size(&va, 3);

    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 0, 7);
    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 1, 8);
    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 2, 9);

    int *base = PMIX_VALUE_ARRAY_GET_BASE(&va, int);
    report("GET_BASE: pointer non-NULL", NULL != base);
    report("GET_BASE: base[0] is 7", 7 == base[0]);
    report("GET_BASE: base[1] is 8", 8 == base[1]);
    report("GET_BASE: base[2] is 9", 9 == base[2]);

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* set_size() grew by doubling array_alloc_size in place. On an array whose
 * alloc size is still 0 -- constructed but not init'd, built from
 * PMIX_VALUE_ARRAY_STATIC_INIT, or zeroed by a reserve() that ran out of
 * memory -- "0 <<= 1" is 0 and the loop never terminated. A regression
 * here hangs rather than failing, so keep it early in the run. */
static void test_set_size_zero_alloc_does_not_hang(void)
{
    pmix_value_array_t va;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    /* Item size is zero as well here, which is its own error... */
    report("set_size on un-init'd array: PMIX_ERR_BAD_PARAM (all builds)",
           PMIX_ERR_BAD_PARAM == pmix_value_array_set_size(&va, 4));

    /* ...so drive the doubling loop itself: a valid item size with no
     * allocation behind it, which is exactly the state a failed reserve()
     * used to leave. This call must return, not spin. */
    va.array_item_sizeof = sizeof(int);
    va.array_alloc_size = 0;
    report("set_size with zero alloc size: returns PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_value_array_set_size(&va, 100));
    report("set_size with zero alloc size: size is 100",
           100 == (int) pmix_value_array_get_size(&va));

    PMIX_VALUE_ARRAY_SET_ITEM(&va, int, 99, 4242);
    report("set_size with zero alloc size: last slot is usable",
           4242 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 99));

    PMIX_DESTRUCT(&va);
}

/* remove_item()'s bounds check used to be compiled out unless
 * --enable-debug, but the memmove length it guards is
 * (array_size - item_index - 1) computed in size_t: one past the end wraps
 * to an enormous count. */
static void test_remove_item_out_of_range(void)
{
    pmix_value_array_t va;
    int v;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));
    for (v = 0; v < 3; v++) {
        pmix_value_array_append_item(&va, &v);
    }

    report("remove_item at size: PMIX_ERR_BAD_PARAM (all builds)",
           PMIX_ERR_BAD_PARAM == pmix_value_array_remove_item(&va, 3));
    report("remove_item well past end: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_value_array_remove_item(&va, 9999));
    report("remove_item out of range: size unchanged", 3 == (int) pmix_value_array_get_size(&va));
    report("remove_item out of range: contents intact",
           0 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 0)
               && 2 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 2));

    report("remove_item last valid index: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_value_array_remove_item(&va, 2));
    report("remove_item last valid index: size is 2", 2 == (int) pmix_value_array_get_size(&va));

    PMIX_DESTRUCT(&va);
}

/* The destructor now returns the array to its constructed state, so
 * nothing is left addressing the freed buffer. Calling PMIX_DESTRUCT twice
 * is a contract violation the object system's magic-ID assert catches
 * under --enable-debug, so it is not exercised here. */
static void test_destruct_leaves_constructed_state(void)
{
    pmix_value_array_t va;
    int v = 7;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));
    pmix_value_array_append_item(&va, &v);

    PMIX_DESTRUCT(&va);
    report("destruct: size reset to 0", 0 == (int) pmix_value_array_get_size(&va));
    report("destruct: items pointer cleared", NULL == va.array_items);
    report("destruct: alloc size reset to 0", 0 == (int) va.array_alloc_size);

    /* and the corpse is still re-usable */
    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    report("re-init after destruct: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_value_array_init(&va, sizeof(int)));
    report("re-init after destruct: append works",
           PMIX_SUCCESS == pmix_value_array_append_item(&va, &v));
    report("re-init after destruct: value reads back",
           7 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 0));
    PMIX_DESTRUCT(&va);
}

/* A reserve() that cannot grow must leave the array exactly as it found
 * it. It used to overwrite array_items with realloc's NULL -- leaking the
 * buffer and losing every element -- and zero both size fields. */
static void test_reserve_failure_preserves_contents(void)
{
    pmix_value_array_t va;
    int v, rc;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));
    for (v = 0; v < 5; v++) {
        pmix_value_array_append_item(&va, &v);
    }

    /* Ask for an allocation no allocator can satisfy. If the platform does
     * somehow honor it, the reserve simply succeeds and the checks below
     * still describe a correct array. */
    rc = pmix_value_array_reserve(&va, (size_t) -1 / sizeof(int));
    report("reserve of an absurd size: reports failure or succeeds honestly",
           PMIX_ERR_OUT_OF_RESOURCE == rc || PMIX_SUCCESS == rc);
    if (PMIX_ERR_OUT_OF_RESOURCE == rc) {
        report("failed reserve: size preserved", 5 == (int) pmix_value_array_get_size(&va));
        report("failed reserve: contents preserved",
               0 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 0)
                   && 4 == PMIX_VALUE_ARRAY_GET_ITEM(&va, int, 4));
        report("failed reserve: array still usable",
               PMIX_SUCCESS == pmix_value_array_append_item(&va, &v));
    }

    PMIX_DESTRUCT(&va);
}

/* Growth across several doublings, checking every element survives each
 * realloc. */
static void test_many_appends(void)
{
    pmix_value_array_t va;
    int i;
    bool ok = true;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));

    for (i = 0; i < 1000; i++) {
        if (PMIX_SUCCESS != pmix_value_array_append_item(&va, &i)) {
            ok = false;
            break;
        }
    }
    report("1000 appends: all succeed", ok);
    report("1000 appends: size is 1000", 1000 == (int) pmix_value_array_get_size(&va));

    ok = true;
    for (i = 0; i < 1000; i++) {
        if (i != PMIX_VALUE_ARRAY_GET_ITEM(&va, int, i)) {
            ok = false;
            break;
        }
    }
    report("1000 appends: every element survived the reallocs", ok);

    PMIX_DESTRUCT(&va);
}

/* ------------------------------------------------------------------ */

/* pmix_value_array_init() assigned realloc()'s result straight back into
 * array_items. On failure that leaks whatever the array already owned and
 * leaves a NULL buffer behind the item size and alloc size the same call
 * has just committed -- the array then claims one element of capacity it
 * does not have. The identical correction was already made in
 * pmix_value_array_reserve() and pmix_value_array_set_size(); init was
 * missed. A zero item size is now rejected outright too: every offset in
 * the class is index * array_item_sizeof, so a zero collapses the whole
 * array onto one address. */
static void test_init_argument_checking(void)
{
    pmix_value_array_t va;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);

    report("init with item size 0: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_value_array_init(&va, 0));
    report("rejected init leaves no allocation", NULL == va.array_items);
    report("rejected init leaves item size 0", 0 == va.array_item_sizeof);
    report("rejected init leaves alloc size 0", 0 == va.array_alloc_size);

    /* a good init still works afterwards */
    report("init(sizeof(int)) after rejection: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_value_array_init(&va, sizeof(int)));
    report("good init: item size recorded", sizeof(int) == va.array_item_sizeof);
    report("good init: capacity seeded to 1", 1 == va.array_alloc_size);
    report("good init: size is 0", 0 == pmix_value_array_get_size(&va));

    PMIX_DESTRUCT(&va);
}

/* Re-initializing a live array must not lose the elements it is holding on
 * the way to a clean, empty one, and must not leak the old buffer. */
static void test_reinit_over_a_populated_array(void)
{
    pmix_value_array_t va;
    int v;

    PMIX_CONSTRUCT(&va, pmix_value_array_t);
    pmix_value_array_init(&va, sizeof(int));

    v = 11; pmix_value_array_append_item(&va, &v);
    v = 22; pmix_value_array_append_item(&va, &v);
    v = 33; pmix_value_array_append_item(&va, &v);
    report("populated: size is 3", 3 == pmix_value_array_get_size(&va));

    report("re-init over a populated array: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_value_array_init(&va, sizeof(long)));
    report("re-init: size back to 0", 0 == pmix_value_array_get_size(&va));
    report("re-init: item size is the new one", sizeof(long) == va.array_item_sizeof);
    report("re-init: buffer is live", NULL != va.array_items);

    {
        long l = 7;
        report("re-init: append works", PMIX_SUCCESS == pmix_value_array_append_item(&va, &l));
        report("re-init: element reads back",
               7 == PMIX_VALUE_ARRAY_GET_ITEM(&va, long, 0));
    }

    PMIX_DESTRUCT(&va);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_value_array_t unit tests ===\n\n");

    test_init();
    test_set_size();
    test_reserve();
    test_get_set_item_macros();
    test_set_item_fn();
    test_append_item();
    test_remove_item();
    test_get_base();
    test_set_size_zero_alloc_does_not_hang();
    test_remove_item_out_of_range();
    test_destruct_leaves_constructed_state();
    test_reserve_failure_preserves_contents();
    test_many_appends();
    test_init_argument_checking();
    test_reinit_over_a_populated_array();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
