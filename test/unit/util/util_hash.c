/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_hash utility functions:
 *   pmix_hash_lookup_key, pmix_hash_store,
 *   pmix_hash_fetch, pmix_hash_remove_data.
 *
 * Requires PMIx_server_init because pmix_hash_store copies values
 * via pmix_bfrops_base_tma_copy_value, which needs the bfrops MCA
 * framework to be initialised.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix_server.h"
#include "pmix.h"
#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/util/pmix_hash.h"

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

/* Build a kval with a PMIX_STRING value. */
static pmix_kval_t *make_kval_str(const char *key, const char *val)
{
    pmix_kval_t *kv;
    PMIX_KVAL_NEW(kv, key);
    if (NULL == kv) {
        return NULL;
    }
    memset(kv->value, 0, sizeof(pmix_value_t));
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(val);
    return kv;
}

/* Remove all items from kvals list and RELEASE each. */
static void drain_list(pmix_list_t *lst)
{
    pmix_kval_t *kv;

    /* Take the removal result as the loop condition rather than testing
     * emptiness first. The two are equivalent at run time, but the compiler
     * cannot connect pmix_list_is_empty() to what pmix_list_remove_first()
     * returns, so it has to assume the NULL - and then warns that
     * PMIX_RELEASE writes the reference count "into a region of size 0"
     * (-Wstringop-overflow), which is an error under --enable-devel-check.
     * This is also the idiom the rest of the tree uses. */
    while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(lst))) {
        PMIX_RELEASE(kv);
    }
}

/* Allocate and initialise a hash table; caller must PMIX_RELEASE. */
static pmix_hash_table_t *new_table(void)
{
    pmix_hash_table_t *t = PMIX_NEW(pmix_hash_table_t, NULL);
    pmix_hash_table_init(t, 64);
    return t;
}

/* ------------------------------------------------------------------ */
/* pmix_hash_lookup_key                                                */
/* ------------------------------------------------------------------ */

static void test_lookup_known_key(void)
{
    /* PMIX_SERVER_TOOL_SUPPORT is a string key registered at server init time. */
    pmix_regattr_input_t *p = pmix_hash_lookup_key(UINT32_MAX, PMIX_SERVER_TOOL_SUPPORT, NULL);
    report("lookup_known_key: non-NULL", p != NULL);
    report("lookup_known_key: string matches",
           p != NULL && strcmp(p->string, PMIX_SERVER_TOOL_SUPPORT) == 0);
}

static void test_lookup_auto_register(void)
{
    /* An unknown key is auto-registered and returned non-NULL. */
    pmix_regattr_input_t *p = pmix_hash_lookup_key(UINT32_MAX, "unit.test.auto.key", NULL);
    report("lookup_auto_register: non-NULL", p != NULL);
    report("lookup_auto_register: string preserved",
           p != NULL && strcmp(p->string, "unit.test.auto.key") == 0);
}

static void test_find_key_does_not_register(void)
{
    uint32_t before, after;
    pmix_regattr_input_t *p;

    /* A known key is returned just as pmix_hash_lookup_key would. */
    p = pmix_hash_find_key(UINT32_MAX, PMIX_SERVER_TOOL_SUPPORT, NULL);
    report("find_key: known key returned", p != NULL);

    /* An unknown key returns NULL and, crucially, leaves the keyindex
     * alone. Checking the status without checking next_id would pass
     * against the old auto-registering lookup as well. */
    before = pmix_globals.keyindex.next_id;
    p = pmix_hash_find_key(UINT32_MAX, "unit.test.never.registered", NULL);
    after = pmix_globals.keyindex.next_id;
    report("find_key: unknown key returns NULL", p == NULL);
    report("find_key: unknown key did not grow keyindex", before == after);
}

/* ------------------------------------------------------------------ */
/* pmix_hash_store / pmix_hash_fetch                                   */
/* ------------------------------------------------------------------ */

static void test_fetch_does_not_register(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_list_t kvals;
    pmix_status_t rc;
    uint32_t before, after;

    /* Fetching a key nobody ever stored must not mint an index for it.
     * Before this was fixed, every failed fetch grew the process-global
     * keyindex by one entry that could never be removed - and would
     * have written into a read-only shared-memory keyindex. */
    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    before = pmix_globals.keyindex.next_id;
    rc = pmix_hash_fetch(t, 0, "unit.test.fetch.unregistered", NULL, 0, &kvals, NULL);
    after = pmix_globals.keyindex.next_id;
    report("fetch_no_register: returns ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    report("fetch_no_register: did not grow keyindex", before == after);
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    PMIX_RELEASE(t);
}

static void test_store_fetch_basic(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv = make_kval_str("unit.key1", "hello");
    pmix_list_t kvals;
    pmix_status_t rc;
    pmix_kval_t *fv;

    rc = pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);
    report("store_fetch_basic: store returns SUCCESS", PMIX_SUCCESS == rc);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key1", NULL, 0, &kvals, NULL);
    report("store_fetch_basic: fetch returns SUCCESS", PMIX_SUCCESS == rc);
    report("store_fetch_basic: list non-empty", !pmix_list_is_empty(&kvals));

    fv = pmix_list_is_empty(&kvals) ? NULL : (pmix_kval_t *) pmix_list_get_first(&kvals);
    report("store_fetch_basic: value type PMIX_STRING",
           fv != NULL && PMIX_STRING == fv->value->type);
    report("store_fetch_basic: value content",
           fv != NULL && fv->value->data.string != NULL
               && 0 == strcmp(fv->value->data.string, "hello"));

    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    pmix_hash_remove_data(t, 0, NULL, NULL);
    PMIX_RELEASE(t);
}

static void test_fetch_not_found_empty_table(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_list_t kvals;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key.absent", NULL, 0, &kvals, NULL);
    report("fetch_not_found_empty: returns ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    PMIX_RELEASE(t);
}

static void test_fetch_wrong_key(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv = make_kval_str("unit.key.A", "valA");
    pmix_list_t kvals;
    pmix_status_t rc;

    pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key.B", NULL, 0, &kvals, NULL);
    report("fetch_wrong_key: returns ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    pmix_hash_remove_data(t, 0, NULL, NULL);
    PMIX_RELEASE(t);
}

static void test_store_overwrite(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv1 = make_kval_str("unit.key.ov", "first");
    pmix_kval_t *kv2 = make_kval_str("unit.key.ov", "second");
    pmix_list_t kvals;
    pmix_status_t rc;
    pmix_kval_t *fv;

    pmix_hash_store(t, 0, kv1, NULL, 0, NULL);
    PMIX_RELEASE(kv1);
    pmix_hash_store(t, 0, kv2, NULL, 0, NULL);
    PMIX_RELEASE(kv2);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key.ov", NULL, 0, &kvals, NULL);
    report("store_overwrite: fetch succeeds", PMIX_SUCCESS == rc);

    fv = pmix_list_is_empty(&kvals) ? NULL : (pmix_kval_t *) pmix_list_get_first(&kvals);
    report("store_overwrite: second value wins",
           fv != NULL && fv->value->data.string != NULL
               && 0 == strcmp(fv->value->data.string, "second"));

    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    pmix_hash_remove_data(t, 0, NULL, NULL);
    PMIX_RELEASE(t);
}

/* ------------------------------------------------------------------ */
/* pmix_hash_remove_data                                               */
/* ------------------------------------------------------------------ */

static void test_remove_then_fetch(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv = make_kval_str("unit.key.rm", "todelete");
    pmix_list_t kvals;
    pmix_status_t rc;

    pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);

    pmix_hash_remove_data(t, 0, "unit.key.rm", NULL);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key.rm", NULL, 0, &kvals, NULL);
    report("remove_then_fetch: returns ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    pmix_hash_remove_data(t, 0, NULL, NULL);
    PMIX_RELEASE(t);
}

/* A wildcard removal with a NULL key means "everything, for every
 * rank". Each rank's proc_data is released - and the table it was
 * reached through has to let go of it in the same breath, or it is left
 * holding a pointer to a freed object that the very next lookup hands
 * back. That the entries are gone is the observable part, so check the
 * table is empty rather than fetching through a pointer that may or may
 * not still look valid. */
static void test_remove_wildcard_empties_the_table(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv0 = make_kval_str("unit.key.wc", "rank0val");
    pmix_kval_t *kv1 = make_kval_str("unit.key.wc", "rank1val");
    pmix_list_t kvals;
    pmix_status_t rc;
    uint32_t id;
    void *pd = NULL, *node = NULL;

    pmix_hash_store(t, 0, kv0, NULL, 0, NULL);
    pmix_hash_store(t, 1, kv1, NULL, 0, NULL);
    PMIX_RELEASE(kv0);
    PMIX_RELEASE(kv1);

    /* both ranks are there to start with */
    rc = pmix_hash_table_get_first_key_uint32(t, &id, &pd, &node);
    report("remove_wildcard: table populated before", PMIX_SUCCESS == rc);

    pmix_hash_remove_data(t, PMIX_RANK_WILDCARD, NULL, NULL);

    node = NULL;
    pd = NULL;
    rc = pmix_hash_table_get_first_key_uint32(t, &id, &pd, &node);
    report("remove_wildcard: table holds no entries after",
           PMIX_SUCCESS != rc);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key.wc", NULL, 0, &kvals, NULL);
    report("remove_wildcard: fetch returns ERR_NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);
    PMIX_RELEASE(t);
}

/* ------------------------------------------------------------------ */
/* Multiple ranks                                                       */
/* ------------------------------------------------------------------ */

static void test_multiple_ranks(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv0 = make_kval_str("unit.key.mr", "rank0val");
    pmix_kval_t *kv1 = make_kval_str("unit.key.mr", "rank1val");
    pmix_list_t kvals;
    pmix_status_t rc;
    pmix_kval_t *fv;

    pmix_hash_store(t, 0, kv0, NULL, 0, NULL);
    PMIX_RELEASE(kv0);
    pmix_hash_store(t, 1, kv1, NULL, 0, NULL);
    PMIX_RELEASE(kv1);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.key.mr", NULL, 0, &kvals, NULL);
    fv = pmix_list_is_empty(&kvals) ? NULL : (pmix_kval_t *) pmix_list_get_first(&kvals);
    report("multiple_ranks: rank 0 value correct",
           PMIX_SUCCESS == rc && fv != NULL && fv->value->data.string != NULL
               && 0 == strcmp(fv->value->data.string, "rank0val"));
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 1, "unit.key.mr", NULL, 0, &kvals, NULL);
    fv = pmix_list_is_empty(&kvals) ? NULL : (pmix_kval_t *) pmix_list_get_first(&kvals);
    report("multiple_ranks: rank 1 value correct",
           PMIX_SUCCESS == rc && fv != NULL && fv->value->data.string != NULL
               && 0 == strcmp(fv->value->data.string, "rank1val"));
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);

    pmix_hash_remove_data(t, 0, NULL, NULL);
    pmix_hash_remove_data(t, 1, NULL, NULL);
    PMIX_RELEASE(t);
}

/* ------------------------------------------------------------------ */
/* Qualified store/fetch                                                */
/* ------------------------------------------------------------------ */

/* Regression for the pmix_hash_store qualifier-array construction bug:
 * when the info array mixes non-qualifier and qualifier entries, the
 * qualifier's index was written to the wrong (out-of-bounds) slot, so the
 * stored qualifier index was left uninitialized and a later qualified
 * fetch failed to match (and the OOB write corrupted the heap). Store a
 * value qualified by a marked qualifier that is preceded by a
 * non-qualifier info, then confirm a matching qualified fetch finds it. */
static void test_store_fetch_qualified(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv = make_kval_str("unit.qkey", "qvalue");
    pmix_info_t quals[2];
    pmix_info_t fq[1];
    pmix_list_t kvals;
    pmix_status_t rc;
    uint32_t plain = 7, qval = 42;

    /* quals[0] is an ordinary (non-qualifier) info; quals[1] is the
     * actual qualifier - this ordering is what triggered the bug. */
    PMIX_INFO_LOAD(&quals[0], "unit.plain.directive", &plain, PMIX_UINT32);
    PMIX_INFO_LOAD(&quals[1], "unit.qualifier", &qval, PMIX_UINT32);
    PMIX_INFO_SET_QUALIFIER(&quals[1]);

    rc = pmix_hash_store(t, 0, kv, quals, 2, NULL);
    PMIX_RELEASE(kv);
    report("store_qualified: store returns SUCCESS", PMIX_SUCCESS == rc);

    /* fetch qualified by the same key/value must match */
    PMIX_INFO_LOAD(&fq[0], "unit.qualifier", &qval, PMIX_UINT32);
    PMIX_INFO_SET_QUALIFIER(&fq[0]);

    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.qkey", fq, 1, &kvals, NULL);
    report("store_qualified: qualified fetch matches",
           PMIX_SUCCESS == rc && !pmix_list_is_empty(&kvals));
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);

    /* fetch qualified by a different value must NOT match */
    qval = 99;
    PMIX_INFO_DESTRUCT(&fq[0]);
    PMIX_INFO_LOAD(&fq[0], "unit.qualifier", &qval, PMIX_UINT32);
    PMIX_INFO_SET_QUALIFIER(&fq[0]);
    PMIX_CONSTRUCT(&kvals, pmix_list_t);
    rc = pmix_hash_fetch(t, 0, "unit.qkey", fq, 1, &kvals, NULL);
    report("store_qualified: mismatched qualifier not found",
           PMIX_ERR_NOT_FOUND == rc);
    drain_list(&kvals);
    PMIX_DESTRUCT(&kvals);

    PMIX_INFO_DESTRUCT(&quals[0]);
    PMIX_INFO_DESTRUCT(&quals[1]);
    PMIX_INFO_DESTRUCT(&fq[0]);
    pmix_hash_remove_data(t, 0, NULL, NULL);
    PMIX_RELEASE(t);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* keys are released when nothing stored refers to them                */
/* ------------------------------------------------------------------ */

static uint32_t key_id(const char *key)
{
    pmix_regattr_input_t *p = pmix_hash_find_key(UINT32_MAX, key, NULL);
    return (NULL == p) ? UINT32_MAX : p->index;
}

/* A key that is not a reserved attribute is counted: once for each value
 * stored under it and each qualifier that names it. When the last of
 * those goes, the key leaves the dictionary and its number is handed to
 * the next key registered - which must then read back as itself. */
static void test_key_release(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv;
    pmix_info_t qual;
    uint32_t id, id2, qid, qv = 3;

    kv = make_kval_str("unit.kr.a", "one");
    (void) pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    (void) pmix_hash_store(t, 1, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);
    id = key_id("unit.kr.a");
    report("key_release: a key stored twice is counted twice",
           UINT32_MAX != id && 2 == pmix_hash_key_refs(id));

    (void) pmix_hash_remove_data(t, 0, "unit.kr.a", NULL);
    report("key_release: removing one value leaves the key",
           id == key_id("unit.kr.a") && 1 == pmix_hash_key_refs(id));
    (void) pmix_hash_remove_data(t, 1, "unit.kr.a", NULL);
    report("key_release: removing the last value releases the key",
           UINT32_MAX == key_id("unit.kr.a"));

    kv = make_kval_str("unit.kr.c", "three");
    (void) pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);
    id2 = key_id("unit.kr.c");
    report("key_release: the next key takes the freed number",
           id2 == id && 1 == pmix_hash_key_refs(id2));
    {
        pmix_list_t kvals;
        pmix_kval_t *got;
        pmix_status_t rc;

        PMIX_CONSTRUCT(&kvals, pmix_list_t);
        rc = pmix_hash_fetch(t, 0, "unit.kr.c", NULL, 0, &kvals, NULL);
        got = (pmix_kval_t *) pmix_list_get_first(&kvals);
        report("key_release: and reads back as itself",
               PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&kvals) &&
                   0 == strcmp(got->key, "unit.kr.c") &&
                   PMIX_STRING == got->value->type &&
                   0 == strcmp(got->value->data.string, "three"));
        drain_list(&kvals);
        PMIX_DESTRUCT(&kvals);
        PMIX_CONSTRUCT(&kvals, pmix_list_t);
        rc = pmix_hash_fetch(t, 0, "unit.kr.a", NULL, 0, &kvals, NULL);
        report("key_release: while the released key finds nothing",
               PMIX_SUCCESS != rc && pmix_list_is_empty(&kvals));
        drain_list(&kvals);
        PMIX_DESTRUCT(&kvals);
    }
    kv = make_kval_str("unit.kr.a", "again");
    (void) pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);
    report("key_release: the released key can be stored again",
           UINT32_MAX != key_id("unit.kr.a") && id2 != key_id("unit.kr.a") &&
               1 == pmix_hash_key_refs(key_id("unit.kr.a")));

    /* a qualifier names its key too */
    kv = make_kval_str("unit.kr.b", "two");
    PMIX_INFO_LOAD(&qual, "unit.kr.qual", &qv, PMIX_UINT32);
    PMIX_INFO_SET_QUALIFIER(&qual);
    (void) pmix_hash_store(t, 0, kv, &qual, 1, NULL);
    PMIX_RELEASE(kv);
    PMIX_INFO_DESTRUCT(&qual);
    qid = key_id("unit.kr.qual");
    report("key_release: a qualifier's key is counted",
           UINT32_MAX != qid && 1 == pmix_hash_key_refs(qid));

    /* reserved attributes are not counted, and stay */
    kv = make_kval_str(PMIX_HOSTNAME, "host");
    (void) pmix_hash_store(t, 0, kv, NULL, 0, NULL);
    PMIX_RELEASE(kv);
    report("key_release: a reserved attribute is not counted",
           UINT32_MAX == pmix_hash_key_refs(key_id(PMIX_HOSTNAME)));

    /* removing everything - what an owner does before it releases the
     * table, which does not own the records it points at - releases
     * every key stored in it */
    (void) pmix_hash_remove_data(t, PMIX_RANK_WILDCARD, NULL, NULL);
    PMIX_RELEASE(t);
    report("key_release: removing all data releases its keys",
           UINT32_MAX == key_id("unit.kr.a") && UINT32_MAX == key_id("unit.kr.b") &&
               UINT32_MAX == key_id("unit.kr.qual"));
    report("key_release: and leaves the reserved attribute",
           UINT32_MAX != key_id(PMIX_HOSTNAME));
}

/* Keys that come and go one at a time keep handing the same numbers
 * round, so the index does not grow with how many there have been. */
static void test_key_churn(void)
{
    pmix_hash_table_t *t = new_table();
    pmix_kval_t *kv;
    char key[64];
    uint32_t id, lowest = UINT32_MAX, highest = 0;
    int n;

    for (n = 0; n < 1000; n++) {
        snprintf(key, sizeof(key), "unit.kr.churn.%d", n);
        kv = make_kval_str(key, "x");
        (void) pmix_hash_store(t, 0, kv, NULL, 0, NULL);
        PMIX_RELEASE(kv);
        id = key_id(key);
        if (id < lowest) {
            lowest = id;
        }
        if (UINT32_MAX != id && id > highest) {
            highest = id;
        }
        (void) pmix_hash_remove_data(t, 0, key, NULL);
    }
    report("key_churn: 1000 keys stored and removed in turn reuse their numbers",
           UINT32_MAX != lowest && highest - lowest < 4);
    (void) pmix_hash_remove_data(t, PMIX_RANK_WILDCARD, NULL, NULL);
    PMIX_RELEASE(t);
}

/* What a persistent DVM needs: the keys a job stored go when the job is
 * deregistered. */
static void dereg_cb(pmix_status_t status, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    lock->status = status;
    PMIX_WAKEUP_THREAD(lock);
}

static void test_job_keys_released(void)
{
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_value_t val;
    pmix_lock_t lock;
    pmix_status_t rc;
    uint32_t u = 5;

    PMIX_LOAD_NSPACE(ns, "unit-kr-job");
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        report("job_keys: register", 0);
        return;
    }
    PMIX_LOAD_PROCID(&proc, "unit-kr-job", 0);
    PMIX_VALUE_LOAD(&val, &u, PMIX_UINT32);
    rc = PMIx_Store_internal(&proc, "unit.kr.job.key", &val);
    PMIX_VALUE_DESTRUCT(&val);
    report("job_keys: a job's key is registered when stored",
           PMIX_SUCCESS == rc && UINT32_MAX != key_id("unit.kr.job.key"));

    PMIX_CONSTRUCT_LOCK(&lock);
    PMIx_server_deregister_nspace(ns, dereg_cb, &lock);
    PMIX_WAIT_THREAD(&lock);
    PMIX_DESTRUCT_LOCK(&lock);
    report("job_keys: it is released when the job is deregistered",
           UINT32_MAX == key_id("unit.kr.job.key"));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_hash unit tests ===\n\n");

    test_lookup_known_key();
    test_lookup_auto_register();
    test_find_key_does_not_register();
    test_fetch_does_not_register();
    test_store_fetch_basic();
    test_fetch_not_found_empty_table();
    test_fetch_wrong_key();
    test_store_overwrite();
    test_remove_then_fetch();
    test_remove_wildcard_empties_the_table();
    test_multiple_ranks();
    test_store_fetch_qualified();
    test_key_release();
    test_key_churn();
    test_job_keys_released();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
