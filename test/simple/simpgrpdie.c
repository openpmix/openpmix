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
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the PMIX_GROUP_MEMBER_FAILED event synthesized by the PMIx server
 * when a member is lost before contributing to a group construct.
 *
 * The whole job is the group membership. Every rank registers a handler for
 * PMIX_GROUP_MEMBER_FAILED, then all ranks sync with a fence. The last rank is
 * the victim: it leaves WITHOUT calling PMIx_Group_construct and WITHOUT
 * calling PMIx_Finalize (after a short delay so the survivors have entered the
 * construct and their server has built the local tracker), so the server sees
 * a dropped connection. Every surviving rank calls PMIx_Group_construct; the
 * server accounts for the departed member, completes the construct on the
 * survivors, and - the behavior under test - notifies each survivor with a
 * PMIX_GROUP_MEMBER_FAILED event naming the victim. Each survivor verifies it
 * received that event for the victim and prints a distinctive success line.
 *
 * See src/server/pmix_server_group.c (notify_local_members_of_loss) and
 * docs/how-things-work/collectives.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"
#include "src/util/pmix_output.h"

static pmix_proc_t myproc;
static volatile int member_failed = 0;
static pmix_proc_t failed_proc;
static volatile int reg_active = -1;

static void memfailed_handler(size_t evhdlr_registration_id, pmix_status_t status,
                              const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                              pmix_info_t results[], size_t nresults,
                              pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            memcpy(&failed_proc, info[n].value.data.proc, sizeof(pmix_proc_t));
        }
    }
    member_failed = 1;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void regcbfunc(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhandler_ref, cbdata);
    reg_active = status;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n, victim;
    pmix_info_t *info;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t code = PMIX_GROUP_MEMBER_FAILED;
    int i;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    /* get our job size */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client %s:%d PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 2) {
        fprintf(stderr, "Client %s:%d FAIL: this test requires at least 2 processes\n",
                myproc.nspace, myproc.rank);
        exit(1);
    }

    /* register a handler for the member-failed event */
    reg_active = -1;
    PMIx_Register_event_handler(&code, 1, NULL, 0, memfailed_handler, regcbfunc, NULL);
    while (-1 == reg_active) {
        usleep(10000);
    }
    if (PMIX_SUCCESS != reg_active) {
        fprintf(stderr, "Client %s:%d FAIL: could not register handler: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(reg_active));
        exit(1);
    }

    /* sync everyone up first - all ranks are alive for this fence */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* the entire job is the group membership; the last rank is the victim */
    victim = nprocs - 1;

    if (myproc.rank == victim) {
        /* leave before contributing, without finalizing, so the server sees a
         * dropped connection and must run its lost-connection accounting. Exit
         * 0 so the harness does not treat this as a real failure. */
        fprintf(stderr, "%d leaving BEFORE group construct (simulated failure)\n", myproc.rank);
        sleep(2);
        _exit(0);
    }

    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }
    /* request fault-tolerant collective tracking so the construct completes on
     * the survivors (and each learns of the loss via PMIX_GROUP_MEMBER_FAILED)
     * when the victim drops before contributing. Without this flag the construct
     * would instead abort - see simpgrpcabort. */
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_GROUP_FT_COLLECTIVE, NULL, PMIX_BOOL);

    rc = PMIx_Group_construct("diegroup", procs, nprocs, info, 2, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_INFO_FREE(info, 2);

    /* The construct must complete on the survivors (success or a
     * lost-connection/partial status), not hang. */
    fprintf(stderr, "%d Group construct complete on survivors: status %s\n",
            myproc.rank, PMIx_Error_string(rc));

    /* the member-failed event is delivered asynchronously right after the
     * construct completes - give it a bounded window to arrive */
    for (i = 0; i < 500 && 0 == member_failed; i++) {
        usleep(10000);
    }

    if (!member_failed) {
        fprintf(stderr, "Client %s:%d FAIL: never received PMIX_GROUP_MEMBER_FAILED\n",
                myproc.nspace, myproc.rank);
        PMIx_Finalize(NULL, 0);
        exit(1);
    }
    if (failed_proc.rank != victim ||
        0 != strncmp(failed_proc.nspace, myproc.nspace, PMIX_MAX_NSLEN)) {
        fprintf(stderr, "Client %s:%d FAIL: member-failed named %s:%d, expected victim rank %d\n",
                myproc.nspace, myproc.rank, failed_proc.nspace, failed_proc.rank, victim);
        PMIx_Finalize(NULL, 0);
        exit(1);
    }

    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    fprintf(stderr, "Client %s:%d saw member-failed for victim rank %d OK\n",
            myproc.nspace, myproc.rank, victim);
    fflush(stderr);
    return 0;
}
