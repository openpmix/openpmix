/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Invite a whole namespace with a single wildcard rank.
 *
 * group_invite.c names every member explicitly. This program asks for the
 * same group with one proc - {nspace, PMIX_RANK_WILDCARD} - and leaves the
 * server to expand it into the concrete ranks. That expansion is a distinct
 * path through pmix_server_group_invite(): it has to find the namespace on
 * the server's list, size the membership from it, and fill the invitation's
 * member array with one entry per rank, because everything that follows -
 * matching answers to members, counting who has responded - is by identity
 * and cannot match a wildcard.
 *
 * Nothing exercised that path, which is how it came to walk the namespace
 * list by clearing the loop's own iterator on a miss; the step expression
 * then read the next pointer out of NULL, so a server whose first namespace
 * was not the one named - its own, in any real launcher - died on the first
 * wildcard invitation it was given.
 *
 * Layout: rank 0 invites {nspace, PMIX_RANK_WILDCARD}, which includes
 * itself. Every rank must receive PMIX_GROUP_CONSTRUCT_COMPLETE - proof the
 * expansion named them all - then fence across the group and destruct it.
 * Requires at least 3 ranks so that a membership short by one is visible.
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

#define GROUP_ID "invwildcard"

static pmix_proc_t myproc;
static volatile bool complete_seen = false;
static volatile bool abort_seen = false;

/* PMIX_GROUP_INVITED handler: an invitee accepts. The leader sees its own
 * invitation reflected back - it is in the membership the wildcard named -
 * and ignores it. The non-blocking join is mandatory: this handler runs on
 * the library's progress thread. */
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

/* An abort means the invitation resolved before every expanded member
 * answered - report it as itself rather than as a timeout. */
static void abort_handler(size_t evhdlr_registration_id, pmix_status_t status,
                          const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                          pmix_info_t results[], size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                info, ninfo, results, nresults);

    fprintf(stderr, "%s:%d ERROR! received PMIX_GROUP_CONSTRUCT_ABORT\n",
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
    pmix_proc_t proc, wildcard;
    uint32_t nprocs;
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
        /* one proc names the entire namespace - the server expands it */
        fprintf(stderr, "%d executing Group_invite for the whole namespace by wildcard\n",
                myproc.rank);
        PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
        results = NULL;
        nresults = 0;
        rc = PMIx_Group_invite(GROUP_ID, &wildcard, 1, NULL, 0, &results, &nresults);
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

    /* every rank the wildcard named must be told the group formed */
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

    /* prove the group is usable, and that it really holds every rank: a
     * fence across it completes only if the membership the expansion built
     * matches the set of procs calling it */
    PMIX_LOAD_PROCID(&proc, GROUP_ID, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence across group FAILED: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    fprintf(stderr, "%d group fence complete\n", myproc.rank);

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
