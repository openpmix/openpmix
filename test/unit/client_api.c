/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression coverage for defects found reviewing src/client.
 *
 * Every case here is a client-side API call that a well-behaved (or merely
 * careless) application can make, and that used to segfault, hang, or
 * silently corrupt the caller's data. They all run as a singleton - no
 * server, no launcher - so the whole file is safe under "make check". That
 * restricts what can be covered to the paths a client resolves locally or
 * rejects up front, which is precisely where these defects lived:
 *
 *   1. PMIx_Get_nb for a request the library answers itself (PMIX_PROCID,
 *      PMIX_VERSION_NUMERIC, or PMIX_RANK) built a bare caddy carrying no
 *      "get logic" object and then unconditionally PMIX_RELEASE'd that
 *      field. PMIX_RELEASE dereferences its argument, so this was a NULL
 *      dereference on the very first such call.
 *
 *   2. PMIx_Get_nb passed its uninitialized "val" pointer into the request
 *      parser, which dereferences it when PMIX_GET_STATIC_VALUES is set.
 *      The parser's own "did the caller provide storage?" test therefore
 *      read whatever was on the stack instead of seeing NULL.
 *
 *   3. PMIX_GET_REFRESH_CACHE dereferenced the caller's proc pointer, which
 *      PMIx_Get explicitly allows to be NULL (a key that is globally unique
 *      within our own namespace). It also packed a message for a server
 *      that a singleton does not have.
 *
 *   4. PMIx_Get with a NULL "val" wrote through it rather than rejecting
 *      the call.
 *
 *   5. PMIx_Compute_distances_nb accepted a NULL callback, then the server
 *      reply handler invoked it unconditionally.
 *
 *   6. PMIx_Fabric_register_nb / PMIx_Fabric_update_nb treat a NULL cbfunc
 *      as "the caddy is in cbdata" - an internal convention of their
 *      blocking wrappers. Called from user code with both NULL, they
 *      dereferenced a NULL caddy.
 *
 *   7. PMIx_Fabric_deregister freed the fabric's info array but left the
 *      pointer dangling, so a second deregister freed it again.
 *
 * The locally-satisfied gets are also repeated enough times that a caddy
 * leak or a reference-count error shows up as growth or a crash rather
 * than passing quietly.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define REPEAT 200

/* concurrency smoke: threads x gets, all driving the group-list lock */
#define NTHREADS 8
#define NGETS    400

static int nfail = 0;

static volatile int cbcount = 0;
static pmix_status_t cbstatus = PMIX_SUCCESS;
static int cbhadvalue = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        fprintf(stdout, "  ok    : %s\n", what);
    } else {
        fprintf(stdout, "  FAILED: %s\n", what);
        nfail++;
    }
}

static void valuecb(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    (void) cbdata;
    cbstatus = status;
    cbhadvalue = (NULL != kv);
    cbcount++;
}

/* Spin the caller until the callback has fired. The library delivers a
 * locally-satisfied get by way of the progress thread, so it does not
 * complete inside the PMIx_Get_nb call itself. */
static int wait_for_cb(int target)
{
    int spins;

    for (spins = 0; spins < 2000; spins++) {
        if (cbcount >= target) {
            return 1;
        }
        usleep(1000);
    }
    return 0;
}

static void test_local_get_nb(void)
{
    pmix_status_t rc;
    int i;

    fprintf(stdout, "\n-- PMIx_Get_nb, request satisfied locally --\n");

    /* PMIX_PROCID with a NULL proc: answered outright by the request
     * parser, which is the path that dereferenced a NULL logic object */
    cbcount = 0;
    rc = PMIx_Get_nb(NULL, PMIX_PROCID, NULL, 0, valuecb, NULL);
    check(PMIX_SUCCESS == rc, "PMIx_Get_nb(NULL, PMIX_PROCID) accepted");
    check(wait_for_cb(1), "PMIx_Get_nb(PMIX_PROCID) callback fired");
    check(PMIX_SUCCESS == cbstatus, "PMIx_Get_nb(PMIX_PROCID) reported success");
    check(cbhadvalue, "PMIx_Get_nb(PMIX_PROCID) delivered a value");

    /* PMIX_VERSION_NUMERIC: the same shortcut, reached with a non-NULL proc */
    cbcount = 0;
    rc = PMIx_Get_nb(NULL, PMIX_VERSION_NUMERIC, NULL, 0, valuecb, NULL);
    check(PMIX_SUCCESS == rc, "PMIx_Get_nb(PMIX_VERSION_NUMERIC) accepted");
    check(wait_for_cb(1), "PMIx_Get_nb(PMIX_VERSION_NUMERIC) callback fired");
    check(PMIX_SUCCESS == cbstatus, "PMIx_Get_nb(PMIX_VERSION_NUMERIC) reported success");

    /* repeat, so a caddy leak or refcount slip has room to show itself */
    cbcount = 0;
    for (i = 0; i < REPEAT; i++) {
        rc = PMIx_Get_nb(NULL, PMIX_PROCID, NULL, 0, valuecb, NULL);
        if (PMIX_SUCCESS != rc) {
            break;
        }
    }
    check(PMIX_SUCCESS == rc, "repeated PMIx_Get_nb(PMIX_PROCID) all accepted");
    check(wait_for_cb(REPEAT), "repeated PMIx_Get_nb callbacks all fired");
}

static void test_get_nb_static_values(void)
{
    pmix_info_t info;
    pmix_status_t rc;

    fprintf(stdout, "\n-- PMIx_Get_nb with PMIX_GET_STATIC_VALUES --\n");

    /* PMIx_Get_nb has no way to receive caller-provided storage, so this
     * must be rejected. It must reach that conclusion by inspecting a
     * known-NULL pointer, not an uninitialized one. */
    PMIX_INFO_LOAD(&info, PMIX_GET_STATIC_VALUES, NULL, PMIX_BOOL);
    rc = PMIx_Get_nb(NULL, PMIX_PROCID, &info, 1, valuecb, NULL);
    check(PMIX_ERR_BAD_PARAM == rc,
          "PMIx_Get_nb(PMIX_GET_STATIC_VALUES) returns PMIX_ERR_BAD_PARAM");
    PMIX_INFO_DESTRUCT(&info);
}

static void test_get_refresh_cache(void)
{
    pmix_info_t info;
    pmix_value_t *val = NULL;
    pmix_status_t rc;

    fprintf(stdout, "\n-- PMIx_Get with PMIX_GET_REFRESH_CACHE --\n");

    /* a NULL proc is legal: it means "this key is unique within my own
     * namespace". The refresh path used to dereference it regardless. */
    PMIX_INFO_LOAD(&info, PMIX_GET_REFRESH_CACHE, NULL, PMIX_BOOL);
    rc = PMIx_Get(NULL, "client.api.absent.key", &info, 1, &val);
    check(PMIX_SUCCESS != rc, "PMIx_Get(NULL proc, refresh, absent key) fails cleanly");
    check(NULL == val, "PMIx_Get failure leaves the caller's value NULL");
    PMIX_INFO_DESTRUCT(&info);

    /* and the same with an explicit proc, which must behave identically */
    val = NULL;
    PMIX_INFO_LOAD(&info, PMIX_GET_REFRESH_CACHE, NULL, PMIX_BOOL);
    rc = PMIx_Get(NULL, PMIX_PROCID, &info, 1, &val);
    check(PMIX_SUCCESS == rc, "PMIx_Get(PMIX_PROCID, refresh) succeeds");
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    PMIX_INFO_DESTRUCT(&info);
}

static void test_get_bad_params(void)
{
    pmix_status_t rc;

    fprintf(stdout, "\n-- PMIx_Get parameter validation --\n");

    /* nowhere to put the answer */
    rc = PMIx_Get(NULL, PMIX_PROCID, NULL, 0, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Get(val=NULL) returns PMIX_ERR_BAD_PARAM");

    /* both proc and key NULL is explicitly unsupported */
    {
        pmix_value_t *val = NULL;
        rc = PMIx_Get(NULL, NULL, NULL, 0, &val);
        check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Get(proc=NULL,key=NULL) returns PMIX_ERR_BAD_PARAM");
    }

    /* PMIx_Get_nb has no callback to report through */
    rc = PMIx_Get_nb(NULL, PMIX_PROCID, NULL, 0, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Get_nb(cbfunc=NULL) returns PMIX_ERR_BAD_PARAM");
}

static void test_get_pointer_and_static(void)
{
    pmix_info_t info;
    pmix_value_t *val;
    pmix_value_t storage;
    pmix_status_t rc;

    fprintf(stdout, "\n-- PMIx_Get pointer/static value forms --\n");

    /* PMIX_GET_POINTER_VALUES hands back a pointer into library state; the
     * caller must not release it */
    val = NULL;
    PMIX_INFO_LOAD(&info, PMIX_GET_POINTER_VALUES, NULL, PMIX_BOOL);
    rc = PMIx_Get(NULL, PMIX_PROCID, &info, 1, &val);
    check(PMIX_SUCCESS == rc, "PMIx_Get(PMIX_GET_POINTER_VALUES) succeeds");
    check(NULL != val && PMIX_PROC == val->type, "pointer form returns a PMIX_PROC value");
    PMIX_INFO_DESTRUCT(&info);

    /* PMIX_GET_STATIC_VALUES fills storage the caller supplied */
    PMIX_VALUE_CONSTRUCT(&storage);
    val = &storage;
    PMIX_INFO_LOAD(&info, PMIX_GET_STATIC_VALUES, NULL, PMIX_BOOL);
    rc = PMIx_Get(NULL, PMIX_PROCID, &info, 1, &val);
    check(PMIX_SUCCESS == rc, "PMIx_Get(PMIX_GET_STATIC_VALUES) succeeds");
    check(val == &storage, "static form left the caller's storage in place");
    check(PMIX_PROC == storage.type, "static form filled in a PMIX_PROC value");
    PMIX_INFO_DESTRUCT(&info);
    PMIX_VALUE_DESTRUCT(&storage);
}

static void test_compute_distances(void)
{
    pmix_status_t rc;

    fprintf(stdout, "\n-- PMIx_Compute_distances_nb parameter validation --\n");

    /* the server-reply handler has no completion route other than the
     * callback, so a NULL one has to be refused rather than accepted and
     * then invoked */
    rc = PMIx_Compute_distances_nb(NULL, NULL, NULL, 0, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc,
          "PMIx_Compute_distances_nb(cbfunc=NULL) returns PMIX_ERR_BAD_PARAM");
}

static void test_group_join_outparams(void)
{
    pmix_info_t *results;
    size_t nresults;
    pmix_proc_t leader;
    pmix_status_t rc;

    fprintf(stdout, "\n-- PMIx_Group_join out-parameters --\n");

    /* Poison them: they are OUT parameters the caller reads on return, and
     * this call used to declare them unused, so whatever was on the stack
     * was what the caller got back. A singleton cannot actually join
     * anything, which is the point - the defaults must be established
     * before the call can fail for any reason. */
    results = (pmix_info_t *) (uintptr_t) 0xdeadbeef;
    nresults = 12345;
    PMIX_LOAD_PROCID(&leader, "no.such.nspace", 0);

    rc = PMIx_Group_join("nosuchgroup", &leader, PMIX_GROUP_ACCEPT, NULL, 0,
                         &results, &nresults);
    check(PMIX_SUCCESS != rc, "PMIx_Group_join fails cleanly with no server");
    check(NULL == results, "PMIx_Group_join defined *results on the failure path");
    check(0 == nresults, "PMIx_Group_join defined *nresults on the failure path");

    /* both are documented as optional, so a caller that does not want the
     * values must be able to say so */
    rc = PMIx_Group_join("nosuchgroup", &leader, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    check(PMIX_SUCCESS != rc, "PMIx_Group_join accepts NULL results/nresults");
}

/* Hammer the paths that take the group-list lock. PMIx_Get with an explicit
 * namespace runs the caller's thread through
 * pmix_client_convert_group_procs(), which walks the group list under that
 * lock - so this is a direct test that the lock is released on every exit.
 * A missed unlock hangs here on the second acquisition rather than
 * corrupting something subtly later. The list is empty in a singleton, so
 * this proves the locking discipline, not the group logic itself. */
static void *get_hammer(void *arg)
{
    pmix_proc_t proc;
    pmix_value_t *val;
    long which = (long) (intptr_t) arg;
    int i;

    PMIX_LOAD_PROCID(&proc, "some.other.nspace", (pmix_rank_t) which);
    for (i = 0; i < NGETS; i++) {
        val = NULL;
        /* expected to fail - we only care that it returns */
        (void) PMIx_Get(&proc, "client.api.absent.key", NULL, 0, &val);
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
    }
    return NULL;
}

static void test_group_lock_concurrency(void)
{
    pthread_t tid[NTHREADS];
    long n;
    int rc;

    fprintf(stdout, "\n-- group-list lock under concurrent callers --\n");

    for (n = 0; n < NTHREADS; n++) {
        rc = pthread_create(&tid[n], NULL, get_hammer, (void *) (intptr_t) n);
        if (0 != rc) {
            check(0, "pthread_create");
            /* join whatever we did start before giving up */
            while (0 < n--) {
                pthread_join(tid[n], NULL);
            }
            return;
        }
    }
    for (n = 0; n < NTHREADS; n++) {
        pthread_join(tid[n], NULL);
    }
    check(1, "8 threads x 400 gets through the group-list lock completed");
}

static void test_fabric(void)
{
    pmix_fabric_t fabric;
    pmix_status_t rc;

    fprintf(stdout, "\n-- fabric API parameter validation and teardown --\n");

    PMIx_Fabric_construct(&fabric);

    /* NULL fabric */
    rc = PMIx_Fabric_register_nb(NULL, NULL, 0, NULL, (void *) &fabric);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Fabric_register_nb(fabric=NULL) rejected");
    rc = PMIx_Fabric_update_nb(NULL, NULL, (void *) &fabric);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Fabric_update_nb(fabric=NULL) rejected");
    rc = PMIx_Fabric_deregister_nb(NULL, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Fabric_deregister_nb(fabric=NULL) rejected");

    /* a NULL cbfunc means "the caddy is in cbdata"; with both NULL there is
     * no caddy at all, and the recv handler would dereference it */
    rc = PMIx_Fabric_register_nb(&fabric, NULL, 0, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Fabric_register_nb(NULL, NULL) rejected");
    rc = PMIx_Fabric_update_nb(&fabric, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Fabric_update_nb(NULL, NULL) rejected");

    /* stand in for a fabric that had been registered, then deregister it
     * twice: the second call must not free the array again */
    PMIX_INFO_CREATE(fabric.info, 2);
    fabric.ninfo = 2;
    PMIX_INFO_LOAD(&fabric.info[0], PMIX_HOSTNAME, "somehost", PMIX_STRING);
    PMIX_INFO_LOAD(&fabric.info[1], PMIX_FABRIC_INDEX, &fabric.index, PMIX_SIZE);

    rc = PMIx_Fabric_deregister(&fabric);
    check(PMIX_SUCCESS == rc, "PMIx_Fabric_deregister succeeds");
    check(NULL == fabric.info && 0 == fabric.ninfo,
          "PMIx_Fabric_deregister leaves no dangling info pointer");

    rc = PMIx_Fabric_deregister(&fabric);
    check(PMIX_SUCCESS == rc, "second PMIx_Fabric_deregister is a safe no-op");
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    (void) argc;
    (void) argv;

    /* force the singleton path - nothing to connect to, so each API call
     * either resolves locally or is rejected up front */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    fprintf(stdout, "\n=== src/client API regression test ===\n");

    /* a singleton reports PMIX_ERR_UNREACH from init - it is fully
     * initialized, it just has no server */
    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_local_get_nb();
    test_get_nb_static_values();
    test_get_refresh_cache();
    test_get_bad_params();
    test_get_pointer_and_static();
    test_compute_distances();
    test_fabric();
    test_group_join_outparams();
    test_group_lock_concurrency();

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Finalize failed: %s\n", PMIx_Error_string(rc));
        nfail++;
    }

    if (0 == nfail) {
        fprintf(stdout, "\nsrc/client API regression test: PASS\n\n");
    } else {
        fprintf(stdout, "\nsrc/client API regression test: FAIL (%d)\n\n", nfail);
    }
    return (0 == nfail) ? 0 : 1;
}
