/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_pointer_array_t:
 *   init, add, get_item, set_item, test_and_set_item,
 *   set_size, get_size, remove_all.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/class/pmix_pointer_array.h"

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
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    int rc = pmix_pointer_array_init(&pa, 4, 1024, 4);
    report("init: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("init: size == 4", 4 == pmix_pointer_array_get_size(&pa));
    PMIX_DESTRUCT(&pa);
}

/* ------------------------------------------------------------------ */
/* add / get_item                                                       */
/* ------------------------------------------------------------------ */

static void test_add_and_get(void)
{
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    pmix_pointer_array_init(&pa, 4, 1024, 4);

    int val1 = 100, val2 = 200, val3 = 300;

    int idx1 = pmix_pointer_array_add(&pa, &val1);
    int idx2 = pmix_pointer_array_add(&pa, &val2);
    int idx3 = pmix_pointer_array_add(&pa, &val3);

    report("add: first index >= 0",       idx1 >= 0);
    report("add: second index >= 0",      idx2 >= 0);
    report("add: third index >= 0",       idx3 >= 0);
    report("add: indices are distinct",   idx1 != idx2 && idx2 != idx3);

    report("get_item idx1: value matches", pmix_pointer_array_get_item(&pa, idx1) == &val1);
    report("get_item idx2: value matches", pmix_pointer_array_get_item(&pa, idx2) == &val2);
    report("get_item idx3: value matches", pmix_pointer_array_get_item(&pa, idx3) == &val3);

    PMIX_DESTRUCT(&pa);
}

/* ------------------------------------------------------------------ */
/* set_item / get_item out of bounds                                    */
/* ------------------------------------------------------------------ */

static void test_set_item(void)
{
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    pmix_pointer_array_init(&pa, 8, 1024, 8);

    int val = 77;
    int rc = pmix_pointer_array_set_item(&pa, 3, &val);
    report("set_item: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_item: get_item returns correct pointer",
           pmix_pointer_array_get_item(&pa, 3) == &val);

    rc = pmix_pointer_array_set_item(&pa, 3, NULL);
    report("set_item NULL: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_item NULL: get_item returns NULL",
           NULL == pmix_pointer_array_get_item(&pa, 3));

    report("get_item out of bounds (negative): returns NULL",
           NULL == pmix_pointer_array_get_item(&pa, -1));

    PMIX_DESTRUCT(&pa);
}

/* ------------------------------------------------------------------ */
/* test_and_set_item                                                    */
/* ------------------------------------------------------------------ */

static void test_test_and_set(void)
{
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    pmix_pointer_array_init(&pa, 8, 1024, 8);

    int val1 = 11, val2 = 22;

    bool ok = pmix_pointer_array_test_and_set_item(&pa, 5, &val1);
    report("test_and_set: first set at [5] succeeds", ok);
    report("test_and_set: item is set to val1",
           pmix_pointer_array_get_item(&pa, 5) == &val1);

    ok = pmix_pointer_array_test_and_set_item(&pa, 5, &val2);
    report("test_and_set: second set at occupied [5] fails", !ok);
    report("test_and_set: original value preserved",
           pmix_pointer_array_get_item(&pa, 5) == &val1);

    PMIX_DESTRUCT(&pa);
}

/* ------------------------------------------------------------------ */
/* remove_all                                                           */
/* ------------------------------------------------------------------ */

static void test_remove_all(void)
{
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    pmix_pointer_array_init(&pa, 8, 1024, 8);

    int val = 42;
    pmix_pointer_array_add(&pa, &val);
    pmix_pointer_array_add(&pa, &val);
    pmix_pointer_array_add(&pa, &val);

    pmix_pointer_array_remove_all(&pa);

    int size = pmix_pointer_array_get_size(&pa);
    bool all_null = true;
    for (int i = 0; i < size; i++) {
        if (NULL != pmix_pointer_array_get_item(&pa, i)) {
            all_null = false;
            break;
        }
    }
    report("remove_all: all items are NULL", all_null);

    PMIX_DESTRUCT(&pa);
}

/* ------------------------------------------------------------------ */
/* set_size / auto-grow                                                 */
/* ------------------------------------------------------------------ */

static void test_set_size(void)
{
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    pmix_pointer_array_init(&pa, 4, 1024, 4);

    int rc = pmix_pointer_array_set_size(&pa, 32);
    report("set_size 32: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("set_size 32: size >= 32", pmix_pointer_array_get_size(&pa) >= 32);

    PMIX_DESTRUCT(&pa);
}

static void test_grow_via_set_item(void)
{
    pmix_pointer_array_t pa;
    PMIX_CONSTRUCT(&pa, pmix_pointer_array_t);
    pmix_pointer_array_init(&pa, 4, 1024, 4);

    int val = 7;
    int rc = pmix_pointer_array_set_item(&pa, 100, &val);
    report("set_item beyond current size: returns PMIX_SUCCESS (auto-grow)", PMIX_SUCCESS == rc);
    report("set_item beyond: value retrievable", pmix_pointer_array_get_item(&pa, 100) == &val);
    report("set_item beyond: size grew past 100", pmix_pointer_array_get_size(&pa) > 100);

    PMIX_DESTRUCT(&pa);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_pointer_array_t unit tests ===\n\n");

    test_init();
    test_add_and_get();
    test_set_item();
    test_test_and_set();
    test_remove_all();
    test_set_size();
    test_grow_via_set_item();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
