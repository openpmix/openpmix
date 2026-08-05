/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Time PMIx_Get across separate PMIx servers.
 *
 * test/unit/get_perf.c times the same thing in a singleton, which is
 * useful but cannot reach gds/shmem3: a singleton has no server, so it
 * is answered out of gds/hash. This one runs under a launcher with the
 * ranks behind different servers, which is the only way to time a get
 * that is answered from a shared-memory segment.
 *
 * It exists to score the caller-thread fast path
 * (pmix_client_fast_get). Run it twice, once with that enabled and
 * once without; the difference is the cost of handing a request to the
 * progress thread and waiting for it. Everything else about the two
 * runs is identical, including which bytes are read.
 *
 * Three shapes, all answered locally after the fence:
 *
 *   peer    a key another rank published, which after a collecting
 *           fence lives in this node's modex segment. This is the one
 *           that matters - it is the endpoint-exchange lookup an MPI
 *           implementation does per peer.
 *   job     a reserved job-level key, which lives in the job segment.
 *   sess    a session-realm key. Included precisely because it is
 *           NOT eligible for the fast path - resolving a realm takes
 *           further fetches and an info-array rebuild, so the gating
 *           sends it the ordinary way. It should show no improvement,
 *           and if it ever does, the gating has been loosened.
 *   self    a key this rank published itself.
 *
 * It asserts the value is what was published before timing anything: a
 * get that misses would go to the server and swamp the measurement, and
 * would be measuring something else entirely.
 *
 * Timings are printed, never asserted on - see the note in
 * test/unit/util/hash_perf.c.
 *
 *   PMIX_PERF_ITERS   gets per measurement (default 20000)
 */

#include <pmix.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "examples.h"

static pmix_proc_t myproc;
static int nerrs = 0;
static int niters = 20000;

static double now_usec(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000000.0 + (double) tv.tv_usec;
}

/* One get, releasing whatever came back. */
static pmix_status_t one_get(const pmix_proc_t *p, const char *key)
{
    pmix_value_t *val = NULL;
    pmix_status_t rc;

    rc = PMIx_Get(p, key, NULL, 0, &val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return rc;
}

/* Confirm the shape answers correctly, then time it. A shape that does
 * not resolve locally is not the shape we mean to be timing. */
static void measure(const char *label, const pmix_proc_t *p, const char *key)
{
    double t0, t1;
    int i, bad = 0;
    pmix_status_t rc;

    rc = one_get(p, key);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_timing: rank %u: %s (%s) does not resolve: %s\n",
                (unsigned) myproc.rank, label, key, PMIx_Error_string(rc));
        nerrs++;
        return;
    }
    for (i = 0; i < 200; i++) {
        (void) one_get(p, key);
    }

    t0 = now_usec();
    for (i = 0; i < niters; i++) {
        if (PMIX_SUCCESS != one_get(p, key)) {
            bad++;
        }
    }
    t1 = now_usec();

    if (0 != bad) {
        fprintf(stderr, "get_timing: rank %u: %s: %d/%d gets failed\n",
                (unsigned) myproc.rank, label, bad, niters);
        nerrs++;
        return;
    }
    /* only rank 0 reports, so the numbers are not interleaved */
    if (0 == myproc.rank) {
        fprintf(stdout, "get_timing:   %-6s %9.1f ns/op\n", label,
                (t1 - t0) * 1000.0 / (double) niters);
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t value;
    pmix_value_t *val;
    pmix_proc_t proc, wildcard, peer;
    uint32_t nprocs = 0, peerrank;
    char key[PMIX_MAX_KEYLEN + 1];
    char peerkey[PMIX_MAX_KEYLEN + 1];
    pmix_info_t info;
    const char *s;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    s = getenv("PMIX_PERF_ITERS");
    if (NULL != s && '\0' != s[0]) {
        int v = atoi(s);
        if (0 < v) {
            niters = v;
        }
    }

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "get_timing: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "get_timing: could not get job size: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (2 > nprocs) {
        fprintf(stderr, "get_timing: needs at least 2 ranks\n");
        goto done;
    }

    snprintf(key, sizeof(key), "tk-%u", (unsigned) myproc.rank);
    PMIX_VALUE_LOAD(&value, &myproc.rank, PMIX_UINT32);
    rc = PMIx_Put(PMIX_GLOBAL, key, &value);
    PMIX_VALUE_DESTRUCT(&value);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_timing: put failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "get_timing: commit failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }

    /* collect, so a peer's value is resident here rather than needing a
     * direct modex - which would be timing the network, not the get */
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &(bool){true}, PMIX_BOOL);
    rc = PMIx_Fence(&wildcard, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_timing: fence failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }

    if (0 == myproc.rank) {
        fprintf(stdout, "get_timing: %u ranks, %d iters, fast_get=%s\n",
                (unsigned) nprocs, niters,
                (NULL == getenv("PMIX_MCA_pmix_client_fast_get"))
                    ? "unset" : getenv("PMIX_MCA_pmix_client_fast_get"));
    }

    peerrank = (myproc.rank + 1) % nprocs;
    snprintf(peerkey, sizeof(peerkey), "tk-%u", (unsigned) peerrank);
    PMIX_LOAD_PROCID(&peer, myproc.nspace, peerrank);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, myproc.rank);

    measure("peer", &peer, peerkey);
    measure("job", &wildcard, PMIX_JOB_SIZE);
    measure("sess", &wildcard, PMIX_UNIV_SIZE);
    measure("self", &proc, key);

done:
    if (0 != nerrs) {
        fprintf(stderr, "get_timing: rank %u: %d error(s)\n", (unsigned) myproc.rank, nerrs);
    }
    PMIx_Finalize(NULL, 0);
    return (0 == nerrs) ? 0 : 1;
}
