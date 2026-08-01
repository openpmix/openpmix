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
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* init() sized the initial allocation from the caller's raw block_size
 * rather than the normalized one, so init(a, 0, max, 0) allocated nothing
 * while a->block_size claimed 8. */
static void test_init_zero_block_size(void)
{
    pmix_pointer_array_t arr;
    int idx;

    PMIX_CONSTRUCT(&arr, pmix_pointer_array_t);
    report("init with 0 initial and 0 block size: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_pointer_array_init(&arr, 0, INT_MAX, 0));
    report("init with 0 block size: array has room", 0 < arr.size);

    idx = pmix_pointer_array_add(&arr, (void *) 0x1);
    report("init with 0 block size: add yields a valid index", 0 <= idx);
    report("init with 0 block size: item reads back",
           (void *) 0x1 == pmix_pointer_array_get_item(&arr, idx));

    PMIX_DESTRUCT(&arr);
}

/* grow_table() bumped number_free before growing free_bits. Cross several
 * 64-slot free_bits words so both reallocs run repeatedly, and confirm the
 * indices and the free accounting stay in step. */
static void test_growth_across_bit_words(void)
{
    pmix_pointer_array_t arr;
    int idx[300];
    int i;
    bool ok = true;

    PMIX_CONSTRUCT(&arr, pmix_pointer_array_t);
    pmix_pointer_array_init(&arr, 2, INT_MAX, 2);

    for (i = 0; i < 300; i++) {
        idx[i] = pmix_pointer_array_add(&arr, (void *) (intptr_t) (i + 1));
        if (0 > idx[i]) {
            ok = false;
            break;
        }
    }
    report("growth: 300 adds across many free_bits words succeed", ok);

    ok = true;
    for (i = 0; i < 300; i++) {
        if ((void *) (intptr_t) (i + 1) != pmix_pointer_array_get_item(&arr, idx[i])) {
            ok = false;
            break;
        }
    }
    report("growth: every item readable at its index", ok);

    /* indices must be unique */
    ok = true;
    for (i = 1; i < 300; i++) {
        if (idx[i] == idx[i - 1]) {
            ok = false;
            break;
        }
    }
    report("growth: no index handed out twice", ok);

    /* Free a hole well inside the array, then confirm the next add reuses
     * it -- which only works if lowest_free and free_bits agree. */
    pmix_pointer_array_set_item(&arr, idx[100], NULL);
    report("growth: freed slot is reused by the next add",
           idx[100] == pmix_pointer_array_add(&arr, (void *) 0xabc));

    PMIX_DESTRUCT(&arr);
}

/* set_item beyond the current extent grows the array; repeated sparse sets
 * exercise grow_table's realloc-of-free_bits branch on its own. */
static void test_sparse_set_item(void)
{
    pmix_pointer_array_t arr;
    bool ok = true;
    int i;

    PMIX_CONSTRUCT(&arr, pmix_pointer_array_t);
    pmix_pointer_array_init(&arr, 4, INT_MAX, 4);

    for (i = 0; i < 512; i += 64) {
        if (PMIX_SUCCESS != pmix_pointer_array_set_item(&arr, i, (void *) (intptr_t) (i + 1))) {
            ok = false;
            break;
        }
    }
    report("sparse set_item: every sparse index accepted", ok);

    ok = true;
    for (i = 0; i < 512; i += 64) {
        if ((void *) (intptr_t) (i + 1) != pmix_pointer_array_get_item(&arr, i)) {
            ok = false;
            break;
        }
    }
    report("sparse set_item: every sparse index reads back", ok);

    report("sparse set_item: an untouched index is NULL",
           NULL == pmix_pointer_array_get_item(&arr, 63));
    report("sparse set_item: an out-of-range index is NULL",
           NULL == pmix_pointer_array_get_item(&arr, 100000));
    report("sparse set_item: a negative index is rejected",
           PMIX_SUCCESS != pmix_pointer_array_set_item(&arr, -1, (void *) 0x1));

    PMIX_DESTRUCT(&arr);
}

/* max_size caps growth; the array has to refuse rather than overrun. */
static void test_max_size_cap(void)
{
    pmix_pointer_array_t arr;
    int i, idx, added = 0;

    PMIX_CONSTRUCT(&arr, pmix_pointer_array_t);
    report("max_size: init with max 8 succeeds",
           PMIX_SUCCESS == pmix_pointer_array_init(&arr, 4, 8, 4));

    for (i = 0; i < 32; i++) {
        idx = pmix_pointer_array_add(&arr, (void *) (intptr_t) (i + 1));
        if (0 > idx) {
            break;
        }
        added++;
    }
    report("max_size: adds stop at the cap", added <= 8);
    report("max_size: the array did fill up to the cap", 0 < added);
    report("max_size: the add past the cap reports failure", 32 > added);

    PMIX_DESTRUCT(&arr);
}

/* ------------------------------------------------------------------ */

/* pmix_pointer_array_test_and_set_item() guarded its index with a bare
 * assert(index >= 0). That is not a guard in any build that ships:
 * configure adds -DNDEBUG whenever --enable-debug is off
 * (config/pmix.m4), so the check vanished and a negative index wrote
 * through table->addr below the start of the allocation -- and then ran
 * SET_BIT on it, corrupting free_bits too. Its sibling
 * pmix_pointer_array_set_item() has always rejected negatives outright.
 *
 * (all builds -- in a debug build the old code aborted on the assert
 * rather than returning false, so this only distinguishes old from new
 * when compiled --disable-debug) */
static void test_test_and_set_negative_index(void)
{
    pmix_pointer_array_t arr;
    int before_free, before_size;

    PMIX_CONSTRUCT(&arr, pmix_pointer_array_t);
    pmix_pointer_array_init(&arr, 8, 64, 8);
    pmix_pointer_array_add(&arr, (void *) 0x1);

    before_free = arr.number_free;
    before_size = arr.size;

    report("test_and_set at -1: returns false",
           !pmix_pointer_array_test_and_set_item(&arr, -1, (void *) 0xbad));
    report("test_and_set at -1: number_free untouched", before_free == arr.number_free);
    report("test_and_set at -1: size untouched", before_size == arr.size);

    report("test_and_set at INT_MIN-ish: returns false",
           !pmix_pointer_array_test_and_set_item(&arr, -1000000, (void *) 0xbad));
    report("test_and_set: rejected calls left the array usable",
           (void *) 0x1 == pmix_pointer_array_get_item(&arr, 0));

    /* a valid index still works */
    report("test_and_set at 3: succeeds",
           pmix_pointer_array_test_and_set_item(&arr, 3, (void *) 0x3));
    report("test_and_set at 3: reads back",
           (void *) 0x3 == pmix_pointer_array_get_item(&arr, 3));

    PMIX_DESTRUCT(&arr);
}

/* PMIX_POINTER_ARRAY_STATIC_INIT set max_size and block_size to 0 where
 * the constructor sets INT_MAX and 8. grow_table() divides by block_size,
 * so an array that reached pmix_pointer_array_add() on a static
 * initializer -- without the pmix_pointer_array_init() that every in-tree
 * use happens to perform first -- took SIGFPE.
 *
 * (all builds -- an integer divide by zero, not an assert) */
static void test_static_init_matches_constructor(void)
{
    pmix_pointer_array_t stat = PMIX_POINTER_ARRAY_STATIC_INIT;
    pmix_pointer_array_t ctor;
    int idx;

    PMIX_CONSTRUCT(&ctor, pmix_pointer_array_t);

    report("static init: lowest_free matches the constructor",
           stat.lowest_free == ctor.lowest_free);
    report("static init: number_free matches the constructor",
           stat.number_free == ctor.number_free);
    report("static init: size matches the constructor", stat.size == ctor.size);
    report("static init: max_size matches the constructor", stat.max_size == ctor.max_size);
    report("static init: block_size matches the constructor",
           stat.block_size == ctor.block_size);
    report("static init: free_bits matches the constructor", stat.free_bits == ctor.free_bits);
    report("static init: addr matches the constructor", stat.addr == ctor.addr);

    /* The payoff: add() on a never-init'd array grows from nothing rather
     * than dividing by a zero block size. */
    idx = pmix_pointer_array_add(&stat, (void *) 0x55);
    report("static init: add without init returns a valid index", 0 <= idx);
    report("static init: the pointer reads back",
           (void *) 0x55 == pmix_pointer_array_get_item(&stat, idx));

    PMIX_DESTRUCT(&stat);
    PMIX_DESTRUCT(&ctor);
}

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
    test_init_zero_block_size();
    test_growth_across_bit_words();
    test_sparse_set_item();
    test_max_size_cap();
    test_test_and_set_negative_index();
    test_static_init_matches_constructor();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
