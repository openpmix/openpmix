/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise a member voluntarily leaving a live group (PMIx_Group_leave).
 *
 * Every rank in the job is a group member.  All ranks construct the group,
 * then sync with a fence.  One rank (the last) then calls PMIx_Group_leave;
 * per the API contract that generates a PMIX_GROUP_LEFT event notifying the
 * remaining members of its departure, and returns success once the event has
 * been locally generated (not indicative of remote receipt).  Every surviving
 * member has registered a handler for PMIX_GROUP_LEFT and must receive that
 * notification - if it does not, this test fails (rather than hangs) via a
 * bounded wait.
 *
 * The whole job is the membership (rather than a subset) so that, when launched
 * across nodes, the departing rank's node always also hosts a surviving member,
 * exercising both the local and remote notification-delivery paths.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pmix.h>
#include "examples.h"

static pmix_proc_t myproc;
static volatile bool left_seen = false;
static pmix_proc_t departed;

/* handler for the PMIX_GROUP_LEFT event - records that we were notified and
 * who departed */
static void left_handler(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                results, nresults);

    PMIX_PROC_CONSTRUCT(&departed);
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            memcpy(&departed, info[n].value.data.proc, sizeof(pmix_proc_t));
        }
    }
    fprintf(stderr, "%s:%d NOTIFIED that %s:%d left the group\n",
            myproc.nspace, myproc.rank, departed.nspace, departed.rank);
    left_seen = true;

    /* we must always continue the event chain */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void errhandler_reg_callbk(pmix_status_t status, size_t errhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;
    EXAMPLES_HIDE_UNUSED_PARAMS(errhandler_ref);

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n, victim;
    pmix_info_t *results;
    size_t nresults;
    mylock_t lock;
    pmix_status_t code = PMIX_GROUP_LEFT;
    int waited;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }

    /* get our job size */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 2) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 2 processes\n");
        }
        exit(1);
    }
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);

    /* register our handler for the group-left event */
    DEBUG_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, left_handler, errhandler_reg_callbk,
                                (void *) &lock);
    DEBUG_WAIT_THREAD(&lock);
    rc = lock.status;
    DEBUG_DESTRUCT_LOCK(&lock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%d: failed to register handler: %s\n", myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* sync everyone up first */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* the entire job is the group membership; the last rank is the one that
     * will voluntarily leave */
    victim = nprocs - 1;

    fprintf(stderr, "%d executing Group_construct\n", myproc.rank);
    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }
    results = NULL;
    nresults = 0;
    rc = PMIx_Group_construct("lgroup", procs, nprocs, NULL, 0, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* sync so everyone has completed construction before anyone leaves */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: post-construct PMIx_Fence failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (myproc.rank == victim) {
        /* voluntarily depart the group - this must generate the PMIX_GROUP_LEFT
         * event to the remaining members and return success once locally
         * generated */
        fprintf(stderr, "%d executing Group_leave\n", myproc.rank);
        rc = PMIx_Group_leave("lgroup", NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_leave FAILED: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        fprintf(stderr, "%d Group_leave complete: SUCCESS\n", myproc.rank);
    } else {
        /* a surviving member - wait (bounded) to be notified of the departure */
        for (waited = 0; !left_seen && waited < 100; waited++) {
            usleep(100000); /* 0.1s; up to 10s total */
        }
        if (!left_seen) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - never received PMIX_GROUP_LEFT\n",
                    myproc.nspace, myproc.rank);
            rc = PMIX_ERR_TIMEOUT;
            goto done;
        }
        if (departed.rank != victim) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - wrong departed rank %d (expected %d)\n",
                    myproc.nspace, myproc.rank, departed.rank, victim);
            rc = PMIX_ERROR;
            goto done;
        }
        fprintf(stderr, "%d PMIX_GROUP_LEFT correctly received: PASS\n", myproc.rank);
    }

done:
    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "%s:%d COMPLETE (rc %s)\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    fflush(stderr);
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
