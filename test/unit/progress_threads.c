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
 *   - the progress_thread_cpus list is validated before it is turned
 *     into a CPU mask, so a typo is reported instead of silently
 *     becoming "bind to cpu 0" (Linux/BSD only - the whole affinity
 *     path is compiled out where pthread_setaffinity_np is absent).
 *
 * Not covered: which CPUs the thread actually ends up on. The bound
 * thread handle is internal, so a successful binding is not observable
 * through any interface a test can reach; only the accept/reject
 * decision about the list is.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"

#include <sys/wait.h>
#include <unistd.h>

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

/* ------------------------------------------------------------------
 * Validation of the progress_thread_cpus list
 *
 * The list is user input - an MCA parameter, or the
 * PMIX_BIND_PROGRESS_THREAD attribute - and it used to be handed
 * straight to strtoul + CPU_SET. strtoul reports 0 for a token with no
 * digits in it, so "cpu0" or a stray space quietly became "bind to cpu
 * 0"; a number past the end of the mask was dropped on the floor by
 * glibc and written past the end of it by BSD; and an inverted or
 * enormous range span turned into a very long loop.
 *
 * The accept/reject decision is observable: with binding declared
 * *required*, a list from which nothing usable could be extracted makes
 * the start fail rather than leaving the thread silently mis-bound.
 *
 * The whole affinity block is inside #ifdef HAVE_PTHREAD_SETAFFINITY_NP,
 * so on platforms without it (macOS) there is nothing here to test and
 * the list is ignored entirely.
 * ------------------------------------------------------------------ */

#ifdef HAVE_PTHREAD_SETAFFINITY_NP

/* run one start with the given cpu list, binding required, and report
 * whether it was accepted */
static bool try_cpu_list(const char *nm, const char *list)
{
    char *saved_cpus = pmix_progress_thread_cpus;
    bool saved_reqd = pmix_bind_progress_thread_reqd;
    pmix_status_t rc;

    pmix_progress_thread_cpus = (NULL == list) ? NULL : strdup(list);
    pmix_bind_progress_thread_reqd = true;

    if (NULL == pmix_progress_thread_init(nm)) {
        rc = PMIX_ERROR;
    } else {
        rc = pmix_progress_thread_start(nm);
        /* a failed start has already dropped the tracker; a successful
         * one is ours to tear down */
        if (PMIX_SUCCESS == rc) {
            (void) pmix_progress_thread_stop(nm);
        }
    }

    if (NULL != pmix_progress_thread_cpus) {
        free(pmix_progress_thread_cpus);
    }
    pmix_progress_thread_cpus = saved_cpus;
    pmix_bind_progress_thread_reqd = saved_reqd;

    return (PMIX_SUCCESS == rc);
}

static void test_cpu_list_validation(void)
{
    /* cpu 0 exists everywhere this test can run */
    report("cpulist:a single valid cpu is accepted",
           try_cpu_list("UT-cpu-ok", "0"));

    /* a token with no digits must not be read as cpu 0 */
    report("cpulist:a non-numeric entry is rejected",
           !try_cpu_list("UT-cpu-alpha", "cpu0"));

    /* trailing garbage after a good number is still garbage */
    report("cpulist:trailing garbage is rejected",
           !try_cpu_list("UT-cpu-trail", "0x"));

    /* beyond the end of the mask */
    report("cpulist:an out-of-range cpu is rejected",
           !try_cpu_list("UT-cpu-huge", "99999999"));

    /* a range that runs backwards names no cpus */
    report("cpulist:an inverted range is rejected",
           !try_cpu_list("UT-cpu-inverted", "8-2"));

    /* a negative cpu is not a cpu */
    report("cpulist:a negative cpu is rejected",
           !try_cpu_list("UT-cpu-negative", "-1"));

    /* an empty entry between two commas */
    report("cpulist:an empty entry alone is rejected",
           !try_cpu_list("UT-cpu-empty", ","));

    /* one bad entry does not discard the good ones: cpu 0 still binds */
    report("cpulist:a good entry survives a bad neighbor",
           try_cpu_list("UT-cpu-mixed", "nope,0"));

    /* an unset list means "do not bind", not "bind to nothing" */
    report("cpulist:no list at all is fine",
           try_cpu_list("UT-cpu-none", NULL));
}

#else

static void test_cpu_list_validation(void)
{
    fprintf(stdout, "  SKIP: cpulist (no pthread_setaffinity_np on this platform)\n");
}

#endif /* HAVE_PTHREAD_SETAFFINITY_NP */

/* ------------------------------------------------------------------
 * Blocking calls made FROM the progress thread
 *
 * A blocking PMIx API hands its work to the progress thread and waits.
 * Called from that thread it would post the work to the very event loop
 * it is standing in, and wait for a loop that cannot run until it
 * returns - the thread waits on itself, forever.  The way a host reaches
 * this is ordinary: deleting a namespace from inside its own
 * client_finalized upcall, for instance, or from an event handler.  Both
 * are dispatched from the progress thread.
 *
 * The guard turns that hang into an answer.  These tests run inside an
 * event handler - which is dispatched from the progress thread, so it
 * stands in for any upcall - and would hang the whole suite if the guard
 * regressed, which is exactly the signal wanted.
 * ------------------------------------------------------------------ */

static pmix_lock_t reentry_lock;
static bool reentry_saw_progress_thread = false;
static pmix_status_t reentry_store_rc = PMIX_SUCCESS;
static pmix_status_t reentry_pset_rc = PMIX_SUCCESS;

static void reentry_handler(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    pmix_value_t val;
    pmix_nspace_t ns;

    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results,
                            nresults);

    /* we are on the progress thread - the whole point of the exercise */
    reentry_saw_progress_thread = pmix_progress_thread_is_current();

    /* an API that reports a status must refuse rather than hang */
    PMIX_VALUE_LOAD(&val, "value", PMIX_STRING);
    reentry_store_rc = PMIx_Store_internal(&pmix_globals.myid, "ut.reentry.key", &val);
    PMIX_VALUE_DESTRUCT(&val);

    /* ... including the one whose caddy lives on the caller's stack */
    reentry_pset_rc = PMIx_server_delete_process_set("ut-no-such-pset");

    /* an API that reports no status completes asynchronously instead.
     * There is nothing to check here beyond the fact that it returns at
     * all - which it did not before the guard existed */
    PMIX_LOAD_NSPACE(ns, "ut-no-such-nspace");
    PMIx_server_deregister_nspace(ns, NULL, NULL);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
    PMIX_WAKEUP_THREAD(&reentry_lock);
}

static void reentry_registered(pmix_status_t status, size_t refid, void *cbdata)
{
    pmix_lock_t *lk = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(refid);
    lk->status = status;
    PMIX_WAKEUP_THREAD(lk);
}

static void test_blocking_call_from_progress_thread(void)
{
    pmix_status_t code = PMIX_ERR_DEBUGGER_RELEASE;
    pmix_lock_t reglock;

    /* off the progress thread, the predicate must say so */
    report("reentry:main thread is not the progress thread",
           !pmix_progress_thread_is_current());

    PMIX_CONSTRUCT_LOCK(&reglock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, reentry_handler, reentry_registered,
                                &reglock);
    PMIX_WAIT_THREAD(&reglock);
    if (PMIX_SUCCESS != reglock.status) {
        report("reentry:handler registered", false);
        PMIX_DESTRUCT_LOCK(&reglock);
        return;
    }
    PMIX_DESTRUCT_LOCK(&reglock);

    PMIX_CONSTRUCT_LOCK(&reentry_lock);
    PMIx_Notify_event(code, &pmix_globals.myid, PMIX_RANGE_LOCAL, NULL, 0, NULL, NULL);
    /* if the guard regresses, the handler never returns and this never
     * wakes - the suite hangs, which is the intended alarm */
    PMIX_WAIT_THREAD(&reentry_lock);
    PMIX_DESTRUCT_LOCK(&reentry_lock);

    report("reentry:handler ran on the progress thread", reentry_saw_progress_thread);
    report("reentry:PMIx_Store_internal reports WOULD_BLOCK",
           PMIX_ERR_WOULD_BLOCK == reentry_store_rc);
    report("reentry:PMIx_server_delete_process_set reports WOULD_BLOCK",
           PMIX_ERR_WOULD_BLOCK == reentry_pset_rc);

    /* and the same calls still work normally from this thread */
    report("reentry:delete_process_set works off the progress thread",
           PMIX_ERR_WOULD_BLOCK != PMIx_server_delete_process_set("ut-no-such-pset"));
}

/* ------------------------------------------------------------------
 * The process-set entry points must screen what the host hands them
 *
 * Both are public APIs, so the host can pass anything, and every one of
 * these arguments used to be carried straight onto the progress thread
 * and dereferenced there: a NULL name reaches strdup and strcmp, a NULL
 * member array reaches memcpy, and a zero member count makes
 * PMIx_Data_array_create answer NULL - which was then read for its
 * "array" member. Each case below segfaults an unfixed library rather
 * than failing, the same bargain test/unit/iof_output.c makes.
 */
static void test_pset_params(void)
{
    pmix_proc_t members[2];
    pmix_status_t rc;
    size_t nsets;

    PMIX_LOAD_PROCID(&members[0], "pset.ut.ns", 0);
    PMIX_LOAD_PROCID(&members[1], "pset.ut.ns", 1);

    rc = PMIx_server_define_process_set(members, 2, NULL);
    report("pset:define rejects a NULL name", PMIX_ERR_BAD_PARAM == rc);

    rc = PMIx_server_define_process_set(NULL, 2, "ut-pset");
    report("pset:define rejects a NULL member array", PMIX_ERR_BAD_PARAM == rc);

    rc = PMIx_server_define_process_set(members, 0, "ut-pset");
    report("pset:define rejects a zero member count", PMIX_ERR_BAD_PARAM == rc);

    rc = PMIx_server_delete_process_set(NULL);
    report("pset:delete rejects a NULL name", PMIX_ERR_BAD_PARAM == rc);

    /* a well-formed pair still has to work, or the screens above would
     * be indistinguishable from a function that refuses everything */
    rc = PMIx_server_define_process_set(members, 2, "ut-pset");
    report("pset:a well-formed set is defined", PMIX_SUCCESS == rc);

    /* Defining a name that is already recorded is a redefinition. No API
     * lets a host change a set's membership, so this is the only way to
     * do it - and the definition used to be appended, leaving the old
     * entry in front of the new one. Every member of the old set went on
     * reporting itself a member through PMIX_PSET_NAMES, the key named
     * the set twice for anyone in both, and the delete below removed
     * only the stale entry, so the set outlived its own deletion. */
    nsets = pmix_list_get_size(&pmix_server_globals.psets);
    rc = PMIx_server_define_process_set(members, 1, "ut-pset");
    report("pset:a redefinition is accepted", PMIX_SUCCESS == rc);
    report("pset:a redefinition replaces rather than shadows",
           nsets == pmix_list_get_size(&pmix_server_globals.psets));

    rc = PMIx_server_delete_process_set("ut-pset");
    report("pset:and deleted again", PMIX_SUCCESS == rc);
    report("pset:the deletion left nothing behind",
           (nsets - 1) == pmix_list_get_size(&pmix_server_globals.psets));
}

/* ------------------------------------------------------------------
 * The process-set entry points must screen the SERVER library's flag
 *
 * Their documented PMIX_ERR_INIT means "the PMIx server library has not
 * been initialized", and pmix_globals.initialized does not answer that -
 * PMIx_Init and PMIx_tool_init set it too. A CLIENT is the case that
 * bites: PMIx_Init constructs only the two IOF lists in
 * pmix_server_globals, so psets is still PMIX_LIST_STATIC_INIT, whose
 * sentinel carries NULL next and prev pointers. The define path writes
 * through the NULL prev in pmix_list_append and the delete path walks
 * off the NULL next; both are a SIGSEGV on the progress thread, taken
 * after the entry point has already answered PMIX_SUCCESS. So these
 * cases run in a forked child - against an unfixed library the parent
 * would die rather than report. (A tool escapes the crash, since
 * PMIx_tool_init calls pmix_server_initialize, but is owed the same
 * refusal.)
 * ------------------------------------------------------------------ */

static int pset_client_child(int which)
{
    pmix_proc_t myproc, member;
    pmix_status_t rc;

    /* a singleton reports PMIX_ERR_UNREACH from init - it is fully
     * initialized, it just has no server */
    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        return 3;
    }

    if (0 == which) {
        PMIX_LOAD_PROCID(&member, "pset.ut.ns", 0);
        rc = PMIx_server_define_process_set(&member, 1, "ut-client-pset");
    } else {
        rc = PMIx_server_delete_process_set("ut-client-pset");
    }
    PMIx_Finalize(NULL, 0);
    return (PMIX_ERR_INIT == rc) ? 0 : 1;
}

static void check_pset_client_refusal(const char *name, int which)
{
    pid_t child;
    int status = 0;

    fflush(stdout);
    child = fork();
    if (0 == child) {
        _exit(pset_client_child(which));
    }
    if (0 > child) {
        report(name, 0);
        return;
    }
    if (0 > waitpid(child, &status, 0)) {
        report(name, 0);
        return;
    }
    /* a child that died on a signal is the unfixed library crashing on
     * the progress thread, which is exactly what the screen prevents */
    report(name, WIFEXITED(status) && 0 == WEXITSTATUS(status));
}

/* ------------------------------------------------------------------
 * The void deregistration APIs must report a dropped request
 *
 * PMIx_server_deregister_nspace and PMIx_server_deregister_client are
 * the only two public APIs that take a completion callback and report no
 * status.  The callback is therefore the ONLY way they can tell a host
 * that its request will not be serviced - and a host that is given one
 * and never hears back waits forever.
 *
 * Their !initialized guard has always invoked the callback.  The
 * progress_thread_stopped guard immediately below it used to return
 * silently, which is the case a host hits while shutting down.
 * ------------------------------------------------------------------ */

static pmix_status_t dropped_status = PMIX_SUCCESS;
static int dropped_count = 0;

static void dropped_cb(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbdata);
    dropped_status = status;
    dropped_count++;
}

static void test_void_deregisters_report_a_dropped_request(void)
{
    pmix_nspace_t ns;
    pmix_proc_t proc;

    /* stop the shared progress thread, which is what makes the library
     * refuse to accept new work - the same state a finalizing host is in */
    PMIx_Progress_thread_stop(NULL, 0);

    PMIX_LOAD_NSPACE(ns, "ut-dropped-nspace");
    dropped_count = 0;
    dropped_status = PMIX_SUCCESS;
    PMIx_server_deregister_nspace(ns, dropped_cb, NULL);
    report("dropped:deregister_nspace invoked the callback", 1 == dropped_count);
    report("dropped:deregister_nspace reported NOT_AVAILABLE",
           PMIX_ERR_NOT_AVAILABLE == dropped_status);

    PMIX_LOAD_PROCID(&proc, "ut-dropped-nspace", 0);
    dropped_count = 0;
    dropped_status = PMIX_SUCCESS;
    PMIx_server_deregister_client(&proc, dropped_cb, NULL);
    report("dropped:deregister_client invoked the callback", 1 == dropped_count);
    report("dropped:deregister_client reported NOT_AVAILABLE",
           PMIX_ERR_NOT_AVAILABLE == dropped_status);

    /* a NULL callback on the same path must simply return */
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    PMIx_server_deregister_client(&proc, NULL, NULL);
    report("dropped:a NULL callback is tolerated", true);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* these fork, so they run before this process becomes a server */
    check_pset_client_refusal("pset:define from a client is refused, not fatal", 0);
    check_pset_client_refusal("pset:delete from a client is refused, not fatal", 1);

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
    test_cpu_list_validation();
    test_blocking_call_from_progress_thread();
    test_pset_params();
    /* must come last - it stops the shared progress thread */
    test_void_deregisters_report_a_dropped_request();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
