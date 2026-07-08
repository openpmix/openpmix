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
 * Exercise the invite/join CONSTRUCT_ABORT path.
 *
 * The leader (rank 0) invites the whole job to a group WITHOUT
 * PMIX_GROUP_OPTIONAL, making the construct all-or-nothing. Every invitee
 * accepts EXCEPT the last rank, which explicitly DECLINES via PMIx_Group_join
 * with PMIX_GROUP_DECLINE. Because participation was not optional, that single
 * non-accepter must abort the entire construct: no group forms. The library
 * must:
 *
 *   - notify every invited participant (including the members that accepted,
 *     so they stop waiting for a completion that will never come) with
 *     PMIX_GROUP_CONSTRUCT_ABORT; and
 *   - return PMIX_GROUP_CONSTRUCT_ABORT from the leader's PMIx_Group_invite.
 *
 * Nobody may receive PMIX_GROUP_CONSTRUCT_COMPLETE. The leader asserts its
 * invite returned the abort status; every rank asserts it received the abort
 * event and did NOT receive a completion. All ranks rejoin the job-level
 * barrier so the run tears down cleanly. See src/client/pmix_client_group.c
 * and #3936.
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

#define GROUP_ID "idabortgroup"

static pmix_proc_t myproc;
static uint32_t decliner = 0;
static volatile bool complete_seen = false;
static volatile bool abort_seen = false;

/* PMIX_GROUP_INVITED handler: accept, unless we are the designated decliner
 * (which explicitly declines) or the leader (which ignores its own
 * invitation). */
static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;
    pmix_group_opt_t opt;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, results, nresults);

    if (PMIX_CHECK_PROCID(source, &myproc)) {
        /* our own invitation - ignore */
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            grp = info[n].value.data.string;
            break;
        }
    }

    if (myproc.rank == decliner) {
        opt = PMIX_GROUP_DECLINE;
        fprintf(stderr, "%s:%d INVITED - declining\n", myproc.nspace, myproc.rank);
    } else {
        opt = PMIX_GROUP_ACCEPT;
        fprintf(stderr, "%s:%d INVITED - accepting\n", myproc.nspace, myproc.rank);
    }
    rc = PMIx_Group_join_nb(grp, source, opt, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s:%d ERROR in PMIx_Group_join_nb: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_CONSTRUCT_COMPLETE handler - must NOT fire in this exerciser */
static void complete_handler(size_t evhdlr_registration_id, pmix_status_t status,
                             const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                             pmix_info_t results[], size_t nresults,
                             pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                info, ninfo, results, nresults);

    fprintf(stderr, "%s:%d ERROR - unexpected PMIX_GROUP_CONSTRUCT_COMPLETE\n",
            myproc.nspace, myproc.rank);
    complete_seen = true;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_CONSTRUCT_ABORT handler - the construct was aborted */
static void abort_handler(size_t evhdlr_registration_id, pmix_status_t status,
                          const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                          pmix_info_t results[], size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                info, ninfo, results, nresults);

    fprintf(stderr, "%s:%d NOTIFIED that group construct was ABORTED\n",
            myproc.nspace, myproc.rank);
    abort_seen = true;
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

static int register_handler(pmix_status_t code, pmix_notification_fn_t fn)
{
    mylock_t lock;
    int rc;

    DEBUG_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, fn, errhandler_reg_callbk, (void *) &lock);
    DEBUG_WAIT_THREAD(&lock);
    rc = lock.status;
    DEBUG_DESTRUCT_LOCK(&lock);
    return rc;
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n;
    pmix_info_t *results;
    size_t nresults;
    int waited;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 3) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 3 processes\n");
        }
        exit(1);
    }
    /* the last rank is the one that will decline, aborting the construct */
    decliner = nprocs - 1;
    fprintf(stderr, "Client %s:%d job size %d (decliner is rank %d)\n",
            myproc.nspace, myproc.rank, nprocs, decliner);

    if (PMIX_SUCCESS != (rc = register_handler(PMIX_GROUP_INVITED, invite_handler))) {
        fprintf(stderr, "%d: failed to register invited handler: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        goto done;
    }
    if (PMIX_SUCCESS != (rc = register_handler(PMIX_GROUP_CONSTRUCT_COMPLETE, complete_handler))) {
        fprintf(stderr, "%d: failed to register complete handler: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        goto done;
    }
    if (PMIX_SUCCESS != (rc = register_handler(PMIX_GROUP_CONSTRUCT_ABORT, abort_handler))) {
        fprintf(stderr, "%d: failed to register abort handler: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        goto done;
    }

    /* sync so every rank has its handlers in place before the leader invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        /* invite the whole job with NO PMIX_GROUP_OPTIONAL - the construct is
         * all-or-nothing, so the lone decliner must abort it */
        fprintf(stderr, "%d executing Group_invite (all-or-nothing) for the whole job\n",
                myproc.rank);
        PMIX_PROC_CREATE(procs, nprocs);
        for (n = 0; n < nprocs; n++) {
            PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
        }
        results = NULL;
        nresults = 0;
        rc = PMIx_Group_invite(GROUP_ID, procs, nprocs, NULL, 0, &results, &nresults);
        PMIX_PROC_FREE(procs, nprocs);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        fprintf(stderr, "%d Group invite returned: %s\n", myproc.rank, PMIx_Error_string(rc));
        if (PMIX_GROUP_CONSTRUCT_ABORT != rc) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - invite returned %s, expected "
                    "PMIX_GROUP_CONSTRUCT_ABORT\n", myproc.nspace, myproc.rank,
                    PMIx_Error_string(rc));
            rc = PMIX_ERROR;
            goto done;
        }
        fprintf(stderr, "%d PMIx_Group_invite returned CONSTRUCT_ABORT: PASS\n", myproc.rank);
    }

    /* every invited participant must receive the abort event and none may be
     * told the (nonexistent) group completed */
    for (waited = 0; !abort_seen && waited < 100; waited++) {
        usleep(100000); /* 0.1s; up to 10s total */
    }
    if (!abort_seen) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - never received "
                "PMIX_GROUP_CONSTRUCT_ABORT\n", myproc.nspace, myproc.rank);
        rc = PMIX_ERR_TIMEOUT;
        goto done;
    }
    if (complete_seen) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - received CONSTRUCT_COMPLETE for an "
                "aborted construct\n", myproc.nspace, myproc.rank);
        rc = PMIX_ERROR;
        goto done;
    }
    fprintf(stderr, "%d PMIX_GROUP_CONSTRUCT_ABORT received: PASS\n", myproc.rank);
    rc = PMIX_SUCCESS;

    /* final job-level sync so every rank tears down together */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: final PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

done:
    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "%s:%d COMPLETE (rc %s)\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    fflush(stderr);
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
