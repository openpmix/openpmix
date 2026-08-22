/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the two things a group formed by invitation gained alongside the
 * membership it already exchanged: a context ID, and the members' endpoint
 * data.
 *
 * Every rank posts one value with PMIx_Put at PMIX_REMOTE scope and then
 * *does not* commit or fence it. That is the point of the test: nothing but
 * the group operation itself can carry that value to the other members, so a
 * PMIx_Get that finds it afterwards can only have been satisfied out of what
 * the group exchanged. Rank 0 then invites the whole job, asking for a
 * context ID; ranks 1..N accept from their PMIX_GROUP_INVITED handler.
 *
 * Once PMIX_GROUP_CONSTRUCT_COMPLETE arrives, every member asserts:
 *
 *   - the event carried a PMIX_GROUP_CONTEXT_ID.  Before openpmix#4068 the
 *     PMIX_GROUP_ASSIGN_CONTEXT_ID directive was accepted and silently
 *     dropped on this path, so no ID appeared here and none reached a
 *     pending PMIx_Group_join.
 *   - every *other* member's value can be read with PMIx_Get, qualified by
 *     that context ID and marked PMIX_OPTIONAL so the request cannot leave
 *     the process.  Values contributed to a group that was assigned an ID are
 *     stored qualified by it, so the qualifier is required rather than
 *     decorative: an unqualified fetch deliberately does not match them.
 *
 * Note the run needs a host that answers the context-id request -
 * test/simple/simptest does, in jctrl_fn.  A host that does not is not a
 * failure of the library, but it is a configuration this test cannot check,
 * so a missing ID is reported as such rather than as a silent pass.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>
#include "examples.h"

#define GROUP_ID  "endptgroup"
#define ENDPT_KEY "endpt-check"

static pmix_proc_t myproc;
static volatile bool complete_seen = false;
static size_t mycid = SIZE_MAX;

/* PMIX_GROUP_INVITED handler: accept, which is also what contributes this
 * process's endpoint data to the group. */
static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t results[], size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, results, nresults);

    /* the leader sees its own invitation reflected back - ignore it */
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
    rc = PMIx_Group_join_nb(grp, source, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s:%d ERROR in PMIx_Group_join_nb: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* PMIX_GROUP_CONSTRUCT_COMPLETE handler: the group formed. Capture the
 * context ID the leader announced, if any. */
static void complete_handler(size_t evhdlr_registration_id, pmix_status_t status,
                             const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                             pmix_info_t results[], size_t nresults,
                             pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_CONTEXT_ID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &mycid, PMIX_SIZE)) {
                mycid = SIZE_MAX;
            }
            break;
        }
    }
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
    pmix_value_t value;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n;
    pmix_info_t *results, dir, quals[2];
    size_t nresults, leadcid = SIZE_MAX;
    bool gotid, gotmbrs;
    int waited;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
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

    /* Post our contribution. Deliberately no PMIx_Commit and no fence: the
     * value must reach the other members through the group operation or not
     * at all, which is what makes the PMIx_Get below meaningful. */
    value.type = PMIX_UINT64;
    value.data.uint64 = 1234UL + (unsigned long) myproc.rank;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, ENDPT_KEY, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

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

    /* sync so every rank has posted and registered before anyone invites */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (0 == myproc.rank) {
        PMIX_PROC_CREATE(procs, nprocs);
        for (n = 0; n < nprocs; n++) {
            PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
        }
        PMIX_INFO_LOAD(&dir, PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);
        results = NULL;
        nresults = 0;
        rc = PMIx_Group_invite(GROUP_ID, procs, nprocs, &dir, 1, &results, &nresults);
        PMIX_PROC_FREE(procs, nprocs);
        PMIX_INFO_DESTRUCT(&dir);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_invite FAILED: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            if (NULL != results) {
                PMIX_INFO_FREE(results, nresults);
            }
            goto done;
        }
        /* The leader asked for a context ID, so its own results must carry
         * one - along with the group it just formed. Everybody else learns
         * these from the completion event; the leader is the one process
         * that does not receive its own invitation, and returning them here
         * is the only way this API hands them to the process that asked. */
        gotid = false;
        gotmbrs = false;
        for (n = 0; n < nresults; n++) {
            if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_CONTEXT_ID)) {
                if (PMIX_SUCCESS != PMIx_Value_get_number(&results[n].value, &leadcid,
                                                          PMIX_SIZE)) {
                    leadcid = SIZE_MAX;
                }
                gotid = true;
            } else if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_MEMBERSHIP)) {
                gotmbrs = true;
            }
        }
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        if (!gotid || SIZE_MAX == leadcid || !gotmbrs) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - PMIx_Group_invite returned no "
                    "%s\n", myproc.nspace, myproc.rank,
                    gotid ? "membership" : "context id");
            rc = PMIX_ERR_NOT_FOUND;
            goto done;
        }
        fprintf(stderr, "%d INVITE results carry the group: PASS (cid %lu)\n", myproc.rank,
                (unsigned long) leadcid);
    }

    /* every member, leader included, learns the outcome from the event */
    for (waited = 0; !complete_seen && waited < 100; waited++) {
        usleep(100000); /* 0.1s; up to 10s total */
    }
    if (!complete_seen) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - never received "
                "PMIX_GROUP_CONSTRUCT_COMPLETE\n", myproc.nspace, myproc.rank);
        rc = PMIX_ERR_TIMEOUT;
        goto done;
    }

    if (SIZE_MAX == mycid) {
        fprintf(stderr, "Client ns %s rank %d: FAILED - no PMIX_GROUP_CONTEXT_ID in the "
                "construct-complete event\n", myproc.nspace, myproc.rank);
        rc = PMIX_ERR_NOT_FOUND;
        goto done;
    }
    fprintf(stderr, "%d CONTEXT_ID received: PASS (cid %lu)\n", myproc.rank,
            (unsigned long) mycid);

    /* Read back what every other member contributed. PMIX_OPTIONAL keeps the
     * request from leaving this process, so a value that turns up can only
     * have come from the group exchange; the context id is a *qualifier*,
     * because that is how a contribution to a group carrying one is stored. */
    PMIX_INFO_LOAD(&quals[0], PMIX_GROUP_CONTEXT_ID, &mycid, PMIX_SIZE);
    PMIX_INFO_SET_QUALIFIER(&quals[0]);
    PMIX_INFO_LOAD(&quals[1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
    for (n = 0; n < nprocs; n++) {
        /* our own contribution included: a group carrying a context ID stores
         * every member's values qualified by it, ours no differently from
         * anybody else's, which is what the collective PMIx_Group_construct
         * path has always done */
        PMIX_LOAD_PROCID(&proc, myproc.nspace, n);
        rc = PMIx_Get(&proc, ENDPT_KEY, quals, 2, &val);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - no endpoint data from rank %d: %s\n",
                    myproc.nspace, myproc.rank, (int) n, PMIx_Error_string(rc));
            goto endpt_done;
        }
        if (PMIX_UINT64 != val->type || (1234UL + (unsigned long) n) != val->data.uint64) {
            fprintf(stderr, "Client ns %s rank %d: FAILED - rank %d endpoint data wrong "
                    "(type %d value %lu)\n", myproc.nspace, myproc.rank, (int) n,
                    (int) val->type, (unsigned long) val->data.uint64);
            PMIX_VALUE_RELEASE(val);
            rc = PMIX_ERR_BAD_PARAM;
            goto endpt_done;
        }
        PMIX_VALUE_RELEASE(val);
    }
    fprintf(stderr, "%d ENDPT data verified for every member: PASS\n", myproc.rank);

endpt_done:
    PMIX_INFO_DESTRUCT(&quals[0]);
    PMIX_INFO_DESTRUCT(&quals[1]);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    /* tear the group back down - a collective over the membership, so every
     * member must call it */
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
    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "%s:%d COMPLETE (rc %s)\n", myproc.nspace, myproc.rank,
            PMIx_Error_string(rc));
    fflush(stderr);
    return (PMIX_SUCCESS == rc) ? 0 : 1;
}
