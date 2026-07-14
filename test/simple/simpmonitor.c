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
 * This client asks its server to watch it for heartbeats, then
 * deliberately never sends one. The server's psensor/heartbeat monitor
 * must therefore observe an empty window and raise
 * PMIX_MONITOR_HEARTBEAT_ALERT back to us. We register a handler for that
 * code and wait a bounded time for it to fire.
 *
 * The subtlety this guards against: PMIx_Process_monitor_nb returns
 * success for a heartbeat request whether or not a monitor was actually
 * started (an unhandled monitor key used to fall through to a spurious
 * success). So a passing "start" says nothing on its own - only the
 * arrival of the alert proves the sensor was really armed. If the
 * monitor path is not wired to psensor, no alert is ever generated and
 * this test fails on the timeout.
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

static pmix_proc_t myproc;
static mylock_t alertlock;

static void hide_unused_params(int x, ...)
{
    va_list ap;
    (void) x;
    va_start(ap, x);
    va_end(ap);
}

/* handler for PMIX_MONITOR_HEARTBEAT_ALERT - fired by the server when it
 * detects that we have stopped sending heartbeats */
static void alert_handler(size_t evhdlr_registration_id, pmix_status_t status,
                          const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                          pmix_info_t results[], size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    int rc = 0;
    hide_unused_params(rc, evhdlr_registration_id, source, info, ninfo, results, nresults);

    fprintf(stderr, "Client %s:%d MONITOR ALERT RECEIVED (%s)\n", myproc.nspace, myproc.rank,
            PMIx_Error_string(status));

    /* let the notification system know we are done with the event */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }

    /* wake the main thread */
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

/* bounded wait on a lock; returns 0 if the lock was signalled,
 * -1 if we timed out first */
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

int main(int argc, char **argv)
{
    int rc = 0;
    pmix_info_t *monitor, *info;
    pmix_status_t code = PMIX_MONITOR_HEARTBEAT_ALERT;
    mylock_t mylock;
    uint32_t n;
    hide_unused_params(rc, argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "Client ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

    /* register a handler specifically for the heartbeat alert */
    DEBUG_CONSTRUCT_LOCK(&alertlock);
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, alert_handler, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "Client ns %s rank %d: alert handler registration failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(mylock.status));
        DEBUG_DESTRUCT_LOCK(&mylock);
        rc = 1;
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    /* ask to be monitored via heartbeats, with a short (1 second) window
     * so the alert fires quickly once we go silent */
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
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Process_monitor_nb failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIX_INFO_FREE(monitor, 1);
        PMIX_INFO_FREE(info, 3);
        rc = 1;
        goto done;
    }
    DEBUG_WAIT_THREAD(&mylock);
    PMIX_INFO_FREE(monitor, 1);
    PMIX_INFO_FREE(info, 3);
    if (PMIX_SUCCESS != mylock.status && PMIX_OPERATION_SUCCEEDED != mylock.status) {
        fprintf(stderr, "Client ns %s rank %d: heartbeat monitor request rejected: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(mylock.status));
        DEBUG_DESTRUCT_LOCK(&mylock);
        rc = 1;
        goto done;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    fprintf(stderr, "Client ns %s rank %d: heartbeat monitor request accepted\n", myproc.nspace,
            myproc.rank);

    /* deliberately send NO heartbeats and wait for the server to notice
     * and alert us. The window is 1s; allow generous slack. */
    if (0 == wait_for_alert(&alertlock, 30)) {
        fprintf(stderr, "Client ns %s rank %d: MONITOR TEST PASSED\n", myproc.nspace, myproc.rank);
        rc = 0;
    } else {
        fprintf(stderr, "Client ns %s rank %d: MONITOR TEST FAILED - no heartbeat alert\n",
                myproc.nspace, myproc.rank);
        rc = 1;
    }

done:
    DEBUG_DESTRUCT_LOCK(&alertlock);
    if (PMIX_SUCCESS != PMIx_Finalize(NULL, 0)) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Finalize failed\n", myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return rc;
}
