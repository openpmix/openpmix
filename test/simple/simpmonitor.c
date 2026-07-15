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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/*
 * Regression test for heartbeat monitoring via PMIx_Process_monitor.
 *
 * Run as two ranks in one namespace (simptest -n 2):
 *
 *   - rank 0 (the monitored proc) asks its server to watch it for
 *     heartbeats, then deliberately never sends one. It stays connected
 *     until rank 1 has had its say.
 *   - rank 1 (the observer) registers for PMIX_MONITOR_HEARTBEAT_ALERT
 *     and waits for it.
 *
 * When rank 0 goes silent the server's psensor/heartbeat monitor observes
 * an empty window and raises PMIX_MONITOR_HEARTBEAT_ALERT with rank 0 as
 * the source, scoped to the namespace. The source proc is deliberately
 * NOT notified of its own event (it is presumed dead), which is why the
 * observer is a *different* rank. rank 1 receives the alert and checks
 * that its source is really rank 0, then prints "MONITOR TEST PASSED".
 *
 * This guards two things at once:
 *   1. that the monitor API is wired to psensor at all - otherwise no
 *      alert is ever generated and rank 1 times out; and
 *   2. that the alert carries the correct source - the source proc is
 *      passed to an asynchronous notify, so it must outlive the call that
 *      raised it. A stack-allocated source produces a garbage rank/nspace
 *      here.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "include/pmix.h"
#include "simptest.h"

#define MONITORED_RANK 0
#define OBSERVER_RANK  1

static pmix_proc_t myproc;
static mylock_t alertlock;
static volatile int alert_source_ok = 0;

static void hide_unused_params(int x, ...)
{
    va_list ap;
    (void) x;
    va_start(ap, x);
    va_end(ap);
}

/* observer handler for PMIX_MONITOR_HEARTBEAT_ALERT */
static void alert_handler(size_t evhdlr_registration_id, pmix_status_t status,
                          const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                          pmix_info_t results[], size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    int rc = 0;
    hide_unused_params(rc, evhdlr_registration_id, info, ninfo, results, nresults);

    /* the alert must name the monitored proc as its source */
    if (NULL != source && PMIX_CHECK_NSPACE(source->nspace, myproc.nspace)
        && MONITORED_RANK == (int) source->rank) {
        alert_source_ok = 1;
    }
    fprintf(stderr, "Observer %s:%d ALERT RECEIVED (%s) source=%s:%d source_ok=%d\n", myproc.nspace,
            myproc.rank, PMIx_Error_string(status), (NULL == source) ? "NULL" : source->nspace,
            (NULL == source) ? -1 : (int) source->rank, alert_source_ok);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    alertlock.status = status;
    DEBUG_WAKEUP_THREAD(&alertlock);
}

static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lk = (mylock_t *) cbdata;

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status, (unsigned long) evhandler_ref);
    }
    lk->status = status;
    DEBUG_WAKEUP_THREAD(lk);
}

static void infocbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    mylock_t *lk = (mylock_t *) cbdata;
    int rc = 0;
    hide_unused_params(rc, info, ninfo);

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    lk->status = status;
    DEBUG_WAKEUP_THREAD(lk);
}

/* bounded wait on a lock; returns 0 if signalled, -1 on timeout */
static int wait_for_alert(mylock_t *lk, int seconds)
{
    struct timespec ts;
    int rc = 0;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;

    pthread_mutex_lock(&lk->mutex);
    while (lk->active) {
        rc = pthread_cond_timedwait(&lk->cond, &lk->mutex, &ts);
        if (ETIMEDOUT == rc) {
            break;
        }
    }
    rc = lk->active ? -1 : 0;
    pthread_mutex_unlock(&lk->mutex);
    return rc;
}

/* rank 0: ask to be monitored via heartbeats, then go silent */
static int run_monitored(void)
{
    int rc;
    pmix_info_t *monitor, *info;
    mylock_t mylock;
    uint32_t n;

    PMIX_INFO_CREATE(monitor, 1);
    PMIX_INFO_LOAD(&monitor[0], PMIX_MONITOR_HEARTBEAT, NULL, PMIX_POINTER);

    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_MONITOR_ID, "SIMPMONITOR", PMIX_STRING);
    n = 1; /* require a heartbeat every second */
    PMIX_INFO_LOAD(&info[1], PMIX_MONITOR_HEARTBEAT_TIME, &n, PMIX_UINT32);
    n = 1; /* tolerate a single missed beat */
    PMIX_INFO_LOAD(&info[2], PMIX_MONITOR_HEARTBEAT_DROPS, &n, PMIX_UINT32);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    rc = PMIx_Process_monitor_nb(monitor, PMIX_MONITOR_HEARTBEAT_ALERT, info, 3, infocbfunc,
                                 (void *) &mylock);
    if (PMIX_SUCCESS == rc) {
        DEBUG_WAIT_THREAD(&mylock);
        rc = mylock.status;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(monitor, 1);
    PMIX_INFO_FREE(info, 3);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Monitored %s:%d: heartbeat monitor request rejected: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, "Monitored %s:%d: monitoring started, going silent\n", myproc.nspace,
            myproc.rank);
    /* deliberately send NO heartbeats; the second fence below keeps us
     * connected until the observer has seen (or waited out) the alert */
    return 0;
}

/* rank 1: watch for the alert the server raises about rank 0 */
static int run_observer(void)
{
    pmix_status_t code = PMIX_MONITOR_HEARTBEAT_ALERT;
    mylock_t mylock;

    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, alert_handler, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "Observer %s:%d: alert handler registration failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(mylock.status));
        DEBUG_DESTRUCT_LOCK(&mylock);
        return 1;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0, result = 0;
    pmix_proc_t proc;
    pmix_info_t fenceinfo;
    bool flag;
    hide_unused_params(rc, argc, argv);

    DEBUG_CONSTRUCT_LOCK(&alertlock);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "Client ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

    /* the observer arms its handler before anyone synchronizes */
    if (OBSERVER_RANK == (int) myproc.rank) {
        result = run_observer();
    }

    /* barrier: everyone is set up before the monitored proc goes silent */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    flag = false;
    PMIX_INFO_LOAD(&fenceinfo, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, &fenceinfo, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence(1) failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        result = 1;
        goto done;
    }

    if (MONITORED_RANK == (int) myproc.rank) {
        result = run_monitored();
    } else if (OBSERVER_RANK == (int) myproc.rank) {
        if (0 == wait_for_alert(&alertlock, 30) && alert_source_ok) {
            fprintf(stderr, "Observer %s:%d: MONITOR TEST PASSED\n", myproc.nspace, myproc.rank);
            result = 0;
        } else {
            fprintf(stderr, "Observer %s:%d: MONITOR TEST FAILED - no valid heartbeat alert\n",
                    myproc.nspace, myproc.rank);
            result = 1;
        }
    }

    /* second barrier: keeps the monitored proc connected until the
     * observer is done, so it does not disconnect (and cancel its
     * monitor) before the alert has fired and been seen */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, &fenceinfo, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence(2) failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        result = 1;
    }

done:
    DEBUG_DESTRUCT_LOCK(&alertlock);
    if (PMIX_SUCCESS != PMIx_Finalize(NULL, 0)) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Finalize failed\n", myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return result;
}
