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
    while (!pmix_list_is_empty(lst)) {
        kv = (pmix_kval_t *) pmix_list_remove_first(lst);
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

/* ------------------------------------------------------------------ */
/* pmix_hash_store / pmix_hash_fetch                                   */
/* ------------------------------------------------------------------ */

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
    test_store_fetch_basic();
    test_fetch_not_found_empty_table();
    test_fetch_wrong_key();
    test_store_overwrite();
    test_remove_then_fetch();
    test_multiple_ranks();
    test_store_fetch_qualified();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
