/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for src/server/pmix_server_dmodex.c - the two data
 * calls the host makes down into us.
 *
 * The case that matters is the deferral. PMIx_server_dmodex_request asks
 * for the modex data of one of our local clients on behalf of a remote
 * server; if that client has not committed its data yet, the request is
 * parked on pmix_server_globals.remote_pnd and the host's response
 * function is not called. The *only* thing that ever took a request back
 * off that list was the target committing - so a target that departs
 * without committing (it aborted, it never called PMIx_Put, its namespace
 * was torn down) left the host holding a request it would never hear
 * about again, and the remote client blocked in PMIx_Get behind it waited
 * forever. The mirror image on the other deferral list, local_reqs, has
 * been handled by pmix_server_purge_events for some time; this is the
 * same rule applied to the list going the other way.
 *
 * Finalize is the second way to reach that state, and it looks correct
 * where the departure case looked wrong. A bare PMIX_LIST_DESTRUCT of
 * remote_pnd frees everything on it - the entry's destructor releases
 * the caddy - so it leaks nothing. What it drops is the response
 * function, which is the only thing that will ever tell the host what
 * became of its request, and the host process outlives our finalize. So
 * the last two cases park a request and then finalize, and the answer is
 * the only observable: the list itself is gone by the time they run.
 *
 * Ordering here is deterministic rather than timed. PMIx_server_dmodex_request
 * thread-shifts and returns, so the parking happens on the progress
 * thread; PMIx_Store_internal is a blocking call onto that same thread,
 * so when it returns, everything queued ahead of it - including our
 * deferred request - has already run. PMIx_server_deregister_nspace is
 * then driven in its blocking form, so purge_events has completed by the
 * time it returns and the response function has either fired or never
 * will. No sleeps, no polling.
 *
 * Test cases:
 *
 *   store internal, then read it back      -> the value survives the xfer
 *   store internal with a NULL value       -> PMIX_ERR_BAD_PARAM
 *   dmodex for an uncommitted local rank   -> parked, host not answered
 *   that rank's nspace is deregistered     -> host answered with an error
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DMUT_NSPACE "server-dmodex-ut"
#define DMUT_NPROCS 2

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* what the host's dmodex response function was told */
static bool dm_fired = false;
static pmix_status_t dm_status = PMIX_SUCCESS;

static void dmresponse(pmix_status_t status, char *data, size_t sz, void *cbdata)
{
    (void) data;
    (void) sz;
    (void) cbdata;

    dm_fired = true;
    dm_status = status;
}

static size_t nparked(void)
{
    return pmix_list_get_size(&pmix_server_globals.remote_pnd);
}

/* A blocking round trip through the progress thread. Anything queued onto
 * that thread before this call has been serviced by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-dmodex-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_status_t register_job(void)
{
    pmix_info_t info[2];
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_status_t rc;
    uint32_t nprocs = DMUT_NPROCS;
    pmix_rank_t r;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, DMUT_NSPACE);
    rc = PMIx_server_register_nspace(ns, DMUT_NPROCS, info, 2, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    for (r = 0; r < DMUT_NPROCS; r++) {
        PMIX_LOAD_PROCID(&proc, DMUT_NSPACE, r);
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_proc_t target;
    pmix_value_t val, *got = NULL;
    pmix_nspace_t ns;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_dmodex: host-facing data call unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- PMIx_Store_internal round trip ------------------------------ */
    PMIX_VALUE_LOAD(&val, "stored-value", PMIX_STRING);
    rc = PMIx_Store_internal(&pmix_globals.myid, "server-dmodex-ut.key", &val);
    report("store internal accepted", PMIX_SUCCESS == rc);
    PMIX_VALUE_DESTRUCT(&val);

    rc = PMIx_Get(&pmix_globals.myid, "server-dmodex-ut.key", NULL, 0, &got);
    report("stored value reads back", PMIX_SUCCESS == rc && NULL != got
                                          && PMIX_STRING == got->type
                                          && NULL != got->data.string
                                          && 0 == strcmp(got->data.string, "stored-value"));
    if (NULL != got) {
        PMIX_VALUE_RELEASE(got);
        got = NULL;
    }

    /* --- and it screens the value it is about to dereference --------- */
    rc = PMIx_Store_internal(&pmix_globals.myid, "server-dmodex-ut.bad", NULL);
    report("store internal rejects a NULL value", PMIX_ERR_BAD_PARAM == rc);

    /* --- a dmodex request for a rank that has not committed ---------- */
    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    dm_fired = false;
    PMIX_LOAD_PROCID(&target, DMUT_NSPACE, 0);
    rc = PMIx_server_dmodex_request(&target, dmresponse, NULL);
    report("dmodex request accepted", PMIX_SUCCESS == rc);
    progress_barrier();
    report("uncommitted rank defers the request", 1 == nparked());
    report("deferred request does not answer the host yet", !dm_fired);

    /* --- the target departs without ever committing ------------------ */
    PMIX_LOAD_NSPACE(ns, DMUT_NSPACE);
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    progress_barrier();
    report("departure drained the deferred request", 0 == nparked());
    report("departure answered the host", dm_fired);
    report("departure answered with a failure status",
           dm_fired && PMIX_SUCCESS != dm_status);

    /* --- and a request still parked when the server finalizes --------- */
    /* remote_pnd used to be torn down with a bare PMIX_LIST_DESTRUCT,
     * which frees the caddies without ever calling the response function
     * the host supplied - so the host was left holding a request it would
     * never hear about again, and the host process outlives our finalize.
     * The local_reqs list beside it had always been drained with a status
     * for exactly this reason. */
    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "re-register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    dm_fired = false;
    dm_status = PMIX_SUCCESS;
    PMIX_LOAD_PROCID(&target, DMUT_NSPACE, 1);
    rc = PMIx_server_dmodex_request(&target, dmresponse, NULL);
    report("second dmodex request accepted", PMIX_SUCCESS == rc);
    progress_barrier();
    report("second request is parked", 1 == nparked());

    PMIx_server_finalize();

    /* the list is gone by now, so the answer is the only observable */
    report("finalize answered the parked request", dm_fired);
    report("finalize answered with a failure status",
           dm_fired && PMIX_SUCCESS != dm_status);

    fprintf(stdout, "server_dmodex: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
