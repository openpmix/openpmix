/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Time a *collecting* PMIx_Fence across separate PMIx servers.
 *
 * This is the write-side companion to examples/get_timing.c. That one
 * scores the read path - a PMIx_Get answered out of a gds/shmem3
 * segment; this one scores the path that builds the segment in the
 * first place, which is where the server spends the fence.
 *
 * It exists for openpmix#4141: a collecting fence with no data costs
 * about what a barrier costs, so the premium is entirely the modex, and
 * the modex is mostly the datastore building its in-segment structures.
 * Comparing gds modules is the point, so run it twice:
 *
 *   prterun ... ./fence_timing                       # whatever is default
 *   prterun --pmixmca gds hash ... ./fence_timing    # forced onto hash
 *
 * The measurement is the FIRST collecting fence, deliberately. Under a
 * cumulative commit every later fence re-ships the whole local store, so
 * later rounds measure a growing payload rather than the same one twice
 * - and one collecting fence is all most MPI jobs ever do.
 *
 * A barrier is timed first, on the same geometry and the same processes,
 * as the baseline to subtract: what is left is what carrying the data
 * cost.
 *
 * Run one rank per node (--map-by node with one slot each). With two
 * ranks on a node half the traffic never leaves the server and the
 * number means something else.
 *
 * Timings are printed, never asserted on - see the note in
 * test/unit/util/hash_perf.c.
 *
 *   PMIX_PERF_KEYS    keys published per rank (default 32)
 *   PMIX_PERF_VALSZ   bytes in each value     (default 1024)
 *   PMIX_PERF_COMPRESSIBLE  1 to fill values with repetitive data instead
 *                     of random (default 0)
 *
 * That last one exists because whether compressing the modex pays is the
 * question, not a detail: pcompress decides on size alone and never looks
 * at the data, so an incompressible payload costs the full compression and
 * returns nothing. Measure both ends before concluding anything about a
 * codec.
 */

#include <pmix.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "examples.h"

static pmix_proc_t myproc;

static double now_usec(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000000.0 + (double) tv.tv_usec;
}

static int env_int(const char *name, int dflt)
{
    const char *s = getenv(name);
    int v;

    if (NULL == s) {
        return dflt;
    }
    v = atoi(s);
    return (0 < v) ? v : dflt;
}

/* Fill with a cheap PRNG rather than a constant. A compressible payload
 * is a different measurement: pcompress would collapse it on the wire and
 * the segment would be built from a fraction of the bytes we think we
 * are sending. */
static void fill_incompressible(char *buf, size_t len, unsigned seed)
{
    uint32_t x = 0x9e3779b9u ^ seed;
    size_t i;

    for (i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (char) (x & 0xff);
    }
}

/* The other end of the range: a short repeating pattern, which any of the
 * codecs collapses. Real modex values sit somewhere between this and the
 * PRNG above - repetitive key names wrapped around opaque endpoint bytes. */
static void fill_compressible(char *buf, size_t len, unsigned seed)
{
    static const char pattern[] = "pmix.endpoint.fabric.device.coordinates=0123456789 ";
    size_t i, n = sizeof(pattern) - 1;

    for (i = 0; i < len; i++) {
        buf[i] = pattern[(i + seed) % n];
    }
}

/* A fence over the whole namespace, optionally collecting the data. */
static pmix_status_t fence_all(bool collect, double *usec)
{
    pmix_proc_t wildcard;
    pmix_info_t info;
    pmix_status_t rc;
    double t0;
    bool flag = true;

    PMIX_PROC_CONSTRUCT(&wildcard);
    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);

    t0 = now_usec();
    if (collect) {
        PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
        rc = PMIx_Fence(&wildcard, 1, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
    } else {
        rc = PMIx_Fence(&wildcard, 1, NULL, 0);
    }
    if (NULL != usec) {
        *usec = now_usec() - t0;
    }
    return rc;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t value;
    pmix_value_t *val = NULL;
    pmix_proc_t wildcard;
    char key[PMIX_MAX_KEYLEN + 1];
    char *payload = NULL;
    double barrier_us = 0.0, modex_us = 0.0;
    int nkeys, valsz, i;
    bool compressible;
    uint32_t nprocs = 0;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    nkeys = env_int("PMIX_PERF_KEYS", 32);
    valsz = env_int("PMIX_PERF_VALSZ", 1024);
    compressible = (NULL != getenv("PMIX_PERF_COMPRESSIBLE")
                    && 0 != strcmp("0", getenv("PMIX_PERF_COMPRESSIBLE")));

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "fence_timing: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_PROC_CONSTRUCT(&wildcard);
    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS == PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val) && NULL != val) {
        nprocs = val->data.uint32;
        PMIX_VALUE_RELEASE(val);
    }

    payload = (char *) malloc((size_t) valsz);
    if (NULL == payload) {
        fprintf(stderr, "fence_timing: out of memory\n");
        goto done;
    }

    /* Line the ranks up so neither timed fence is really measuring the
     * skew in how they got here. */
    if (PMIX_SUCCESS != (rc = fence_all(false, NULL))) {
        fprintf(stderr, "fence_timing: sync fence failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    /* Baseline: the same collective carrying nothing. */
    if (PMIX_SUCCESS != (rc = fence_all(false, &barrier_us))) {
        fprintf(stderr, "fence_timing: barrier failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    /* Publish. Untimed: PMIx_Put is a local store, and PMIx_Commit ships
     * it to our own server - neither is the collective we are scoring. */
    for (i = 0; i < nkeys; i++) {
        snprintf(key, sizeof(key), "fence-timing-%u-%d", (unsigned) myproc.rank, i);
        if (compressible) {
            fill_compressible(payload, (size_t) valsz,
                              (unsigned) (myproc.rank * 1000003u + (unsigned) i));
        } else {
            fill_incompressible(payload, (size_t) valsz,
                                (unsigned) (myproc.rank * 1000003u + (unsigned) i));
        }
        PMIX_VALUE_CONSTRUCT(&value);
        value.type = PMIX_BYTE_OBJECT;
        value.data.bo.bytes = payload;
        value.data.bo.size = (size_t) valsz;
        rc = PMIx_Put(PMIX_GLOBAL, key, &value);
        /* the value only borrows the payload buffer; PMIx_Put has copied
         * what it needs by now, and destructing it would free ours */
        value.data.bo.bytes = NULL;
        value.data.bo.size = 0;
        PMIX_VALUE_DESTRUCT(&value);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "fence_timing: PMIx_Put failed: %s\n", PMIx_Error_string(rc));
            goto done;
        }
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "fence_timing: PMIx_Commit failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    /* Re-align, then time the first collecting fence. */
    if (PMIX_SUCCESS != (rc = fence_all(false, NULL))) {
        fprintf(stderr, "fence_timing: sync fence failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    if (PMIX_SUCCESS != (rc = fence_all(true, &modex_us))) {
        fprintf(stderr, "fence_timing: collecting fence failed: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }

    /* Confirm we actually moved data before believing the number: a
     * fence that quietly carried nothing is fast for the wrong reason. */
    if (1 < nprocs) {
        pmix_proc_t peer;
        PMIX_PROC_CONSTRUCT(&peer);
        PMIX_LOAD_PROCID(&peer, myproc.nspace,
                         (myproc.rank + 1) % nprocs);
        snprintf(key, sizeof(key), "fence-timing-%u-0", (unsigned) peer.rank);
        val = NULL;
        rc = PMIx_Get(&peer, key, NULL, 0, &val);
        if (PMIX_SUCCESS != rc || NULL == val) {
            fprintf(stderr, "fence_timing: rank %u could not read %s from rank %u: %s\n",
                    (unsigned) myproc.rank, key, (unsigned) peer.rank,
                    PMIx_Error_string(rc));
            if (NULL != val) {
                PMIX_VALUE_RELEASE(val);
            }
            goto done;
        }
        PMIX_VALUE_RELEASE(val);
    }

    /* Only rank 0 reports, so the lines are not interleaved. */
    if (0 == myproc.rank) {
        fprintf(stdout,
                "fence_timing: nprocs=%u keys=%d valsz=%d payload=%s "
                "barrier=%.1f us modex=%.1f us premium=%.1f us\n",
                nprocs, nkeys, valsz, compressible ? "compressible" : "random",
                barrier_us, modex_us, modex_us - barrier_us);
        fflush(stdout);
    }

done:
    if (NULL != payload) {
        free(payload);
    }
    PMIx_Finalize(NULL, 0);
    return 0;
}
