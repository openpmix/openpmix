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

#include <limits.h>
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

/* pmix_hash_table_construct() initialized every member except ht_label.
 * That is not a decorative field: src/util/pmix_hash.c prints it at four
 * sites as "(NULL == table->ht_label) ? "UNKNOWN" : table->ht_label", so
 * the NULL test is what stands between %s and a wild pointer. PMIX_NEW
 * mallocs without zeroing, and PMIX_CONSTRUCT initializes in place, so
 * every table that does not set its own label -- gds/shmem3's and the mca
 * base's among them -- carried garbage there.
 *
 * (all builds -- there was never an assert on this) */
static void test_construct_initializes_label(void)
{
    pmix_hash_table_t *heap;
    pmix_hash_table_t stack;
    pmix_hash_table_t stat = PMIX_HASH_TABLE_STATIC_INIT;

    /* The heap case is the one that had garbage: PMIX_NEW does not zero. */
    heap = PMIX_NEW(pmix_hash_table_t);
    report("PMIX_NEW: allocation succeeded", NULL != heap);
    if (NULL != heap) {
        report("PMIX_NEW: ht_label is NULL, not garbage", NULL == heap->ht_label);
        PMIX_RELEASE(heap);
    }

    PMIX_CONSTRUCT(&stack, pmix_hash_table_t);
    report("PMIX_CONSTRUCT: ht_label is NULL", NULL == stack.ht_label);
    report("static init: ht_label matches the constructor",
           stat.ht_label == stack.ht_label);
    PMIX_DESTRUCT(&stack);
}

/* pmix_hash_table_init2() is a PMIX_EXPORT entry point that took two
 * ratios and used them without looking. A zero density_numer divided by
 * zero on its first line; a zero growth_denom did the same inside
 * pmix_hash_grow(). Worse silently: a growth factor that does not grow, or
 * a density of 1/1 or looser, lets the table fill completely -- and every
 * get/set/remove probes with an unbounded "for (;; ii += 1)" loop, so a
 * full table is an infinite loop, not an error. */
static void test_init2_rejects_bad_ratios(void)
{
    pmix_hash_table_t ht;

    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);

    report("init2 density_numer 0: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_init2(&ht, 16, 0, 2, 2, 1));
    report("init2 density_denom 0: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_init2(&ht, 16, 1, 0, 2, 1));
    report("init2 density 1/1 (table can fill): PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_init2(&ht, 16, 1, 1, 2, 1));
    report("init2 growth_denom 0: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_init2(&ht, 16, 1, 2, 2, 0));
    report("init2 growth 1/1 (does not grow): PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_init2(&ht, 16, 1, 2, 1, 1));
    report("init2 negative growth_numer: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_init2(&ht, 16, 1, 2, -2, 1));

    report("rejected init2 left the table un-init'd", 0 == ht.ht_capacity);
    report("rejected init2 left no allocation", NULL == ht.ht_table);

    /* the sane ratios still work, and so does the default init */
    report("init2 with 1/2 and 3/2: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_hash_table_init2(&ht, 16, 1, 2, 3, 2));
    report("accepted init2: capacity is non-zero", 0 < ht.ht_capacity);
    PMIX_DESTRUCT(&ht);

    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    report("plain init still works", PMIX_SUCCESS == pmix_hash_table_init(&ht, 16));
    PMIX_DESTRUCT(&ht);
}

/* pmix_next_poweroftwo() computed "1 << (32 - clz(value))" in *signed*
 * arithmetic. For any value at or above 0x40000000 that shift lands on bit
 * 31 of an int -- signed overflow, undefined; for a negative value clz is
 * 0 and the shift distance is 32, also undefined. The two arms disagreed
 * on negatives as well. It is computed unsigned now, and inputs with no
 * representable answer say so with 0 rather than inventing one. */
static void test_next_poweroftwo(void)
{
    report("next_poweroftwo(0) is 1", 1 == pmix_next_poweroftwo(0));
    report("next_poweroftwo(1) is 2", 2 == pmix_next_poweroftwo(1));
    report("next_poweroftwo(3) is 4", 4 == pmix_next_poweroftwo(3));
    report("next_poweroftwo(8) is 16", 16 == pmix_next_poweroftwo(8));
    report("next_poweroftwo(1000) is 1024", 1024 == pmix_next_poweroftwo(1000));

    /* the boundary that used to be undefined */
    report("next_poweroftwo(1<<29) is 1<<30",
           (1 << 30) == pmix_next_poweroftwo(1 << 29));
    report("next_poweroftwo(1<<30) has no positive answer -> 0",
           0 == pmix_next_poweroftwo(1 << 30));
    report("next_poweroftwo(INT_MAX) has no positive answer -> 0",
           0 == pmix_next_poweroftwo(INT_MAX));

    /* negatives are answered, not shifted by 32 */
    report("next_poweroftwo(-1) is 0", 0 == pmix_next_poweroftwo(-1));
    report("next_poweroftwo(INT_MIN) is 0", 0 == pmix_next_poweroftwo(INT_MIN));
}

/* Regression (all builds): "one key type per table" is enforced by a single
 * pointer compare that used to sit inside "#if PMIX_ENABLE_DEBUG". In every
 * build that ships, mixing key types was therefore silent -- the entry point
 * simply retargeted ht_type_methods, and from that moment a ptr-keyed table
 * had no elt_destructor (so every copied key leaked) while pmix_hash_grow()
 * and pmix_hash_table_remove_elt_at() rehashed its elements through
 * key.u32, i.e. the low bytes of a key pointer, scattering live entries to
 * slots no later lookup would probe.
 *
 * The check is unconditional now, so the wrong-type call is refused and the
 * table is left exactly as it was. Backing the fix out makes the "declines"
 * cases fail in an optimized build; the "still intact" cases are what
 * demonstrates the corruption it was hiding. */
static void test_key_type_mixing_is_refused(void)
{
    pmix_hash_table_t ht;
    const char *key = "the-key";
    void *val = NULL;
    uint32_t k32;
    void *node = NULL;

    PMIX_CONSTRUCT(&ht, pmix_hash_table_t);
    report("mixing: init succeeds", PMIX_SUCCESS == pmix_hash_table_init(&ht, 8));
    report("mixing: ptr set succeeds",
           PMIX_SUCCESS == pmix_hash_table_set_value_ptr(&ht, key, strlen(key), (void *) 0x1234));

    /* Every uint32/uint64 entry point must refuse this table, and refuse
     * it with PMIX_ERROR specifically: a plain "not PMIX_SUCCESS" is not
     * a discriminating assertion here, because without the check a get
     * simply probes past the ptr entry and reports PMIX_ERR_NOT_FOUND. */
    report("mixing: uint32 get declines",
           PMIX_ERROR == pmix_hash_table_get_value_uint32(&ht, 1, &val));
    report("mixing: uint32 set declines",
           PMIX_ERROR == pmix_hash_table_set_value_uint32(&ht, 1, (void *) 0x1));
    report("mixing: uint32 remove declines",
           PMIX_ERROR == pmix_hash_table_remove_value_uint32(&ht, 1));
    report("mixing: uint64 get declines",
           PMIX_ERROR == pmix_hash_table_get_value_uint64(&ht, 1, &val));
    report("mixing: uint64 set declines",
           PMIX_ERROR == pmix_hash_table_set_value_uint64(&ht, 1, (void *) 0x1));
    report("mixing: uint64 remove declines",
           PMIX_ERROR == pmix_hash_table_remove_value_uint64(&ht, 1));

    /* and the ptr table is intact: the rejected calls neither retargeted
     * the methods nor added an entry of their own. Note the refused set
     * above is what this catches -- without the check it inserts, and the
     * matching refused remove would have taken it away again, so check
     * the count here rather than after a set/remove pair. */
    report("mixing: the table still holds exactly one entry",
           1 == pmix_hash_table_get_size(&ht));

    /* the reverse direction too: a uint32 table refuses ptr and uint64 */
    report("mixing: remove_all resets the key type",
           PMIX_SUCCESS == pmix_hash_table_remove_all(&ht));
    report("mixing: uint32 set on the reused table",
           PMIX_SUCCESS == pmix_hash_table_set_value_uint32(&ht, 7, (void *) 0x77));
    report("mixing: ptr get on a uint32 table declines",
           PMIX_ERROR == pmix_hash_table_get_value_ptr(&ht, key, strlen(key), &val));
    report("mixing: uint64 set on a uint32 table declines",
           PMIX_ERROR == pmix_hash_table_set_value_uint64(&ht, 7, (void *) 0x77));
    report("mixing: the uint32 table still holds exactly one entry",
           1 == pmix_hash_table_get_size(&ht));
    k32 = 0;
    report("mixing: the uint32 entry is still there",
           PMIX_SUCCESS == pmix_hash_table_get_first_key_uint32(&ht, &k32, &val, &node));
    report("mixing: it is the key we stored", 7 == k32 && (void *) 0x77 == val);

    /* the ptr family also rejects an unusable key outright: a NULL one is
     * dereferenced by the hash, and a zero-length one compares equal to
     * every other key while asking the allocator for a 0-byte block. */
    report("mixing: remove_all again", PMIX_SUCCESS == pmix_hash_table_remove_all(&ht));
    report("ptr key: NULL key is refused",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_set_value_ptr(&ht, NULL, 4, (void *) 0x1));
    report("ptr key: zero-length key is refused",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_set_value_ptr(&ht, key, 0, (void *) 0x1));
    report("ptr key: NULL key refused on get",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_get_value_ptr(&ht, NULL, 4, &val));
    report("ptr key: zero-length key refused on remove",
           PMIX_ERR_BAD_PARAM == pmix_hash_table_remove_value_ptr(&ht, key, 0));
    report("ptr key: a real key still works",
           PMIX_SUCCESS == pmix_hash_table_set_value_ptr(&ht, key, strlen(key), (void *) 0x9));

    PMIX_DESTRUCT(&ht);
}

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
    test_construct_initializes_label();
    test_init2_rejects_bad_ratios();
    test_next_poweroftwo();
    test_key_type_mixing_is_refused();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
