/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the name-addressed progress-thread engine in
 * src/runtime/pmix_progress_threads.c.
 *
 * The process is initialized as a PMIx server, which brings up libevent
 * thread support and the shared "PMIX-wide" progress thread.  All of the
 * tests here operate on *named* progress threads so they never disturb
 * the library's shared thread; each test balances its own init/stop
 * calls so nothing is left running.
 *
 * What is covered:
 *   - init returns a usable base and is reference-counted per name
 *     (a second init of the same name returns the same base);
 *   - distinct names get distinct bases;
 *   - a started engine actually progresses events posted to its base;
 *   - resume on an active engine reports PMIX_ERR_RESOURCE_BUSY;
 *   - pause followed by resume restores progress;
 *   - stop is reference counted and removes the tracker at zero, after
 *     which start/stop of that name report PMIX_ERR_NOT_FOUND;
 *   - operations on an unknown name report PMIX_ERR_NOT_FOUND.
 *
 * Not covered: CPU-affinity binding (start_progress_engine's
 * pthread_setaffinity_np path) is not observable through any public
 * interface - the bound thread handle is internal - so it cannot be
 * asserted from a unit test.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/threads/pmix_threads.h"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* ------------------------------------------------------------------ */
/* Helper: prove a base is being progressed by its engine thread.      */
/*                                                                     */
/* We post a manually-activated event onto the base; if the engine is  */
/* looping it will fire our callback, which wakes the lock we block    */
/* on.  This mirrors the marker-event pattern the library itself uses  */
/* in PMIx_Progress_thread_stop.  It only returns once the event has   */
/* run, so reaching the end means the base is live.                    */
/* ------------------------------------------------------------------ */

static void wake_cb(int fd, short args, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(fd, args);
    PMIX_WAKEUP_THREAD(lock);
}

static void drive_one_event(pmix_event_base_t *base)
{
    pmix_lock_t lock;
    pmix_event_t ev;

    PMIX_CONSTRUCT_LOCK(&lock);
    pmix_event_assign(&ev, base, -1, EV_WRITE, wake_cb, &lock);
    PMIX_POST_OBJECT(&lock);
    pmix_event_active(&ev, EV_WRITE, 1);
    PMIX_WAIT_THREAD(&lock);
    PMIX_DESTRUCT_LOCK(&lock);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_refcount_identity(void)
{
    const char *nm = "UT-refcount";
    pmix_event_base_t *b1, *b2;
    pmix_status_t rc;

    b1 = pmix_progress_thread_init(nm);
    report("refcount:init-returns-base", NULL != b1);

    /* second init of the same name returns the same base and ups the
     * refcount rather than creating a new thread */
    b2 = pmix_progress_thread_init(nm);
    report("refcount:reinit-same-base", NULL != b2 && b2 == b1);

    /* refcount is 2; the first stop must leave it alive */
    rc = pmix_progress_thread_stop(nm);
    report("refcount:first-stop-ok", PMIX_SUCCESS == rc);
    rc = pmix_progress_thread_start(nm);
    report("refcount:still-present-after-first-stop", PMIX_SUCCESS == rc);

    /* engine got started above; a second stop drops the refcount to 0,
     * stops the engine, and removes the tracker */
    rc = pmix_progress_thread_stop(nm);
    report("refcount:second-stop-ok", PMIX_SUCCESS == rc);

    /* now it is gone: both start and stop should not find it */
    rc = pmix_progress_thread_start(nm);
    report("refcount:start-after-removal-notfound", PMIX_ERR_NOT_FOUND == rc);
    rc = pmix_progress_thread_stop(nm);
    report("refcount:stop-after-removal-notfound", PMIX_ERR_NOT_FOUND == rc);
}

static void test_distinct_names(void)
{
    pmix_event_base_t *ba, *bb;

    ba = pmix_progress_thread_init("UT-A");
    bb = pmix_progress_thread_init("UT-B");
    report("distinct:two-names-two-bases",
           NULL != ba && NULL != bb && ba != bb);

    (void) pmix_progress_thread_stop("UT-A");
    (void) pmix_progress_thread_stop("UT-B");
}

static void test_start_progresses(void)
{
    const char *nm = "UT-run";
    pmix_event_base_t *base;
    pmix_status_t rc;

    base = pmix_progress_thread_init(nm);
    if (NULL == base) {
        report("start:init", 0);
        return;
    }

    rc = pmix_progress_thread_start(nm);
    report("start:start-ok", PMIX_SUCCESS == rc);

    if (PMIX_SUCCESS == rc) {
        /* if this returns, the engine thread progressed our event */
        drive_one_event(base);
        report("start:event-progressed", 1);

        /* resume on an already-running engine is refused */
        rc = pmix_progress_thread_resume(nm);
        report("start:resume-busy", PMIX_ERR_RESOURCE_BUSY == rc);
    }

    (void) pmix_progress_thread_stop(nm);
}

static void test_pause_resume(void)
{
    const char *nm = "UT-pause";
    pmix_event_base_t *base;
    pmix_status_t rc;

    base = pmix_progress_thread_init(nm);
    if (NULL == base) {
        report("pause:init", 0);
        return;
    }
    rc = pmix_progress_thread_start(nm);
    report("pause:start-ok", PMIX_SUCCESS == rc);

    rc = pmix_progress_thread_pause(nm);
    report("pause:pause-ok", PMIX_SUCCESS == rc);

    /* resume must restart the engine (start_progress_engine path) */
    rc = pmix_progress_thread_resume(nm);
    report("pause:resume-ok", PMIX_SUCCESS == rc);

    if (PMIX_SUCCESS == rc) {
        /* progress works again after the resume */
        drive_one_event(base);
        report("pause:progress-after-resume", 1);
    }

    (void) pmix_progress_thread_stop(nm);
}

static void test_unknown_name(void)
{
    const char *nm = "UT-does-not-exist";

    report("unknown:start", PMIX_ERR_NOT_FOUND == pmix_progress_thread_start(nm));
    report("unknown:stop", PMIX_ERR_NOT_FOUND == pmix_progress_thread_stop(nm));
    report("unknown:pause", PMIX_ERR_NOT_FOUND == pmix_progress_thread_pause(nm));
    report("unknown:resume", PMIX_ERR_NOT_FOUND == pmix_progress_thread_resume(nm));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== progress-thread engine unit tests ===\n\n");

    test_refcount_identity();
    test_distinct_names();
    test_start_progresses();
    test_pause_resume();
    test_unknown_name();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
