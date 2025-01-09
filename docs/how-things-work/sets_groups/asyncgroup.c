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
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * Copyright (c) 2019      Triad National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <pmix.h>

/* LOCAL DEFINES */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile bool active;
    pmix_status_t status;
    int count;
    char *answer;
    size_t evhandler_ref;
} mylock_t;

#define DEBUG_CONSTRUCT_LOCK(l)                \
    do {                                       \
        pthread_mutex_init(&(l)->mutex, NULL); \
        pthread_cond_init(&(l)->cond, NULL);   \
        (l)->active = true;                    \
        (l)->status = PMIX_SUCCESS;            \
        (l)->count = 0;                        \
        (l)->answer = NULL;                    \
        (l)->evhandler_ref = 0;                \
    } while (0)

#define DEBUG_DESTRUCT_LOCK(l)              \
    do {                                    \
        pthread_mutex_destroy(&(l)->mutex); \
        pthread_cond_destroy(&(l)->cond);   \
        if (NULL != (l)->answer) {          \
            free((l)->answer);              \
        }                                   \
    } while (0)

#define DEBUG_WAIT_THREAD(lck)                              \
    do {                                                    \
        pthread_mutex_lock(&(lck)->mutex);                  \
        while ((lck)->active) {                             \
            pthread_cond_wait(&(lck)->cond, &(lck)->mutex); \
        }                                                   \
        pthread_mutex_unlock(&(lck)->mutex);                \
    } while (0)

#define DEBUG_WAKEUP_THREAD(lck)              \
    do {                                      \
        pthread_mutex_lock(&(lck)->mutex);    \
        (lck)->active = false;                \
        pthread_cond_broadcast(&(lck)->cond); \
        pthread_mutex_unlock(&(lck)->mutex);  \
    } while (0)


/* LOCAL STATIC VARIABLES */
static pmix_proc_t myproc;
static mylock_t invitedlock;

static void op_callbk(pmix_status_t status, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

static void errhandler_reg_callbk(pmix_status_t status, size_t errhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

static void grpcomplete(size_t evhdlr_registration_id, pmix_status_t status,
                        const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                        pmix_info_t results[], size_t nresults,
                        pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    DEBUG_WAKEUP_THREAD(&invitedlock);
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void invitefn(size_t evhdlr_registration_id, pmix_status_t status, const pmix_proc_t *source,
                     pmix_info_t info[], size_t ninfo, pmix_info_t results[], size_t nresults,
                     pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    char *grp = NULL;
    pmix_status_t rc;

    /* if I am the leader, I can ignore this event */
    if (PMIX_CHECK_PROCID(source, &myproc)) {
        /* mark the event chain as complete */
        if (NULL != cbfunc) {
            cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        }
        return;
    }

    /* search for grp id */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            grp = info[n].value.data.string;
            break;
        }
    }
    invitedlock.status = status;
    rc = PMIx_Group_join_nb(grp, source, PMIX_GROUP_ACCEPT, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s:%d Error in Group_join_nb: %sn", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
    }
    /* mark the event chain as complete */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

int main(int argc, char **argv)
{
    int rc, ret;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs;
    mylock_t lock;
    pmix_status_t code;
    pmix_info_t *results;
    size_t nresults;
    char hostname[1024];

    gethostname(hostname, sizeof(hostname));

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "[%d] Client ns %s rank %d: Running on %s\n",
            (int) getpid(), myproc.nspace, myproc.rank, hostname);

    DEBUG_CONSTRUCT_LOCK(&invitedlock);

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);

    /* get our job size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 4) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 4 processes\n");
        }
        exit(1);
    }
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);


    /* we need to register handlers for invitations */
    DEBUG_CONSTRUCT_LOCK(&lock);
    code = PMIX_GROUP_INVITED;
    PMIx_Register_event_handler(&code, 1, NULL, 0, invitefn, errhandler_reg_callbk, (void *) &lock);
    DEBUG_WAIT_THREAD(&lock);
    rc = lock.status;
    DEBUG_DESTRUCT_LOCK(&lock);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }
    if (2 == myproc.rank || 3 == myproc.rank) {
        /* need to register for group complete event */
        DEBUG_CONSTRUCT_LOCK(&lock);
        code = PMIX_GROUP_CONSTRUCT_COMPLETE;
        PMIx_Register_event_handler(&code, 1, NULL, 0, grpcomplete, errhandler_reg_callbk, (void *) &lock);
        DEBUG_WAIT_THREAD(&lock);
        rc = lock.status;
        DEBUG_DESTRUCT_LOCK(&lock);
        if (PMIX_SUCCESS != rc) {
            goto done;
        }
    }

    /* call fence to sync */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        goto done;
    }

    /* rank=0 constructs a new group */
    if (0 == myproc.rank) {
        fprintf(stderr, "%d executing Group_invite\n", myproc.rank);
        nprocs = 3;
        PMIX_PROC_CREATE(procs, nprocs);
        PMIX_PROC_LOAD(&procs[0], myproc.nspace, 0);
        PMIX_PROC_LOAD(&procs[1], myproc.nspace, 2);
        PMIX_PROC_LOAD(&procs[2], myproc.nspace, 3);
        rc = PMIx_Group_invite("ourgroup", procs, nprocs, NULL, 0, &results, &nresults);
        fprintf(stderr, "Client ns %s rank %d: Group invite complete with status %s!\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        if (PMIX_SUCCESS != rc) {
            exit(1);
        }
        PMIX_PROC_FREE(procs, nprocs);
        fprintf(stderr, "%s:%d Execute fence across group\n", myproc.nspace, myproc.rank);
        PMIX_PROC_LOAD(&proc, "ourgroup", PMIX_RANK_WILDCARD);
        rc = PMIx_Fence(&proc, 1, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Fence across group failed: %d\n",
                    myproc.nspace, myproc.rank, rc);
            exit(1);
        }
        fprintf(stderr, "%d Executing Group_destruct\n", myproc.rank);
        rc = PMIx_Group_destruct("ourgroup", NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            exit(1);
        }
    } else if (2 == myproc.rank || 3 == myproc.rank) {
        /* wait to be invited */
        fprintf(stderr, "%s:%d waiting to join group\n", myproc.nspace, myproc.rank);
        DEBUG_WAIT_THREAD(&invitedlock);
        DEBUG_DESTRUCT_LOCK(&invitedlock);
        fprintf(stderr, "%s:%d Group complete - executing fence across group\n",
                myproc.nspace, myproc.rank);
        PMIX_PROC_LOAD(&proc, "ourgroup", PMIX_RANK_WILDCARD);
        rc = PMIx_Fence(&proc, 1, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Fence across group failed: %s(%d)\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc), rc);
            goto done;
        }
        fprintf(stderr, "%d Executing Group_destruct\n", myproc.rank);
        rc = PMIx_Group_destruct("ourgroup", NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
    }

    /* call fence to sync */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        goto done;
    }

done:
    /* finalize us */

    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (ret = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(ret));
        rc = ret;
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n",
                myproc.nspace, myproc.rank);
    }
    fprintf(stderr, "%s:%d COMPLETE\n", myproc.nspace, myproc.rank);
    fflush(stderr);
    return (rc);
}
