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

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
