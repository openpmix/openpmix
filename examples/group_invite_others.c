/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A leader may not form a group it does not belong to.
 *
 * group_invite.c has rank 0 invite the whole job, itself included. This
 * program checks the case that is *not* legal: a process inviting others
 * to a group it is not a member of. PMIx_Group_invite refuses it with
 * PMIX_ERR_NOT_A_MEMBER - the same status PMIx_Group_construct gives for
 * the same mistake - and no invitation is issued.
 *
 * This test previously asserted the opposite - that the invitation ran and
 * the group formed on the invitees alone - on the understanding that "the
 * API says nothing that requires the inviter to appear in the procs
 * array". That was wrong. A group's leader is one of its members, and the
 * operation has no meaning otherwise: the leader would be waiting on a
 * completion event addressed to a group it is not in, which is precisely
 * the hang that made the old expectation look plausible. The check now
 * lives where the invitation is created, in pmix_server_group_invite().
 *
 * Layout: rank 0 attempts to invite ranks 1..N-1 without joining, and must
 * be refused. The other ranks have nothing to wait for - no invitation is
 * ever sent - so they proceed straight to the closing fence. They keep
 * handlers registered for PMIX_GROUP_INVITED, PMIX_GROUP_CONSTRUCT_COMPLETE
 * and PMIX_GROUP_CONSTRUCT_ABORT, and check after that fence that none of
 * the three fired: a refusal that still issued the invitation would show up
 * as an event on an invitee, not as a status at the leader.
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
static volatile bool invited_seen = false;
static volatile bool complete_seen = false;
static volatile bool abort_seen = false;

/* PMIX_GROUP_INVITED handler. No invitation may be issued for this group -
 * the leader's request is refused before one goes out - so an event here
 * is the failure this program is looking for. Record it rather than
 * joining, and let the check after the closing fence report it. */
static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, results, nresults);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            grp = info[n].value.data.string;
            break;
        }
    }
    fprintf(stderr, "%s:%d ERROR! INVITED to group %s by %s:%d - the invitation "
            "should have been refused\n",
            myproc.nspace, myproc.rank, (NULL == grp) ? "(unknown)" : grp,
            source->nspace, source->rank);
    invited_seen = true;

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
        if (PMIX_ERR_NOT_A_MEMBER != rc) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - a leader-excluded invite "
                            "returned %s, expected PMIX_ERR_NOT_A_MEMBER\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            rc = PMIX_ERROR;
            goto done;
        }
        fprintf(stderr, "%d leader-excluded invite refused: PASS\n", myproc.rank);
        rc = PMIX_SUCCESS;
        /* nothing was invited, so there is nothing to wait for */
        goto lastsync;
    }

    /* No invitation is coming - the leader's request is refused before one
     * is issued - so there is nothing for an invitee to wait for. */
    fprintf(stderr, "%s:%d not expecting an invitation\n",
            myproc.nspace, myproc.rank);

lastsync:
    /* final sync across the whole job, leader included */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: final PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* the refusal is what this program checks, so it has to be checked from
     * both ends: the leader saw the status, and no invitee may have seen an
     * invitation, a completion or an abort for a group that was never built.
     * The fence above puts every rank past the point where the leader's
     * request was answered, so anything issued for it would have arrived. */
    if (invited_seen || complete_seen || abort_seen) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - a refused invitation still "
                "produced group events\n", myproc.nspace, myproc.rank);
        rc = PMIX_ERROR;
        goto done;
    }

done:
    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "%s:%d COMPLETE (rc %s)\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    fflush(stderr);
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
