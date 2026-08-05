/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * End-to-end timings for PMIx_Get, through the real public API.
 *
 * Unlike test/unit/util/hash_perf.c - which drives the datastore
 * directly and so measures only the lookup - this runs the whole
 * client path, including the thread-shift onto the progress thread
 * and the condition-variable wake back. That fixed round trip is the
 * floor under every get that is not answered by process_request(),
 * and isolating it is the point of this program.
 *
 * The comparison that matters is:
 *
 *   get/shortcut   PMIX_PROCID, which process_request() answers
 *                  outright - no thread-shift, no datastore
 *   get/self       a key this process put - thread-shift, datastore
 *                  lookup, deep copy, wake
 *
 * The difference between those two is what a caller-thread fast path
 * for a local hit could recover. Everything else here is context for
 * it.
 *
 * This runs as a SINGLETON: there is no server, so a get that misses
 * locally is refused rather than sent, and PMIx_Commit short-circuits.
 * That is what makes the program safe in "make check", and it is also
 * why it cannot measure a server round trip - that half belongs to
 * contrib/dockerswarm.
 *
 * It asserts CORRECTNESS ONLY and never on elapsed time; see the
 * header comment in hash_perf.c for why. Timings are printed so a
 * regression shows in a diff of two "make check" logs.
 *
 * Tunable through the environment for a real measurement run:
 *   PMIX_PERF_KEYS    keys this process puts   (default 8)
 *   PMIX_PERF_ITERS   gets per measurement     (default 20000)
 *
 * Exit 0 if all correctness checks pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "pmix.h"

static int npass = 0;
static int nfail = 0;

static int nkeys = 8;
static int niters = 20000;

static pmix_proc_t myproc;

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

    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000000.0 + (double) tv.tv_usec;
}

static void makekey(char *buf, size_t sz, int k)
{
    snprintf(buf, sz, "unit.getperf.k%d", k);
}

/* One get, releasing whatever came back. */
static pmix_status_t one_get(const pmix_proc_t *proc, const char *key, const pmix_info_t *info,
                             size_t ninfo)
{
    pmix_value_t *val = NULL;
    pmix_status_t rc;

    rc = PMIx_Get(proc, key, info, ninfo, &val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return rc;
}

/* Time niters gets of one shape and print ns/op. "want" is the status
 * every iteration must return, so a shape that does not hold is
 * reported rather than silently mistimed. */
static void measure(const char *label, const pmix_proc_t *proc, const char *key,
                    const pmix_info_t *info, size_t ninfo, pmix_status_t want)
{
    pmix_status_t rc;
    double t0, t1;
    int i, bad = 0;

    for (i = 0; i < 100; i++) {
        (void) one_get(proc, key, info, ninfo);
    }

    t0 = now_usec();
    for (i = 0; i < niters; i++) {
        rc = one_get(proc, key, info, ninfo);
        if (want != rc) {
            bad++;
        }
    }
    t1 = now_usec();

    fprintf(stdout, "  %-14s %8.1f ns/op\n", label, (t1 - t0) * 1000.0 / (double) niters);
    report(label, 0 == bad);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t val;
    pmix_info_t optional;
    char key[PMIX_MAX_KEYLEN + 1];
    char absent[PMIX_MAX_KEYLEN + 1];
    int k;
    (void) argc;
    (void) argv;

    envint("PMIX_PERF_KEYS", &nkeys);
    envint("PMIX_PERF_ITERS", &niters);

    /* force the singleton path, exactly as client_api.c does */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    /* a singleton reports PMIX_ERR_UNREACH from init - it is fully
     * initialized, it just has no server */
    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== PMIx_Get end-to-end timings ===\n");
    fprintf(stdout, "  keys=%d iters=%d\n\n", nkeys, niters);

    for (k = 0; k < nkeys; k++) {
        makekey(key, sizeof(key), k);
        PMIX_VALUE_LOAD(&val, key, PMIX_STRING);
        rc = PMIx_Put(PMIX_GLOBAL, key, &val);
        PMIX_VALUE_DESTRUCT(&val);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIx_Put(%s) failed: %s\n", key, PMIx_Error_string(rc));
            PMIx_Finalize(NULL, 0);
            return 1;
        }
    }
    rc = PMIx_Commit();
    report("setup: put and committed", PMIX_SUCCESS == rc);

    makekey(key, sizeof(key), 0);
    snprintf(absent, sizeof(absent), "unit.getperf.never.stored");

    /* PMIX_OPTIONAL keeps the miss shapes out of the "ask the server"
     * branch. A singleton would refuse there anyway, but saying so
     * explicitly keeps this measuring the local lookup either way. */
    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);

    /* The floor: answered entirely by process_request(), so no
     * thread-shift and no datastore. Everything below pays that. */
    measure("get/shortcut", NULL, PMIX_PROCID, NULL, 0, PMIX_SUCCESS);

    /* The common get: full path for data already in this process. */
    measure("get/self", &myproc, key, NULL, 0, PMIX_SUCCESS);

    /* A local miss - full path, no copy. */
    measure("get/miss", &myproc, absent, &optional, 1, PMIX_ERR_NOT_FOUND);

    /* NULL proc means PMIX_RANK_UNDEF, which is the fan-out shape. */
    measure("get/undef", NULL, key, NULL, 0, PMIX_SUCCESS);
    measure("get/undef-miss", NULL, absent, &optional, 1, PMIX_ERR_NOT_FOUND);

    /* The value has to survive, or the timings were measuring a
     * lookup that does not return what a real get would. */
    {
        pmix_value_t *v = NULL;
        int ok;

        rc = PMIx_Get(&myproc, key, NULL, 0, &v);
        ok = (PMIX_SUCCESS == rc && NULL != v && PMIX_STRING == v->type
              && NULL != v->data.string && 0 == strcmp(v->data.string, key));
        report("get/self returns the value that was put", ok);
        if (NULL != v) {
            PMIX_VALUE_RELEASE(v);
        }
    }

    PMIX_INFO_DESTRUCT(&optional);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Finalize failed: %s\n", PMIx_Error_string(rc));
        nfail++;
    }

    return (nfail > 0) ? 1 : 0;
}
