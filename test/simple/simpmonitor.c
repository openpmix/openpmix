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
 * rank 0 also asks for a *file* monitor on a file it creates and then
 * never touches, so the other psensor component gets the same end-to-end
 * treatment: the server's file monitor must notice the stale mtime and
 * raise PMIX_MONITOR_FILE_ALERT. rank 1 waits for that too and prints
 * "FILE MONITOR TEST PASSED".
 *
 * The heartbeat request names its own status code (MONITOR_TEST_CODE)
 * rather than PMIX_MONITOR_HEARTBEAT_ALERT, and the file request names
 * none at all (PMIX_SUCCESS). Between them they cover both halves of the
 * "error" argument PMIx_Process_monitor(3) documents: the code the
 * requestor asked for is what gets raised, and asking for nothing leaves
 * the framework's own alert code standing. psensor used to hardcode its
 * alert either way, so the first request would have produced the wrong
 * status.
 *
 * rank 0 also starts two more monitors that must never fire, and that is
 * the point of them. They raise a status of their own
 * (MONITOR_SILENT_CODE) so that the observer can tell them apart from
 * the two above, which name the same source:
 *
 *   - a heartbeat monitor with a drop allowance far larger than the run.
 *     rank 0 never beats at all, so an implementation that parses
 *     PMIX_MONITOR_HEARTBEAT_DROPS and then ignores it alerts within a
 *     second. One that honors it stays silent for the whole test.
 *   - a file monitor, given a PMIX_MONITOR_ID and then cancelled with
 *     PMIX_MONITOR_CANCEL before it can trip. The cancel only works if
 *     the id was recorded when the monitor was started *and* the cancel
 *     key reaches psensor rather than falling through to the
 *     resource-usage path, which answers success without stopping
 *     anything.
 *
 * Either failure shows up as a MONITOR_SILENT_CODE event at the
 * observer, and the run prints "SILENCE TEST FAILED".
 *
 * This guards several things at once:
 *   1. that the monitor API is wired to psensor at all - otherwise no
 *      alert is ever generated and rank 1 times out;
 *   2. that the alert carries the correct source - the source proc is
 *      passed to an asynchronous notify, so it must outlive the call that
 *      raised it. A stack-allocated source produces a garbage rank/nspace
 *      here;
 *   3. that both monitors keep sampling on their own timer. Each tracker
 *      arms a persistent timer, and the sampler must not re-arm it - so
 *      an alert that never comes can mean the timer stopped after its
 *      first fire, not just that the framework was never driven; and
 *   4. that a monitor honors the status code, the drop allowance and the
 *      cancel handle it was given, as described above.
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

/* The status the heartbeat monitor is asked to raise. Host-defined codes
 * are supposed to be built off PMIX_EXTERNAL_ERR_BASE so they cannot
 * collide with the library's own, and using one here means the alert we
 * receive can only have come from the code we supplied. */
#define MONITOR_TEST_CODE (PMIX_EXTERNAL_ERR_BASE - 1)

/* The status raised by the two monitors that are supposed to stay quiet.
 * They watch the same process as the two above, so the source proc
 * cannot tell them apart - only the code can. */
#define MONITOR_SILENT_CODE (PMIX_EXTERNAL_ERR_BASE - 2)

static pmix_proc_t myproc;
static mylock_t alertlock;
static mylock_t filelock;
static volatile int alert_source_ok = 0;
static volatile int file_alert_source_ok = 0;
/* set if the observer ever sees a MONITOR_SILENT_CODE event - the two
 * monitors that raise it are supposed to stay quiet */
static volatile int silent_alert_seen = 0;
static char watchfile[1024] = {0};
static char obs_watchfile[1024] = {0};

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

/* observer handler for PMIX_MONITOR_FILE_ALERT */
static void file_alert_handler(size_t evhdlr_registration_id, pmix_status_t status,
                               const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                               pmix_info_t results[], size_t nresults,
                               pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    int rc = 0;
    hide_unused_params(rc, evhdlr_registration_id, info, ninfo, results, nresults);

    if (NULL != source && PMIX_CHECK_NSPACE(source->nspace, myproc.nspace)
        && MONITORED_RANK == (int) source->rank) {
        file_alert_source_ok = 1;
    }
    fprintf(stderr, "Observer %s:%d FILE ALERT RECEIVED (%s) source=%s:%d source_ok=%d\n",
            myproc.nspace, myproc.rank, PMIx_Error_string(status),
            (NULL == source) ? "NULL" : source->nspace,
            (NULL == source) ? -1 : (int) source->rank, file_alert_source_ok);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    filelock.status = status;
    DEBUG_WAKEUP_THREAD(&filelock);
}

/* observer handler for MONITOR_SILENT_CODE - reaching this at all is a
 * failure, so it records the fact and returns */
static void silent_alert_handler(size_t evhdlr_registration_id, pmix_status_t status,
                                 const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                 pmix_info_t results[], size_t nresults,
                                 pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    int rc = 0;
    hide_unused_params(rc, evhdlr_registration_id, info, ninfo, results, nresults);

    silent_alert_seen = 1;
    fprintf(stderr, "Observer %s:%d UNEXPECTED ALERT (%d) source=%s:%d\n", myproc.nspace,
            myproc.rank, (int) status, (NULL == source) ? "NULL" : source->nspace,
            (NULL == source) ? -1 : (int) source->rank);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
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
    rc = PMIx_Process_monitor_nb(monitor, MONITOR_TEST_CODE, info, 3, infocbfunc,
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

/* rank 0: create a file, ask the server to watch it for modification,
 * and then never touch it again. The file monitor must notice that the
 * mtime has stopped moving and alert. Note the drop count: the first
 * sample only records the current mtime, so it takes ndrops+1 samples to
 * trip - which is exactly what proves the tracker's timer kept firing. */
static int run_file_monitored(void)
{
    int rc;
    pmix_info_t *monitor, *info;
    mylock_t mylock;
    uint32_t n;
    FILE *fp;
    bool flag = true;

    if (NULL == getcwd(watchfile, sizeof(watchfile) - 32)) {
        fprintf(stderr, "Monitored %s:%d: getcwd failed\n", myproc.nspace, myproc.rank);
        return 1;
    }
    snprintf(watchfile + strlen(watchfile), 32, "/simpmon-%lu.watch",
             (unsigned long) getpid());
    fp = fopen(watchfile, "w");
    if (NULL == fp) {
        fprintf(stderr, "Monitored %s:%d: cannot create %s\n", myproc.nspace, myproc.rank,
                watchfile);
        return 1;
    }
    fprintf(fp, "watch me\n");
    fclose(fp);

    PMIX_INFO_CREATE(monitor, 1);
    PMIX_INFO_LOAD(&monitor[0], PMIX_MONITOR_FILE, watchfile, PMIX_STRING);

    PMIX_INFO_CREATE(info, 4);
    PMIX_INFO_LOAD(&info[0], PMIX_MONITOR_ID, "SIMPMONITOR-FILE", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_MONITOR_FILE_MODIFY, &flag, PMIX_BOOL);
    n = 1; /* stat it every second */
    PMIX_INFO_LOAD(&info[2], PMIX_MONITOR_FILE_CHECK_TIME, &n, PMIX_UINT32);
    n = 1; /* tolerate one unchanged check */
    PMIX_INFO_LOAD(&info[3], PMIX_MONITOR_FILE_DROPS, &n, PMIX_UINT32);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    rc = PMIx_Process_monitor_nb(monitor, PMIX_SUCCESS, info, 4, infocbfunc,
                                 (void *) &mylock);
    if (PMIX_SUCCESS == rc) {
        DEBUG_WAIT_THREAD(&mylock);
        rc = mylock.status;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(monitor, 1);
    PMIX_INFO_FREE(info, 4);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Monitored %s:%d: file monitor request rejected: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, "Monitored %s:%d: file monitoring started on %s\n", myproc.nspace,
            myproc.rank, watchfile);
    return 0;
}

/* rank 0: arm two more monitors on itself that must never fire. Both
 * raise MONITOR_SILENT_CODE, which nothing else in this test uses, so
 * the observer can tell them from the two that are supposed to alert.
 *
 * The heartbeat one is given a drop allowance no run can exhaust. rank 0
 * never beats at all, so a heartbeat monitor that ignores
 * PMIX_MONITOR_HEARTBEAT_DROPS alerts within a window.
 *
 * The file one is given a PMIX_MONITOR_ID and then cancelled. A cancel
 * has to reach psensor - the resource-usage path answers success for an
 * id it has never heard of, so a cancel that falls through looks like it
 * worked - and psensor has to have recorded the id when the monitor was
 * started, or nothing matches it. The cancel names only this monitor, so
 * the real ones armed above must survive it.
 *
 * Returns 0 if both requests (and the cancel) were accepted. */
static int arm_silent_monitors(void)
{
    int rc;
    pmix_info_t *monitor, *info;
    mylock_t mylock;
    uint32_t n;
    FILE *fp;
    bool flag = true;

    /* --- heartbeat, with a drop allowance we cannot spend --- */
    PMIX_INFO_CREATE(monitor, 1);
    PMIX_INFO_LOAD(&monitor[0], PMIX_MONITOR_HEARTBEAT, NULL, PMIX_POINTER);
    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_MONITOR_ID, "SIMPMONITOR-CAPPED", PMIX_STRING);
    n = 1;
    PMIX_INFO_LOAD(&info[1], PMIX_MONITOR_HEARTBEAT_TIME, &n, PMIX_UINT32);
    n = 100000; /* far more empty windows than this test can produce */
    PMIX_INFO_LOAD(&info[2], PMIX_MONITOR_HEARTBEAT_DROPS, &n, PMIX_UINT32);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    rc = PMIx_Process_monitor_nb(monitor, MONITOR_SILENT_CODE, info, 3, infocbfunc,
                                 (void *) &mylock);
    if (PMIX_SUCCESS == rc) {
        DEBUG_WAIT_THREAD(&mylock);
        rc = mylock.status;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(monitor, 1);
    PMIX_INFO_FREE(info, 3);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Monitored %s:%d: drop-allowance monitor rejected: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        return 1;
    }

    /* --- file, cancelled before it can trip --- */
    if (NULL == getcwd(obs_watchfile, sizeof(obs_watchfile) - 32)) {
        fprintf(stderr, "Monitored %s:%d: getcwd failed\n", myproc.nspace, myproc.rank);
        return 1;
    }
    snprintf(obs_watchfile + strlen(obs_watchfile), 32, "/simpmon-obs-%lu.watch",
             (unsigned long) getpid());
    fp = fopen(obs_watchfile, "w");
    if (NULL == fp) {
        fprintf(stderr, "Monitored %s:%d: cannot create %s\n", myproc.nspace, myproc.rank,
                obs_watchfile);
        return 1;
    }
    fprintf(fp, "watch me too\n");
    fclose(fp);

    PMIX_INFO_CREATE(monitor, 1);
    PMIX_INFO_LOAD(&monitor[0], PMIX_MONITOR_FILE, obs_watchfile, PMIX_STRING);
    PMIX_INFO_CREATE(info, 4);
    PMIX_INFO_LOAD(&info[0], PMIX_MONITOR_ID, "SIMPMONITOR-CANCEL", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_MONITOR_FILE_MODIFY, &flag, PMIX_BOOL);
    n = 1;
    PMIX_INFO_LOAD(&info[2], PMIX_MONITOR_FILE_CHECK_TIME, &n, PMIX_UINT32);
    n = 0; /* alert on the first check that shows no change */
    PMIX_INFO_LOAD(&info[3], PMIX_MONITOR_FILE_DROPS, &n, PMIX_UINT32);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    rc = PMIx_Process_monitor_nb(monitor, MONITOR_SILENT_CODE, info, 4, infocbfunc,
                                 (void *) &mylock);
    if (PMIX_SUCCESS == rc) {
        DEBUG_WAIT_THREAD(&mylock);
        rc = mylock.status;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(monitor, 1);
    PMIX_INFO_FREE(info, 4);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Monitored %s:%d: cancellable file monitor rejected: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        return 1;
    }

    /* now take it back */
    PMIX_INFO_CREATE(monitor, 1);
    PMIX_INFO_LOAD(&monitor[0], PMIX_MONITOR_CANCEL, "SIMPMONITOR-CANCEL", PMIX_STRING);
    DEBUG_CONSTRUCT_LOCK(&mylock);
    rc = PMIx_Process_monitor_nb(monitor, PMIX_SUCCESS, NULL, 0, infocbfunc, (void *) &mylock);
    if (PMIX_SUCCESS == rc) {
        DEBUG_WAIT_THREAD(&mylock);
        rc = mylock.status;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);
    PMIX_INFO_FREE(monitor, 1);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Monitored %s:%d: monitor cancel rejected: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, "Monitored %s:%d: silent monitors armed (%s cancelled)\n", myproc.nspace,
            myproc.rank, obs_watchfile);
    return 0;
}

/* The blocking form of PMIx_Process_monitor with PMIX_SEND_HEARTBEAT
 * used to hang forever: the _nb entry point fired the beat and returned
 * success without ever invoking the callback, so the blocking wrapper
 * waited on a lock nobody would wake. This has to run on a connected
 * client - a singleton is turned away at the "am I connected?" check
 * long before it reaches the heartbeat branch - and it has to run on the
 * observer, since the monitored rank must stay silent for the alert to
 * fire. Returns 0 if the call came back at all. */
static int send_one_heartbeat(void)
{
    pmix_status_t rc;
    pmix_info_t monitor;
    pmix_info_t *results = NULL;
    size_t nresults = 0;

    PMIX_INFO_LOAD(&monitor, PMIX_SEND_HEARTBEAT, NULL, PMIX_POINTER);
    rc = PMIx_Process_monitor(&monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    PMIX_INFO_DESTRUCT(&monitor);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "Observer %s:%d: HEARTBEAT TEST FAILED - %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, "Observer %s:%d: HEARTBEAT TEST PASSED\n", myproc.nspace, myproc.rank);
    return 0;
}

/* rank 1: watch for the alerts the server raises about rank 0 */
static int run_observer(void)
{
    pmix_status_t code = MONITOR_TEST_CODE;
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

    code = PMIX_MONITOR_FILE_ALERT;
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, file_alert_handler, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "Observer %s:%d: file alert handler registration failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(mylock.status));
        DEBUG_DESTRUCT_LOCK(&mylock);
        return 1;
    }
    DEBUG_DESTRUCT_LOCK(&mylock);

    code = MONITOR_SILENT_CODE;
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, silent_alert_handler, evhandler_reg_callbk,
                                (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "Observer %s:%d: silent alert handler registration failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(mylock.status));
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
    DEBUG_CONSTRUCT_LOCK(&filelock);

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
        if (0 == result) {
            result = run_file_monitored();
        }
        if (0 == result) {
            result = arm_silent_monitors();
        }
    } else if (OBSERVER_RANK == (int) myproc.rank) {
        /* the blocking heartbeat call must return before we go on to
         * wait for the alert - if it hangs, this whole test hangs and
         * the driver's time limit reports it */
        if (0 != send_one_heartbeat()) {
            result = 1;
        }
        if (0 == wait_for_alert(&alertlock, 30) && alert_source_ok) {
            fprintf(stderr, "Observer %s:%d: MONITOR TEST PASSED\n", myproc.nspace, myproc.rank);
        } else {
            fprintf(stderr, "Observer %s:%d: MONITOR TEST FAILED - no valid heartbeat alert\n",
                    myproc.nspace, myproc.rank);
            result = 1;
        }
        if (0 == wait_for_alert(&filelock, 30) && file_alert_source_ok) {
            fprintf(stderr, "Observer %s:%d: FILE MONITOR TEST PASSED\n", myproc.nspace,
                    myproc.rank);
        } else {
            fprintf(stderr, "Observer %s:%d: FILE MONITOR TEST FAILED - no valid file alert\n",
                    myproc.nspace, myproc.rank);
            result = 1;
        }
        /* Both alerts about rank 0 have now been waited out, which means
         * the monitors have been running long enough for our own two to
         * have tripped had they been going to. */
        if (silent_alert_seen) {
            fprintf(stderr,
                    "Observer %s:%d: SILENCE TEST FAILED - alerted on a monitor that should "
                    "not have fired\n",
                    myproc.nspace, myproc.rank);
            result = 1;
        } else {
            fprintf(stderr, "Observer %s:%d: SILENCE TEST PASSED\n", myproc.nspace, myproc.rank);
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
    if (MONITORED_RANK == (int) myproc.rank && '\0' != watchfile[0]) {
        unlink(watchfile);
    }
    if (MONITORED_RANK == (int) myproc.rank && '\0' != obs_watchfile[0]) {
        unlink(obs_watchfile);
    }
    DEBUG_DESTRUCT_LOCK(&alertlock);
    DEBUG_DESTRUCT_LOCK(&filelock);
    if (PMIX_SUCCESS != PMIx_Finalize(NULL, 0)) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Finalize failed\n", myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return result;
}
