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

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
