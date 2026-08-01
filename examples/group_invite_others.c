/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Invite/join where the leader is NOT one of the invitees.
 *
 * group_invite.c has rank 0 invite the whole job, itself included. That is
 * the common shape, but it is not the only legal one: a coordinator may
 * form a group among *other* processes without joining it. The API says
 * nothing that requires the inviter to appear in the procs array, and the
 * two cases run different code inside the library.
 *
 * They differ because of how the leader's own answer is counted. The
 * invitation resolves when every invitee has answered, and the leader
 * counts as having answered by virtue of issuing the invitation - but only
 * when it is actually one of the invited members. Crediting that answer
 * unconditionally makes the count start one ahead of the membership here,
 * so the invitation resolves one answer early: the last invitee's accept
 * arrives after the decision, it is recorded as a non-responder, and
 * because this construct is all-or-nothing (no PMIX_GROUP_OPTIONAL) the
 * whole thing aborts. Every invitee then reports having received
 * PMIX_GROUP_CONSTRUCT_ABORT instead of PMIX_GROUP_CONSTRUCT_COMPLETE, and
 * the leader's PMIx_Group_invite returns that status. That is exactly what
 * this program checks for.
 *
 * Layout: rank 0 is the leader and invites ranks 1..N-1. Those ranks accept
 * from their PMIX_GROUP_INVITED handlers (the non-blocking join is
 * mandatory - the handler runs on the progress thread). The group is the
 * invitees only, so they - not the leader - receive
 * PMIX_GROUP_CONSTRUCT_COMPLETE, fence across the group to prove it is
 * usable, and destruct it. The leader takes no part beyond inviting.
 *
 * Requires at least 3 ranks: with 2 there is a single invitee and the
 * off-by-one is invisible.
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

#define GROUP_ID "invothers"

static pmix_proc_t myproc;
static volatile bool complete_seen = false;
static volatile bool abort_seen = false;

/* PMIX_GROUP_INVITED handler: an invitee accepts. The leader never sees
 * this event here - it did not invite itself. */
static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, results, nresults);

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

/* An abort is the symptom of the invitation having resolved before every
 * invitee answered, so report it explicitly rather than letting the run
 * fail as a timeout. */
static void abort_handler(size_t evhdlr_registration_id, pmix_status_t status,
                          const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                          pmix_info_t results[], size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                info, ninfo, results, nresults);

    fprintf(stderr, "%s:%d ERROR! received PMIX_GROUP_CONSTRUCT_ABORT - the "
            "invitation resolved before every invitee answered\n",
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
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);

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

    /* sync so every rank has its handlers in place before anyone invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        /* the leader invites everyone EXCEPT itself */
        fprintf(stderr, "%d executing Group_invite for ranks 1..%d (not itself)\n",
                myproc.rank, nprocs - 1);
        PMIX_PROC_CREATE(procs, nprocs - 1);
        for (n = 1; n < nprocs; n++) {
            PMIX_PROC_LOAD(&procs[n - 1], myproc.nspace, n);
        }
        results = NULL;
        nresults = 0;
        rc = PMIx_Group_invite(GROUP_ID, procs, nprocs - 1, NULL, 0, &results, &nresults);
        PMIX_PROC_FREE(procs, nprocs - 1);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: ERROR! PMIx_Group_invite FAILED: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        fprintf(stderr, "%d Group invite complete with status PMIX_SUCCESS\n", myproc.rank);
        /* the leader is not a member, so it has nothing further to do with
         * the group - just wait at the closing job fence below */
        goto lastsync;
    }

    fprintf(stderr, "%s:%d waiting to be invited and to join the group\n",
            myproc.nspace, myproc.rank);

    /* every invitee must be told the group formed */
    for (waited = 0; !complete_seen && !abort_seen && waited < 100; waited++) {
        usleep(100000); /* 0.1s; up to 10s total */
    }
    if (abort_seen) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - construct aborted\n",
                myproc.nspace, myproc.rank);
        rc = PMIX_GROUP_CONSTRUCT_ABORT;
        goto done;
    }
    if (!complete_seen) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - never received "
                "PMIX_GROUP_CONSTRUCT_COMPLETE\n", myproc.nspace, myproc.rank);
        rc = PMIX_ERR_TIMEOUT;
        goto done;
    }
    fprintf(stderr, "%d PMIX_GROUP_CONSTRUCT_COMPLETE received: PASS\n", myproc.rank);

    /* prove the group is usable: fence across it by its group id. Only the
     * invitees are members, so only they participate. */
    PMIX_LOAD_PROCID(&proc, GROUP_ID, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: ERROR! PMIx_Fence across group FAILED: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    fprintf(stderr, "%d group fence complete\n", myproc.rank);

    fprintf(stderr, "%d executing Group_destruct\n", myproc.rank);
    rc = PMIx_Group_destruct(GROUP_ID, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: ERROR! PMIx_Group_destruct FAILED: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

lastsync:
    /* final sync across the whole job, leader included */
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
