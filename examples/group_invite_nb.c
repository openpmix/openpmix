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
 * The non-blocking ends of the invite/join model.
 *
 * group_invite.c drives the blocking PMIx_Group_invite and passes no callback
 * to PMIx_Group_join_nb, so neither of the two things this test is about is
 * exercised there:
 *
 *   - The leader invites with PMIx_Group_invite_nb and waits for its own
 *     callback.  That form used to be inert: it resolved the invitation and
 *     then stopped, announcing nothing at all -- no PMIX_GROUP_INVITE_FAILED,
 *     no PMIX_GROUP_CONSTRUCT_COMPLETE, no PMIX_GROUP_CONSTRUCT_ABORT -- so no
 *     group ever formed, and it leaked its tracker and event registration on
 *     every call.  The announcement was built out of blocking notifications
 *     and so could only run on the blocking form's own thread.
 *
 *   - Each invitee accepts with a real PMIx_Group_join_nb callback and asserts
 *     on what it is handed.  The join must complete when the *construct*
 *     resolves, which is what PMIx_Group_join.3.rst has always specified, and
 *     it must carry the group id and the membership the leader announced.  It
 *     used to complete as soon as the acceptance had been handed to the local
 *     event system -- much earlier, and carrying no group data at all, so the
 *     results always came back empty.
 *
 * Both are openpmix#4059 follow-ons.  Every rank still ends by fencing across
 * the formed group and destructing it, so a group that forms but is unusable
 * fails here too.
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

#define GROUP_ID "nbgroup"

static pmix_proc_t myproc;
static volatile bool complete_seen = false;
static volatile bool invite_done = false;
static pmix_status_t invite_status = PMIX_ERROR;
static volatile bool join_done = false;
static pmix_status_t join_status = PMIX_ERROR;
static volatile int join_nmembers = -1;
static char join_grpid[PMIX_MAX_NSLEN + 1] = {0};

/* PMIx_Group_join_nb completion: this is the assertion subject.  It must fire
 * when the construct resolves, and be handed the group's identity. */
static void join_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    size_t n;
    EXAMPLES_HIDE_UNUSED_PARAMS(cbdata);

    join_status = status;
    join_nmembers = 0;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            snprintf(join_grpid, sizeof(join_grpid), "%s", info[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_MEMBERSHIP)) {
            join_nmembers = (int) info[n].value.data.darray->size;
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    join_done = true;
}

/* PMIX_GROUP_INVITED handler: an invitee accepts the invitation.  The leader
 * does not see its own invitation here -- it is not among the invitees. */
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

    /* the non-blocking form is mandatory here: we are on the library's
     * progress thread, so the blocking form would deadlock */
    rc = PMIx_Group_join_nb(grp, source, PMIX_GROUP_ACCEPT, NULL, 0, join_cbfunc, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s:%d ERROR in PMIx_Group_join_nb: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        join_status = rc;
        join_done = true;
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_CONSTRUCT_COMPLETE handler: the group finished forming */
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

/* PMIx_Group_invite_nb completion on the leader */
static void invite_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                          pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(info, ninfo, cbdata);

    invite_status = status;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    invite_done = true;
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

/* bounded wait on a flag the progress thread sets; returns 0 on timeout */
static int wait_for(volatile bool *flag, int tenths)
{
    int i;

    for (i = 0; !*flag && i < tenths; i++) {
        usleep(100000);
    }
    return *flag ? 1 : 0;
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n;
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
    if (nprocs < 2) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 2 processes\n");
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

    /* sync so every rank has its handlers in place before anyone invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        /* the leader invites the whole job, itself included, and does not
         * block: the invitation completes through invite_cbfunc */
        fprintf(stderr, "%d executing Group_invite_nb for the whole job\n", myproc.rank);
        PMIX_PROC_CREATE(procs, nprocs);
        for (n = 0; n < nprocs; n++) {
            PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
        }
        rc = PMIx_Group_invite_nb(GROUP_ID, procs, nprocs, NULL, 0, invite_cbfunc, NULL);
        PMIX_PROC_FREE(procs, nprocs);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_invite_nb FAILED: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        /* the callback must fire, and report success */
        if (!wait_for(&invite_done, 200)) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - PMIx_Group_invite_nb never "
                    "completed\n", myproc.nspace, myproc.rank);
            rc = PMIX_ERR_TIMEOUT;
            goto done;
        }
        if (PMIX_SUCCESS != invite_status) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - PMIx_Group_invite_nb reported %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(invite_status));
            rc = invite_status;
            goto done;
        }
        fprintf(stderr, "%d Group_invite_nb completed: PASS\n", myproc.rank);
    } else {
        /* an invitee: our handler accepts, and join_cbfunc must be called
         * when the construct resolves - not when the acceptance goes out */
        fprintf(stderr, "%s:%d waiting to be invited and to join the group\n",
                myproc.nspace, myproc.rank);
        if (!wait_for(&join_done, 200)) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - PMIx_Group_join_nb never "
                    "completed\n", myproc.nspace, myproc.rank);
            rc = PMIX_ERR_TIMEOUT;
            goto done;
        }
        if (PMIX_SUCCESS != join_status) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - join completed with %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(join_status));
            rc = join_status;
            goto done;
        }
        /* it must carry the group's identity, which is only knowable once
         * the construct has resolved */
        if (0 != strcmp(join_grpid, GROUP_ID)) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - join returned group id '%s', "
                    "expected '%s'\n", myproc.nspace, myproc.rank, join_grpid, GROUP_ID);
            rc = PMIX_ERR_BAD_PARAM;
            goto done;
        }
        if (join_nmembers != (int) nprocs) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - join returned %d members, "
                    "expected %d\n", myproc.nspace, myproc.rank, join_nmembers, (int) nprocs);
            rc = PMIX_ERR_BAD_PARAM;
            goto done;
        }
        fprintf(stderr, "%d Group_join_nb completed with group %s and %d members: PASS\n",
                myproc.rank, join_grpid, join_nmembers);
    }

    /* every member (leader and invitees) must receive CONSTRUCT_COMPLETE */
    if (!wait_for(&complete_seen, 100)) {
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

    fprintf(stderr, "%d executing Group_destruct\n", myproc.rank);
    rc = PMIx_Group_destruct(GROUP_ID, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct FAILED: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

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
