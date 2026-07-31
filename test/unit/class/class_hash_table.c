/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_hash_table_t:
 *   init, get_size, uint32 / uint64 / ptr key ops,
 *   remove_all, and iteration.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_hash_table.h"

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
    pmix_hash_table_t ht;
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    int rc = pmix_hash_table_init(&ht, 16);
    report("init: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("init: size is 0", 0 == pmix_hash_table_get_size(&ht));
    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */
/* uint32 key: set / get / remove                                       */
/* ------------------------------------------------------------------ */

static void test_uint32_set_get(void)
{
    pmix_hash_table_t ht;
    void *val;
    int data1 = 42, data2 = 99;
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 16);

    int rc = pmix_hash_table_set_value_uint32(&ht, 1, &data1);
    report("uint32 set key 1: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    rc = pmix_hash_table_set_value_uint32(&ht, 2, &data2);
    report("uint32 set key 2: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    report("uint32: size is 2", 2 == pmix_hash_table_get_size(&ht));

    rc = pmix_hash_table_get_value_uint32(&ht, 1, &val);
    report("uint32 get key 1: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("uint32 get key 1: value matches", val == (void *) &data1);

    rc = pmix_hash_table_get_value_uint32(&ht, 2, &val);
    report("uint32 get key 2: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("uint32 get key 2: value matches", val == (void *) &data2);

    rc = pmix_hash_table_get_value_uint32(&ht, 99, &val);
    report("uint32 get missing key: returns ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);

    PMIX_DESTRUCT(&ht);
}

static void test_uint32_remove(void)
{
    pmix_hash_table_t ht;
    void *val;
    int data = 7;
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 16);

    pmix_hash_table_set_value_uint32(&ht, 10, &data);

    int rc = pmix_hash_table_remove_value_uint32(&ht, 10);
    report("uint32 remove: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    rc = pmix_hash_table_get_value_uint32(&ht, 10, &val);
    report("uint32 get after remove: ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);

    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */
/* uint64 key: set / get / remove                                       */
/* ------------------------------------------------------------------ */

static void test_uint64_set_get(void)
{
    pmix_hash_table_t ht;
    void *val;
    int data = 123;
    uint64_t key = 0xDEADBEEFCAFEBABEULL;
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 16);

    int rc = pmix_hash_table_set_value_uint64(&ht, key, &data);
    report("uint64 set: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    rc = pmix_hash_table_get_value_uint64(&ht, key, &val);
    report("uint64 get: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("uint64 get: value matches", val == (void *) &data);

    rc = pmix_hash_table_remove_value_uint64(&ht, key);
    report("uint64 remove: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    rc = pmix_hash_table_get_value_uint64(&ht, key, &val);
    report("uint64 get after remove: ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);

    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */
/* ptr (binary) key: set / get / remove                                 */
/* ------------------------------------------------------------------ */

static void test_ptr_set_get(void)
{
    pmix_hash_table_t ht;
    void *val;
    int data = 55;
    const char *key = "mykey";
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 16);

    int rc = pmix_hash_table_set_value_ptr(&ht, key, strlen(key), &data);
    report("ptr set: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    rc = pmix_hash_table_get_value_ptr(&ht, key, strlen(key), &val);
    report("ptr get: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("ptr get: value matches", val == (void *) &data);

    rc = pmix_hash_table_remove_value_ptr(&ht, key, strlen(key));
    report("ptr remove: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);

    rc = pmix_hash_table_get_value_ptr(&ht, key, strlen(key), &val);
    report("ptr get after remove: ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);

    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */
/* remove_all                                                           */
/* ------------------------------------------------------------------ */

static void test_remove_all(void)
{
    pmix_hash_table_t ht;
    void *val;
    int d1 = 1, d2 = 2, d3 = 3;
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 16);

    pmix_hash_table_set_value_uint32(&ht, 1, &d1);
    pmix_hash_table_set_value_uint32(&ht, 2, &d2);
    pmix_hash_table_set_value_uint32(&ht, 3, &d3);
    report("remove_all: size is 3 before", 3 == pmix_hash_table_get_size(&ht));

    int rc = pmix_hash_table_remove_all(&ht);
    report("remove_all: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("remove_all: size is 0 after", 0 == pmix_hash_table_get_size(&ht));

    rc = pmix_hash_table_get_value_uint32(&ht, 1, &val);
    report("remove_all: key 1 not found", PMIX_ERR_NOT_FOUND == rc);

    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */
/* uint32 iteration                                                     */
/* ------------------------------------------------------------------ */

static void test_uint32_iteration(void)
{
    pmix_hash_table_t ht;
    int d1 = 10, d2 = 20, d3 = 30;
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 16);

    pmix_hash_table_set_value_uint32(&ht, 100, &d1);
    pmix_hash_table_set_value_uint32(&ht, 200, &d2);
    pmix_hash_table_set_value_uint32(&ht, 300, &d3);

    uint32_t key;
    void *val;
    void *node = NULL;
    int count = 0;

    if (PMIX_SUCCESS == pmix_hash_table_get_first_key_uint32(&ht, &key, &val, &node)) {
        count++;
        while (PMIX_SUCCESS
               == pmix_hash_table_get_next_key_uint32(&ht, &key, &val, node, &node)) {
            count++;
        }
    }
    report("uint32 iteration: visited all 3 entries", 3 == count);

    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */
/* sizeof_hash_element                                                  */
/* ------------------------------------------------------------------ */

static void test_sizeof_hash_element(void)
{
    size_t sz = pmix_hash_table_sizeof_hash_element();
    report("sizeof_hash_element: returns positive size", sz > 0);
}

/* ------------------------------------------------------------------ */
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* Every lookup begins with "key % capacity", and capacity is 0 until
 * pmix_hash_table_init() runs. The guard that caught that used to sit
 * inside #if PMIX_ENABLE_DEBUG, so an optimized build took a SIGFPE where
 * a debug build returned an error. Each of these nine entry points must
 * report a failure and come back. */
static void test_uninitialized_table(void)
{
    pmix_hash_table_t ht;
    void *value = NULL;
    const char *key = "k";

    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);

    report("un-init'd: get uint32 fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_get_value_uint32(&ht, 1, &value));
    report("un-init'd: set uint32 fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_set_value_uint32(&ht, 1, (void *) 0x1));
    report("un-init'd: remove uint32 fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_remove_value_uint32(&ht, 1));

    report("un-init'd: get uint64 fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_get_value_uint64(&ht, 1, &value));
    report("un-init'd: set uint64 fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_set_value_uint64(&ht, 1, (void *) 0x1));
    report("un-init'd: remove uint64 fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_remove_value_uint64(&ht, 1));

    report("un-init'd: get ptr fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_get_value_ptr(&ht, key, 1, &value));
    report("un-init'd: set ptr fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_set_value_ptr(&ht, key, 1, (void *) 0x1));
    report("un-init'd: remove ptr fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_remove_value_ptr(&ht, key, 1));

    PMIX_DESTRUCT(&ht);
}

/* The destructor used to free ht_table and leave the pointer in place
 * behind a non-zero ht_capacity, so a stray lookup indexed freed memory
 * instead of failing the capacity check. Calling PMIX_DESTRUCT twice is a
 * contract violation the object system's magic-ID assert catches under
 * --enable-debug, so it is not exercised here. */
static void test_destruct_leaves_constructed_state(void)
{
    pmix_hash_table_t ht;
    void *value = NULL;

    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 32);
    pmix_hash_table_set_value_uint32(&ht, 7, (void *) 0x77);

    PMIX_DESTRUCT(&ht);
    report("destruct: lookup on the corpse fails cleanly",
           PMIX_SUCCESS != pmix_hash_table_get_value_uint32(&ht, 7, &value));
    report("destruct: size reset to 0", 0 == (int) pmix_hash_table_get_size(&ht));

    /* and it is re-usable */
    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    report("re-init after destruct: PMIX_SUCCESS", PMIX_SUCCESS == pmix_hash_table_init(&ht, 32));
    report("re-init after destruct: set works",
           PMIX_SUCCESS == pmix_hash_table_set_value_uint32(&ht, 7, (void *) 0x77));
    report("re-init after destruct: get works",
           PMIX_SUCCESS == pmix_hash_table_get_value_uint32(&ht, 7, &value)
               && (void *) 0x77 == value);
    PMIX_DESTRUCT(&ht);
}

/* Enough insertions to drive several pmix_hash_grow() passes, then verify
 * every key survived rehashing, and that removal's backshift keeps the
 * remaining probe runs findable. */
static void test_growth_and_backshift(void)
{
    pmix_hash_table_t ht;
    void *value;
    uint64_t k;
    bool ok = true;

    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    pmix_hash_table_init(&ht, 4); /* deliberately tiny: forces repeated growth */

    for (k = 1; k <= 500; k++) {
        if (PMIX_SUCCESS != pmix_hash_table_set_value_uint64(&ht, k, (void *) (intptr_t) (k * 3))) {
            ok = false;
            break;
        }
    }
    report("growth: 500 inserts into a 4-slot table succeed", ok);

    ok = true;
    for (k = 1; k <= 500; k++) {
        if (PMIX_SUCCESS != pmix_hash_table_get_value_uint64(&ht, k, &value)
            || (void *) (intptr_t) (k * 3) != value) {
            ok = false;
            break;
        }
    }
    report("growth: every key survived rehashing", ok);
    report("growth: size reports 500", 500 == (int) pmix_hash_table_get_size(&ht));

    /* Remove every other key; the backshift has to keep the rest reachable */
    ok = true;
    for (k = 2; k <= 500; k += 2) {
        if (PMIX_SUCCESS != pmix_hash_table_remove_value_uint64(&ht, k)) {
            ok = false;
            break;
        }
    }
    report("backshift: removing 250 keys succeeds", ok);
    report("backshift: size reports 250", 250 == (int) pmix_hash_table_get_size(&ht));

    ok = true;
    for (k = 1; k <= 500; k += 2) {
        if (PMIX_SUCCESS != pmix_hash_table_get_value_uint64(&ht, k, &value)
            || (void *) (intptr_t) (k * 3) != value) {
            ok = false;
            break;
        }
    }
    report("backshift: every surviving key is still findable", ok);

    ok = true;
    for (k = 2; k <= 500; k += 2) {
        if (PMIX_ERR_NOT_FOUND != pmix_hash_table_get_value_uint64(&ht, k, &value)) {
            ok = false;
            break;
        }
    }
    report("backshift: every removed key reports NOT_FOUND", ok);

    PMIX_DESTRUCT(&ht);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_hash_table_t unit tests ===\n\n");

    test_init();
    test_uint32_set_get();
    test_uint32_remove();
    test_uint64_set_get();
    test_ptr_set_get();
    test_remove_all();
    test_uint32_iteration();
    test_sizeof_hash_element();
    test_uninitialized_table();
    test_destruct_leaves_constructed_state();
    test_growth_and_backshift();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
