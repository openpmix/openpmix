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
 * The invite/join happy path, run by an application that ends the event
 * chain -- the regression case for openpmix#4059.
 *
 * This is group_invite.c with one addition, and that addition is the whole
 * point: the leader registers an ordinary single-code handler for
 * PMIX_GROUP_INVITE_ACCEPTED, logs the acceptance, and returns
 * PMIX_EVENT_ACTION_COMPLETE -- the documented way for an application to say
 * "I handled this", and what every other handler in these examples already
 * does.  Logging who joined is about as ordinary as an invite application
 * gets.
 *
 * That used to hang the leader forever.  The library counted invitation
 * answers in an ordinary multi-code event handler, and the chain runs
 * single-code handlers first, so the application's handler ended the chain
 * before the library's was reached.  Nothing then counted the answers, the
 * invitation never resolved, and PMIx_Group_invite blocked with no timeout
 * to bound it.  The acceptances all arrived and the application saw every
 * one of them; only the library was blind.
 *
 * The library now counts answers in an internal *observer*, which runs ahead
 * of the chain and cannot be pre-empted, so the group forms exactly as it
 * does without the extra handler.  See openpmix#4059 and the "Internal
 * observers" section of src/event/AGENTS.md.
 *
 * Note the second thing this asserts, which is the same defect pointed the
 * other way: the application's own acceptance handler must fire.  The
 * library's invite handler used to complete the chain itself, so whenever it
 * ran first it swallowed the application's handler for these codes.  The
 * driver checks that the leader logged all N-1 acceptances.
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

#define GROUP_ID "supgroup"

static pmix_proc_t myproc;
static volatile bool complete_seen = false;

/* PMIX_GROUP_INVITED handler: an invitee accepts the invitation.  The leader
 * sees its own invitation reflected back (source == self) and ignores it. */
static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, results, nresults);

    /* if I am the leader, ignore my own invitation and let the chain proceed */
    if (PMIX_CHECK_PROCID(source, &myproc)) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    /* find the group id carried on the invitation and accept it */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            grp = info[n].value.data.string;
            break;
        }
    }
    fprintf(stderr, "%s:%d INVITED to group %s by %s:%d - accepting\n",
            myproc.nspace, myproc.rank, (NULL == grp) ? "(unknown)" : grp,
            source->nspace, source->rank);
    rc = PMIx_Group_join_nb(grp, source, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s:%d ERROR in PMIx_Group_join_nb: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* THE handler this test exists for.  The leader registers it for
 * PMIX_GROUP_INVITE_ACCEPTED to log who joined -- and, like every other
 * handler here, reports that it handled the event by ending the chain. */
static void accepted_handler(size_t evhdlr_registration_id, pmix_status_t status,
                             const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                             pmix_info_t results[], size_t nresults,
                             pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, info, ninfo,
                                results, nresults);

    fprintf(stderr, "%s:%d APP saw an acceptance from %s:%d\n", myproc.nspace,
            myproc.rank, source->nspace, source->rank);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_CONSTRUCT_COMPLETE handler: the group finished forming.  This one
 * also ends the chain, which is what used to blind the invitee-side
 * leader-failure watch. */
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

static void errhandler_reg_callbk(pmix_status_t status, size_t errhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;
    EXAMPLES_HIDE_UNUSED_PARAMS(errhandler_ref);

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

/* register one event handler for a single code, blocking until registered */
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

    /* register handlers for the invitation and the completion events */
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

    /* the leader also watches the acceptances roll in - the registration this
     * whole test is about */
    if (0 == myproc.rank) {
        if (PMIX_SUCCESS != (rc = register_handler(PMIX_GROUP_INVITE_ACCEPTED,
                                                   accepted_handler))) {
            fprintf(stderr, "%d: failed to register accepted handler: %s\n", myproc.rank,
                    PMIx_Error_string(rc));
            goto done;
        }
    }

    /* sync so every rank has its handlers in place before anyone invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        /* the leader invites the whole job (itself included) to the group.
         * Deliberately no PMIX_TIMEOUT: bounding this call would convert the
         * regression from a hang into an abort, which is not the same test. */
        fprintf(stderr, "%d executing Group_invite for the whole job\n", myproc.rank);
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
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_invite FAILED: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        fprintf(stderr, "%d Group invite complete with status PMIX_SUCCESS\n", myproc.rank);
    } else {
        fprintf(stderr, "%s:%d waiting to be invited and to join the group\n",
                myproc.nspace, myproc.rank);
    }

    /* every member (leader and invitees) must receive CONSTRUCT_COMPLETE */
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

    /* prove the group is usable: fence across it by its group id */
    PMIX_LOAD_PROCID(&proc, GROUP_ID, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence across group FAILED: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    fprintf(stderr, "%d group fence complete\n", myproc.rank);

    /* tear the group back down.  Destruct is a collective over the group
     * membership, so every member (leader and invitees alike) must call it. */
    fprintf(stderr, "%d executing Group_destruct\n", myproc.rank);
    rc = PMIx_Group_destruct(GROUP_ID, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct FAILED: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* final sync across the job */
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
