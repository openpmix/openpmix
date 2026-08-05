/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Micro-benchmark for the pmix_hash datastore that backs PMIx_Get.
 *
 * This drives pmix_hash_store/pmix_hash_fetch directly, with no event
 * loop and no server round trip, so it isolates the datastore lookup
 * from everything else on the get path.
 *
 * It asserts CORRECTNESS ONLY and never on elapsed time: a timing
 * assertion on a shared CI machine goes flaky, and the next person
 * "fixes" it by loosening the bound until it means nothing. The
 * timings are printed instead, so a regression shows up in a diff of
 * two "make check" logs.
 *
 * The shapes measured are the ones the get path actually takes:
 *
 *   hit/rank     a key this rank stored          - the common get:
 *                                                  lookup AND the deep
 *                                                  copy out of the table
 *   miss/rank    a registered key this rank does - lookup only, no copy;
 *                not hold                         scans the rank's whole
 *                                                  per-key store
 *   hit/undef    PMIX_RANK_UNDEF, key present    - scans ranks until found
 *   miss/undef   PMIX_RANK_UNDEF, registered key - the worst case: scans
 *                held by nobody                    EVERY rank's store
 *   miss/unreg   a key nobody ever stored        - the early-out in
 *                                                  pmix_hash_fetch, which
 *                                                  answers without
 *                                                  touching a table
 *
 * Read them against each other rather than in isolation. "hit/rank"
 * minus "miss/rank" is roughly what make_copy costs; "miss/undef"
 * divided by "miss/rank" is roughly the rank fan-out; and "miss/unreg"
 * is the floor, since it does no work at all.
 *
 * Note that "miss/rank" and "miss/undef" need a key that is REGISTERED
 * but stored nowhere useful - a key nobody ever stored short-circuits
 * before any table is walked, which is the "miss/unreg" shape and not
 * the scan being measured here.
 *
 * Requires PMIx_server_init because pmix_hash_store copies values via
 * pmix_bfrops_base_tma_copy_value, which needs the bfrops MCA
 * framework. Same requirement as util_hash.c.
 *
 * Tunable through the environment for a real measurement run:
 *   PMIX_PERF_NRANKS  ranks in the table        (default 64)
 *   PMIX_PERF_KEYS    keys stored per rank      (default 8)
 *   PMIX_PERF_ITERS   fetches per measurement   (default 20000)
 *
 * Exit 0 if all correctness checks pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "include/pmix_server.h"
#include "pmix.h"
#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/util/pmix_hash.h"

static int npass = 0;
static int nfail = 0;

/* Defaults chosen so the whole program runs in a fraction of a second
 * under "make check"; override through the environment to measure. */
static int nranks = 64;
static int nkeys = 8;
static int niters = 20000;

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

/* Read a positive integer from the environment, or keep the default. */
static void envint(const char *name, int *slot)
{
    const char *s = getenv(name);
    long v;
    char *end;

    if (NULL == s || '\0' == s[0]) {
        return;
    }
    v = strtol(s, &end, 10);
    if ('\0' != *end || 0 >= v || INT_MAX < v) {
        fprintf(stderr, "ignoring bad %s=\"%s\"\n", name, s);
        return;
    }
    *slot = (int) v;
}

static double now_usec(void)
{
    struct timeval tv;

    /* gettimeofday rather than clock_gettime: it is what the rest of
     * test/ uses, and microsecond resolution amortized over niters is
     * far finer than anything measured here. */
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000000.0 + (double) tv.tv_usec;
}

/* "unit.perf.rNNN.kNNN" - distinct per (rank, key) so that a fetch for
 * one rank's key against another rank is a genuine miss on a key that
 * IS registered in the keyindex. A key that was never stored anywhere
 * short-circuits in pmix_hash_fetch before any table is walked, which
 * is not the path being measured. */
static void makekey(char *buf, size_t sz, int rank, int key)
{
    snprintf(buf, sz, "unit.perf.r%d.k%d", rank, key);
}

/* Populate a table with nranks x nkeys string values. */
static pmix_status_t fill(pmix_hash_table_t *t)
{
    char key[PMIX_MAX_KEYLEN + 1];
    pmix_kval_t *kv;
    pmix_status_t rc;
    int r, k;

    for (r = 0; r < nranks; r++) {
        for (k = 0; k < nkeys; k++) {
            makekey(key, sizeof(key), r, k);
            PMIX_KVAL_NEW(kv, key);
            if (NULL == kv) {
                return PMIX_ERR_NOMEM;
            }
            memset(kv->value, 0, sizeof(pmix_value_t));
            kv->value->type = PMIX_STRING;
            kv->value->data.string = strdup(key);
            if (NULL == kv->value->data.string) {
                PMIX_RELEASE(kv);
                return PMIX_ERR_NOMEM;
            }
            /* hash_store copies, so the kval stays ours to release */
            rc = pmix_hash_store(t, (pmix_rank_t) r, kv, NULL, 0, NULL);
            PMIX_RELEASE(kv);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

/* Drain and release everything a fetch appended. Taking the removal
 * result as the loop condition is deliberate - see the note in
 * util_hash.c on -Wstringop-overflow. */
static void drain_list(pmix_list_t *lst)
{
    pmix_kval_t *kv;

    while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(lst))) {
        PMIX_RELEASE(kv);
    }
}

/* One fetch, draining whatever it returned. Returns the status so the
 * caller can confirm the shape it thinks it is measuring. */
static pmix_status_t one_fetch(pmix_hash_table_t *t, pmix_rank_t rank, const char *key)
{
    pmix_list_t kvs;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&kvs, pmix_list_t);
    rc = pmix_hash_fetch(t, rank, key, NULL, 0, &kvs, NULL);
    drain_list(&kvs);
    PMIX_DESTRUCT(&kvs);
    return rc;
}

/* "which rank holds this key?", the two ways.
 *
 * search_loop() is what gds/hash used to do for a PMIX_RANK_UNDEF
 * request: ask every rank in turn and take the first that answers. It
 * is reproduced here rather than called, because the point is to time
 * it against its replacement on identical data.
 *
 * search_helper() is pmix_hash_fetch_lowest_rank(), which walks the
 * entries the table actually holds. Both return the lowest-numbered
 * match, so a run must agree on the answer as well as be faster - the
 * caller checks that too. */
static pmix_status_t search_loop(pmix_hash_table_t *t, pmix_rank_t maxrank, const char *key)
{
    pmix_list_t kvs;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;
    pmix_rank_t r;

    for (r = 0; r < maxrank; r++) {
        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rc = pmix_hash_fetch(t, r, key, NULL, 0, &kvs, NULL);
        drain_list(&kvs);
        PMIX_DESTRUCT(&kvs);
        if (PMIX_SUCCESS == rc) {
            return rc;
        }
    }
    return rc;
}

static pmix_status_t search_helper(pmix_hash_table_t *t, pmix_rank_t maxrank, const char *key)
{
    pmix_list_t kvs;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&kvs, pmix_list_t);
    rc = pmix_hash_fetch_lowest_rank(t, maxrank, key, NULL, 0, &kvs, NULL);
    drain_list(&kvs);
    PMIX_DESTRUCT(&kvs);
    return rc;
}

/* Time one of the two search forms. */
static void measure_search(const char *label, pmix_hash_table_t *t, pmix_rank_t maxrank,
                           const char *key, pmix_status_t want, bool helper)
{
    pmix_status_t rc;
    double t0, t1;
    int i, bad = 0;

    for (i = 0; i < 100; i++) {
        (void) (helper ? search_helper(t, maxrank, key) : search_loop(t, maxrank, key));
    }

    t0 = now_usec();
    for (i = 0; i < niters; i++) {
        rc = helper ? search_helper(t, maxrank, key) : search_loop(t, maxrank, key);
        if (want != rc) {
            bad++;
        }
    }
    t1 = now_usec();

    fprintf(stdout, "  %-12s %8.1f ns/op\n", label, (t1 - t0) * 1000.0 / (double) niters);
    report(label, 0 == bad);
}

/* Time niters fetches of one shape and print ns/op.
 *
 * "want" is the status every iteration must return; a shape that does
 * not hold is not the shape being timed, so it is reported as a
 * failure rather than silently mistimed. */
static void measure(const char *label, pmix_hash_table_t *t, pmix_rank_t rank, const char *key,
                    pmix_status_t want)
{
    pmix_status_t rc;
    double t0, t1;
    int i, bad = 0;

    /* warm the tables and the keyindex lookup before timing */
    for (i = 0; i < 100; i++) {
        (void) one_fetch(t, rank, key);
    }

    t0 = now_usec();
    for (i = 0; i < niters; i++) {
        rc = one_fetch(t, rank, key);
        if (want != rc) {
            bad++;
        }
    }
    t1 = now_usec();

    fprintf(stdout, "  %-12s %8.1f ns/op\n", label, (t1 - t0) * 1000.0 / (double) niters);
    report(label, 0 == bad);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_hash_table_t *t;
    char key[PMIX_MAX_KEYLEN + 1];
    char lastkey[PMIX_MAX_KEYLEN + 1];
    char absent[PMIX_MAX_KEYLEN + 1];
    char unreg[PMIX_MAX_KEYLEN + 1];
    pmix_hash_table_t *sparse;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    envint("PMIX_PERF_NRANKS", &nranks);
    envint("PMIX_PERF_KEYS", &nkeys);
    envint("PMIX_PERF_ITERS", &niters);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_hash fetch timings ===\n");
    fprintf(stdout, "  nranks=%d keys/rank=%d iters=%d\n\n", nranks, nkeys, niters);

    t = PMIX_NEW(pmix_hash_table_t, NULL);
    if (NULL == t) {
        fprintf(stderr, "table allocation failed\n");
        PMIx_server_finalize();
        return 1;
    }
    pmix_hash_table_init(t, 256);

    rc = fill(t);
    report("fill: stored every key", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "fill failed: %s\n", PMIx_Error_string(rc));
        PMIX_RELEASE(t);
        PMIx_server_finalize();
        return 1;
    }

    /* A key rank 0 stored: found on rank 0, absent from every other. */
    makekey(key, sizeof(key), 0, 0);

    /* A key that is REGISTERED but held by nobody. Store it, then
     * remove it: removal drops the value but leaves the key in the
     * keyindex, so a later fetch gets past the never-registered
     * early-out and has to walk the tables to come up empty. That is
     * the shape the scan changes are about. */
    snprintf(absent, sizeof(absent), "unit.perf.registered.absent");
    {
        pmix_kval_t *kv;

        PMIX_KVAL_NEW(kv, absent);
        if (NULL == kv) {
            fprintf(stderr, "kval allocation failed\n");
            PMIX_RELEASE(t);
            PMIx_server_finalize();
            return 1;
        }
        memset(kv->value, 0, sizeof(pmix_value_t));
        kv->value->type = PMIX_STRING;
        kv->value->data.string = strdup(absent);
        rc = pmix_hash_store(t, 0, kv, NULL, 0, NULL);
        PMIX_RELEASE(kv);
        report("setup: stored the soon-to-be-absent key", PMIX_SUCCESS == rc);
        rc = pmix_hash_remove_data(t, 0, absent, NULL);
        report("setup: removed it again", PMIX_SUCCESS == rc);
        /* it must now miss everywhere, or the two miss shapes below
         * are not measuring a miss at all */
        report("setup: registered-absent key really misses",
               PMIX_ERR_NOT_FOUND == one_fetch(t, PMIX_RANK_UNDEF, absent));
    }

    /* A key nobody ever stored, for the early-out floor. */
    snprintf(unreg, sizeof(unreg), "unit.perf.never.stored");

    measure("hit/rank", t, 0, key, PMIX_SUCCESS);
    measure("miss/rank", t, 0, absent, PMIX_ERR_NOT_FOUND);
    measure("hit/undef", t, PMIX_RANK_UNDEF, key, PMIX_SUCCESS);
    measure("miss/undef", t, PMIX_RANK_UNDEF, absent, PMIX_ERR_NOT_FOUND);
    measure("miss/unreg", t, PMIX_RANK_UNDEF, unreg, PMIX_ERR_NOT_FOUND);

    /* "which rank holds this key?" - the per-rank loop gds/hash used to
     * run for a PMIX_RANK_UNDEF request, against the single walk that
     * replaced it. Measured on a hit at the LAST rank and on a miss,
     * which are the two shapes where the loop cannot stop early.
     *
     * Note this is the case least favourable to the new form: every
     * rank has an entry here, so both walk the same number of them and
     * the difference is only the per-call overhead the loop repeats.
     * The sparse case below is the one gds/hash actually meets. */
    makekey(lastkey, sizeof(lastkey), nranks - 1, 0);
    measure_search("search-loop/hit", t, (pmix_rank_t) nranks, lastkey, PMIX_SUCCESS, false);
    measure_search("search-new/hit", t, (pmix_rank_t) nranks, lastkey, PMIX_SUCCESS, true);
    measure_search("search-loop/miss", t, (pmix_rank_t) nranks, absent, PMIX_ERR_NOT_FOUND, false);
    measure_search("search-new/miss", t, (pmix_rank_t) nranks, absent, PMIX_ERR_NOT_FOUND, true);

    /* Faster is worthless if it answers differently. Both forms must
     * name the same rank when more than one holds the key. */
    {
        pmix_kval_t *kv;
        pmix_list_t kvs;
        pmix_status_t rl, rh;
        char shared[PMIX_MAX_KEYLEN + 1];
        int r;

        snprintf(shared, sizeof(shared), "unit.perf.shared.key");
        for (r = nranks - 1; 0 <= r; r--) {
            /* stored high rank first, so bucket order and rank order
             * are unlikely to agree */
            PMIX_KVAL_NEW(kv, shared);
            if (NULL == kv) {
                break;
            }
            memset(kv->value, 0, sizeof(pmix_value_t));
            kv->value->type = PMIX_UINT32;
            kv->value->data.uint32 = (uint32_t) r;
            (void) pmix_hash_store(t, (pmix_rank_t) r, kv, NULL, 0, NULL);
            PMIX_RELEASE(kv);
        }

        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rh = pmix_hash_fetch_lowest_rank(t, (pmix_rank_t) nranks, shared, NULL, 0, &kvs, NULL);
        kv = (pmix_kval_t *) pmix_list_get_first(&kvs);
        report("lowest-rank search returns rank 0's value",
               PMIX_SUCCESS == rh && NULL != kv && NULL != kv->value
                   && PMIX_UINT32 == kv->value->type && 0 == kv->value->data.uint32);
        drain_list(&kvs);
        PMIX_DESTRUCT(&kvs);

        rl = search_loop(t, (pmix_rank_t) nranks, shared);
        report("both search forms agree on the status", rl == rh);

        /* a search bounded below the ranks that hold it finds nothing */
        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rh = pmix_hash_fetch_lowest_rank(t, 0, shared, NULL, 0, &kvs, NULL);
        report("a zero maxrank matches nothing", PMIX_ERR_NOT_FOUND == rh);
        drain_list(&kvs);
        PMIX_DESTRUCT(&kvs);

        /* NULL key is not a lowest-rank question */
        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rh = pmix_hash_fetch_lowest_rank(t, (pmix_rank_t) nranks, NULL, NULL, 0, &kvs, NULL);
        report("a NULL key is refused", PMIX_ERR_BAD_PARAM == rh);
        drain_list(&kvs);
        PMIX_DESTRUCT(&kvs);
    }

    /* The shape gds/hash actually meets. A PMIx_Get with no proc walks
     * three tables - internal, local, remote - and on a client that has
     * not fenced, two of them are empty or nearly so while the job may
     * have thousands of ranks. The loop asks such a table for every one
     * of them; the walk sees only what is there. */
    sparse = PMIX_NEW(pmix_hash_table_t, NULL);
    if (NULL != sparse) {
        pmix_kval_t *kv;

        pmix_hash_table_init(sparse, 256);
        /* two ranks' worth of data in a job that claims "nranks" of them */
        PMIX_KVAL_NEW(kv, key);
        if (NULL != kv) {
            memset(kv->value, 0, sizeof(pmix_value_t));
            kv->value->type = PMIX_UINT32;
            kv->value->data.uint32 = 0;
            (void) pmix_hash_store(sparse, 0, kv, NULL, 0, NULL);
            PMIX_RELEASE(kv);
        }

        fprintf(stdout, "  (sparse table: 1 rank populated, maxrank=%d)\n", nranks);
        measure_search("sparse-loop/miss", sparse, (pmix_rank_t) nranks, absent,
                       PMIX_ERR_NOT_FOUND, false);
        measure_search("sparse-new/miss", sparse, (pmix_rank_t) nranks, absent,
                       PMIX_ERR_NOT_FOUND, true);

        pmix_hash_remove_data(sparse, PMIX_RANK_WILDCARD, NULL, NULL);
        PMIX_RELEASE(sparse);
    }

    /* The value has to survive all of that, or the timings above were
     * measuring a lookup that does not return what a get would. */
    {
        pmix_list_t kvs;
        pmix_kval_t *kv;
        int ok;

        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rc = pmix_hash_fetch(t, 0, key, NULL, 0, &kvs, NULL);
        kv = (pmix_kval_t *) pmix_list_get_first(&kvs);
        ok = (PMIX_SUCCESS == rc && NULL != kv && NULL != kv->value
              && PMIX_STRING == kv->value->type && NULL != kv->value->data.string
              && 0 == strcmp(kv->value->data.string, key));
        report("hit/rank returns the stored value", ok);
        drain_list(&kvs);
        PMIX_DESTRUCT(&kvs);
    }

    /* WILDCARD with a NULL key is the "apply to every rank entry" case,
     * which releases each proc_data. The table destructor only removes
     * the elements, so without this the stored values leak. */
    pmix_hash_remove_data(t, PMIX_RANK_WILDCARD, NULL, NULL);
    PMIX_RELEASE(t);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
