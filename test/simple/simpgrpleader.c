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
 * Exercise leader failure and app-driven reselection in the invite/join model.
 *
 * Rank 0 is the leader. Each other rank joins the group led by rank 0 with
 * PMIx_Group_join_nb(..., PMIX_GROUP_ACCEPT), which arms the library's
 * leader-failure watch on the leader. The leader then dies before the construct
 * completes. Because each joiner is waiting on the leader, the library surfaces
 * the loss to each of them as a PMIX_GROUP_LEADER_FAILED event naming the
 * leader. The application then drives reselection (a policy the library leaves
 * to it): the lowest surviving joiner (rank 1) declares itself the new leader
 * and announces that with PMIX_GROUP_LEADER_SELECTED; the remaining joiners
 * receive it. Every joiner asserts it saw LEADER_FAILED for rank 0, and the
 * non-electing joiners assert they saw LEADER_SELECTED naming rank 1.
 *
 * See src/client/pmix_client_group.c (setup_leader_watch / leader_watch_handler)
 * and docs/how-things-work/collectives.
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
static const char *thegrp = "leadergroup";

static volatile int reg_count = 0;
static volatile int leader_failed_seen = 0;
static pmix_proc_t failed_leader;
static volatile int leader_selected_seen = 0;
static pmix_proc_t new_leader;

static void regcbfunc(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, evhandler_ref, cbdata);
    reg_count++;
}

static void leaderfailed_handler(size_t evhdlr_registration_id, pmix_status_t status,
                                 const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                 pmix_info_t *results, size_t nresults,
                                 pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            memcpy(&failed_leader, info[n].value.data.proc, sizeof(pmix_proc_t));
        }
    }
    leader_failed_seen = 1;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void leaderselected_handler(size_t evhdlr_registration_id, pmix_status_t status,
                                   const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                   pmix_info_t *results, size_t nresults,
                                   pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            memcpy(&new_leader, info[n].value.data.proc, sizeof(pmix_proc_t));
        }
    }
    leader_selected_seen = 1;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, leader;
    uint32_t nprocs, n;
    pmix_status_t code;
    int i;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client %s:%d PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 3) {
        fprintf(stderr, "Client %s:%d FAIL: this test requires at least 3 processes\n",
                myproc.nspace, myproc.rank);
        exit(1);
    }

    /* register the leader-failure and leader-selected handlers */
    code = PMIX_GROUP_LEADER_FAILED;
    PMIx_Register_event_handler(&code, 1, NULL, 0, leaderfailed_handler, regcbfunc, NULL);
    code = PMIX_GROUP_LEADER_SELECTED;
    PMIx_Register_event_handler(&code, 1, NULL, 0, leaderselected_handler, regcbfunc, NULL);
    while (reg_count < 2) {
        usleep(10000);
    }

    /* sync everyone up first - all ranks are alive for this fence */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* rank 0 is the leader */
    PMIX_LOAD_PROCID(&leader, myproc.nspace, 0);

    if (0 == myproc.rank) {
        /* the leader: give the joiners time to arm their watch on us, then die
         * before the construct completes. Exit 0 so the harness does not treat
         * this as a real failure - the dropped connection is what drives the
         * test. */
        fprintf(stderr, "%d is the leader, now leaving (simulated leader failure)\n",
                myproc.rank);
        sleep(2);
        _exit(0);
    }

    /* a joiner: accept into the group led by rank 0. This arms the library's
     * leader-failure watch on the leader. (No prior invitation is required to
     * join a group whose leader is known.) */
    rc = PMIx_Group_join_nb(thegrp, &leader, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client %s:%d FAIL: Group_join_nb: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* wait for the library to surface the leader's loss */
    for (i = 0; i < 1000 && 0 == leader_failed_seen; i++) {
        usleep(10000);
    }
    if (!leader_failed_seen) {
        fprintf(stderr, "Client %s:%d FAIL: never received PMIX_GROUP_LEADER_FAILED\n",
                myproc.nspace, myproc.rank);
        exit(1);
    }
    if (0 != failed_leader.rank) {
        fprintf(stderr, "Client %s:%d FAIL: leader-failed named rank %d, expected 0\n",
                myproc.nspace, myproc.rank, failed_leader.rank);
        exit(1);
    }
    fprintf(stderr, "rank %d saw LEADER_FAILED for leader %d\n", myproc.rank, failed_leader.rank);

    /* reselection policy (the application's, not the library's): the lowest
     * surviving joiner becomes the new leader and announces it */
    if (1 == myproc.rank) {
        pmix_proc_t *others;
        pmix_data_array_t darray;
        pmix_info_t cinfo[4];
        uint32_t k = 0;

        PMIX_PROC_CREATE(others, nprocs - 2);
        for (n = 2; n < nprocs; n++) {
            PMIX_PROC_LOAD(&others[k], myproc.nspace, n);
            k++;
        }
        darray.type = PMIX_PROC;
        darray.array = others;
        darray.size = nprocs - 2;
        PMIX_INFO_LOAD(&cinfo[0], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
        PMIX_INFO_LOAD(&cinfo[1], PMIX_EVENT_AFFECTED_PROC, &myproc, PMIX_PROC);
        PMIX_INFO_LOAD(&cinfo[2], PMIX_GROUP_ID, thegrp, PMIX_STRING);
        PMIX_INFO_LOAD(&cinfo[3], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        rc = PMIx_Notify_event(PMIX_GROUP_LEADER_SELECTED, &myproc, PMIX_RANGE_CUSTOM,
                               cinfo, 4, NULL, NULL);
        PMIX_PROC_FREE(others, nprocs - 2);
        PMIX_INFO_DESTRUCT(&cinfo[0]);
        PMIX_INFO_DESTRUCT(&cinfo[1]);
        PMIX_INFO_DESTRUCT(&cinfo[2]);
        PMIX_INFO_DESTRUCT(&cinfo[3]);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client %s:%d FAIL: could not announce LEADER_SELECTED: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            exit(1);
        }
        fprintf(stderr, "rank %d elected as new leader\n", myproc.rank);
    } else {
        for (i = 0; i < 1000 && 0 == leader_selected_seen; i++) {
            usleep(10000);
        }
        if (!leader_selected_seen) {
            fprintf(stderr, "Client %s:%d FAIL: never received PMIX_GROUP_LEADER_SELECTED\n",
                    myproc.nspace, myproc.rank);
            exit(1);
        }
        if (1 != new_leader.rank) {
            fprintf(stderr, "Client %s:%d FAIL: leader-selected named rank %d, expected 1\n",
                    myproc.nspace, myproc.rank, new_leader.rank);
            exit(1);
        }
        fprintf(stderr, "rank %d saw LEADER_SELECTED new leader %d\n", myproc.rank, new_leader.rank);
    }

    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    fflush(stderr);
    return 0;
}
