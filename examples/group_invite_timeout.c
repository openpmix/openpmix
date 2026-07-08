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
 * Exercise the invite/join TIMEOUT path.
 *
 * The leader (rank 0) invites the whole job to a group, passing PMIX_TIMEOUT
 * and PMIX_GROUP_OPTIONAL (so a non-responder yields a reduced group rather than
 * aborting the construct).  Every invitee accepts EXCEPT the last rank, which
 * deliberately never responds to its PMIX_GROUP_INVITED event.  Without a
 * timeout the leader would wait forever for that missing acceptance; with one,
 * the library must:
 *
 *   - fire PMIX_GROUP_INVITE_FAILED at the leader naming the non-responder;
 *   - complete PMIx_Group_invite on the members that DID accept (reduced
 *     membership) rather than hang or error; and
 *   - deliver PMIX_GROUP_CONSTRUCT_COMPLETE only to those members (the
 *     non-responder is not part of the group).
 *
 * The leader asserts it saw the INVITE_FAILED for the right proc and that the
 * invite returned; the accepting members assert they were notified of
 * completion.  The non-responder simply declines to answer and then rejoins the
 * job-level barrier so the run tears down cleanly.  See
 * src/client/pmix_client_group.c and #3936.
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

#define GROUP_ID "itogroup"
#define INVITE_TIMEOUT 3

static pmix_proc_t myproc;
static uint32_t victim = 0;
static volatile bool complete_seen = false;
static volatile bool failed_seen = false;
static pmix_proc_t failed_proc;

/* PMIX_GROUP_INVITED handler: accept, unless we are the designated
 * non-responder (which stays silent to drive the timeout) or the leader
 * (which ignores its own invitation). */
static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, results, nresults);

    if (PMIX_CHECK_PROCID(source, &myproc)) {
        /* our own invitation - ignore */
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    if (myproc.rank == victim) {
        /* the non-responder: deliberately do NOT answer so the leader's timeout
         * fires */
        fprintf(stderr, "%s:%d INVITED but deliberately NOT responding (drives timeout)\n",
                myproc.nspace, myproc.rank);
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
    fprintf(stderr, "%s:%d INVITED - accepting\n", myproc.nspace, myproc.rank);
    rc = PMIx_Group_join_nb(grp, source, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s:%d ERROR in PMIx_Group_join_nb: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_CONSTRUCT_COMPLETE handler */
static void complete_handler(size_t evhdlr_registration_id, pmix_status_t status,
                             const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                             pmix_info_t results[], size_t nresults,
                             pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                info, ninfo, results, nresults);

    fprintf(stderr, "%s:%d NOTIFIED that group construct is complete\n",
            myproc.nspace, myproc.rank);
    complete_seen = true;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_INVITE_FAILED handler (leader) - records who failed to respond */
static void failed_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    PMIX_PROC_CONSTRUCT(&failed_proc);
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            memcpy(&failed_proc, info[n].value.data.proc, sizeof(pmix_proc_t));
        }
    }
    fprintf(stderr, "%s:%d NOTIFIED that %s:%d failed to respond to the invitation\n",
            myproc.nspace, myproc.rank, failed_proc.nspace, failed_proc.rank);
    failed_seen = true;
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
    pmix_info_t *results, tinfo[2];
    size_t nresults;
    int waited, timeout = INVITE_TIMEOUT;
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
    /* the last rank is the one that will not answer the invitation */
    victim = nprocs - 1;
    fprintf(stderr, "Client %s:%d job size %d (non-responder is rank %d)\n",
            myproc.nspace, myproc.rank, nprocs, victim);

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
    if (0 == myproc.rank) {
        if (PMIX_SUCCESS != (rc = register_handler(PMIX_GROUP_INVITE_FAILED, failed_handler))) {
            fprintf(stderr, "%d: failed to register invite-failed handler: %s\n", myproc.rank,
                    PMIx_Error_string(rc));
            goto done;
        }
    }

    /* sync so every rank has its handlers in place before the leader invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        fprintf(stderr, "%d executing Group_invite (timeout %d s) for the whole job\n",
                myproc.rank, timeout);
        PMIX_PROC_CREATE(procs, nprocs);
        for (n = 0; n < nprocs; n++) {
            PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
        }
        /* mark participation PMIX_GROUP_OPTIONAL so a non-responder yields a
         * reduced group rather than aborting the construct */
        PMIX_INFO_LOAD(&tinfo[0], PMIX_TIMEOUT, &timeout, PMIX_INT);
        PMIX_INFO_LOAD(&tinfo[1], PMIX_GROUP_OPTIONAL, NULL, PMIX_BOOL);
        results = NULL;
        nresults = 0;
        rc = PMIx_Group_invite(GROUP_ID, procs, nprocs, tinfo, 2, &results, &nresults);
        PMIX_PROC_FREE(procs, nprocs);
        PMIX_INFO_DESTRUCT(&tinfo[0]);
        PMIX_INFO_DESTRUCT(&tinfo[1]);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        fprintf(stderr, "%d Group invite returned: %s\n", myproc.rank, PMIx_Error_string(rc));
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_invite FAILED: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        /* we must have been told the non-responder failed to answer */
        if (!failed_seen) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - never received "
                    "PMIX_GROUP_INVITE_FAILED\n", myproc.nspace, myproc.rank);
            rc = PMIX_ERROR;
            goto done;
        }
        if (failed_proc.rank != victim) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - wrong failed rank %d (expected %d)\n",
                    myproc.nspace, myproc.rank, failed_proc.rank, victim);
            rc = PMIX_ERROR;
            goto done;
        }
        fprintf(stderr, "%d PMIX_GROUP_INVITE_FAILED for non-responder: PASS\n", myproc.rank);
    } else if (myproc.rank == victim) {
        /* the non-responder does not join; it only reports and waits at the
         * closing barrier */
        fprintf(stderr, "%d not joining the group (non-responder)\n", myproc.rank);
    }

    /* the leader and the accepting members must be notified of completion; the
     * non-responder must NOT be (it is not part of the group) */
    if (myproc.rank != victim) {
        for (waited = 0; !complete_seen && waited < 100; waited++) {
            usleep(100000); /* 0.1s; up to 10s total */
        }
        if (!complete_seen) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - never received "
                    "PMIX_GROUP_CONSTRUCT_COMPLETE\n", myproc.nspace, myproc.rank);
            rc = PMIX_ERR_TIMEOUT;
            goto done;
        }
        fprintf(stderr, "%d PMIX_GROUP_CONSTRUCT_COMPLETE received: PASS\n", myproc.rank);
    }

    /* final job-level sync so every rank (including the non-responder) tears
     * down together */
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
