/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the threading primitives in src/threads - the
 * pmix_thread_t lifecycle, the thread-specific-data key registry, and
 * the pmix_lock_t handshake.
 *
 * These primitives are used from ~300 sites across the tree but had no
 * test of their own, because their only in-tree caller (the progress
 * engine in src/runtime/pmix_progress_threads.c) only ever drives the
 * success path. Everything checked here is on an error or edge path that
 * a normal run never reaches:
 *
 * - pmix_thread_join() on a thread that was never started, and on one
 *   that has already been joined. Both arrive carrying the (pthread_t)-1
 *   sentinel that the constructor and join itself install. Passing that
 *   to pthread_join is undefined behavior; on glibc it dereferences the
 *   sentinel and the process dies. A caller does reach this: the resume
 *   path marks its tracker active before creating the thread, so a
 *   failed create leaves a tracker that later gets asked to join.
 *
 * - pmix_tsd_keys_destruct() standing in for the thread-exit destructor
 *   run that never happens on the main thread. pthread only ever invokes
 *   a destructor with a non-NULL value, so a destructor written to that
 *   contract may dereference its argument without checking - and this
 *   was the one call site that could hand it NULL, for any key the
 *   finalizing thread never used. The "destructor must NOT fire" case
 *   below is the whole point of that test; it passes vacuously if the
 *   destructor is made NULL-tolerant, so it deliberately reports a
 *   failure from inside the destructor instead.
 *
 * No PMIx_Init here, and not even pmix_init_util: nothing under test
 * needs the MCA, an event base, or a server. The class system
 * initializes itself lazily on first PMIX_CONSTRUCT.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/threads/pmix_threads.h"
#include "src/threads/pmix_tsd.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* pmix_thread_t lifecycle                                            */
/* ------------------------------------------------------------------ */

/* written by the spawned thread, read after the join - which is the
 * synchronization, so no atomic is needed here */
static int body_ran = 0;

static void *thread_body(pmix_object_t *obj)
{
    pmix_thread_t *t = (pmix_thread_t *) obj;

    /* the engine reads its context out of t_arg, so check the object we
     * were handed is the one that was started */
    if (t->t_arg == (void *) &body_ran) {
        body_ran = 1;
    }
    return PMIX_THREAD_CANCELLED;
}

static void thread_lifecycle(void)
{
    pmix_thread_t t;
    void *ret = (void *) 0xdeadbeef;
    int rc;

    PMIX_CONSTRUCT(&t, pmix_thread_t);

    /* The constructor has to establish every field. PMIX_NEW and
     * PMIX_CONSTRUCT do zero the object first, which means these two
     * assertions would now hold even against a constructor that skipped
     * them - so read a pass here as "the field is NULL", which is what
     * the thread body needs, rather than as proof the constructor set
     * it. */
    report("construct leaves t_run NULL", NULL == t.t_run);
    report("construct leaves t_arg NULL", NULL == t.t_arg);

    /* joining a thread that was never started must be refused, not
     * handed to pthread_join */
    rc = pmix_thread_join(&t, &ret);
    report("join of unstarted thread is refused", PMIX_SUCCESS != rc);
    report("join of unstarted thread clears the return", NULL == ret);

#if PMIX_ENABLE_DEBUG
    /* the debug guard rejects a thread with no body rather than letting
     * pthread_create branch to NULL in the new thread */
    rc = pmix_thread_start(&t);
    report("start with no run function is refused", PMIX_SUCCESS != rc);
#endif

    /* now the real thing */
    t.t_run = thread_body;
    t.t_arg = (void *) &body_ran;
    rc = pmix_thread_start(&t);
    report("start succeeds", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&t);
        return;
    }

    ret = NULL;
    rc = pmix_thread_join(&t, &ret);
    report("join succeeds", PMIX_SUCCESS == rc);
    report("thread body saw its t_arg", 1 == body_ran);
    report("join returns the body's value", PMIX_THREAD_CANCELLED == ret);

    /* join resets the handle to the sentinel, so a second join is the
     * same undefined pthread_join call as the unstarted case */
    ret = (void *) 0xdeadbeef;
    rc = pmix_thread_join(&t, &ret);
    report("second join is refused", PMIX_SUCCESS != rc);
    report("second join clears the return", NULL == ret);

    PMIX_DESTRUCT(&t);
}

/* ------------------------------------------------------------------ */
/* the pmix_lock_t handshake                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_lock_t lock;
    pmix_status_t towake;
} waker_t;

/* a file-scope lock, to check the static initializer agrees with what
 * PMIX_CONSTRUCT_LOCK produces */
static pmix_lock_t static_lock = PMIX_LOCK_STATIC_INIT;

static void *waker_body(pmix_object_t *obj)
{
    pmix_thread_t *t = (pmix_thread_t *) obj;
    waker_t *w = (waker_t *) t->t_arg;

    w->lock.status = w->towake;
    PMIX_WAKEUP_THREAD(&w->lock);
    return NULL;
}

static void lock_handshake(void)
{
    pmix_thread_t t;
    waker_t w;
    int rc;

    /* the CONSTRUCT_LOCK / WAIT_THREAD / WAKEUP_THREAD round trip every
     * blocking PMIx API is built out of, with a real second thread doing
     * the waking */
    PMIX_CONSTRUCT_LOCK(&w.lock);
    w.towake = PMIX_ERR_TIMEOUT;

    /* A constructed lock's status is PMIX_ERR_INIT until a handler
     * assigns it. That default is deliberately an *error*: a waiter reads
     * status only after being woken, so the value standing there when
     * nobody assigned one is a handler that forgot, and reporting that as
     * PMIX_SUCCESS would turn the omission into a silent wrong answer.
     * The lock usually lives in a PMIX_NEW'd caddy, so before this was
     * initialized at all the alternative was heap garbage. */
    report("construct leaves status at PMIX_ERR_INIT", PMIX_ERR_INIT == w.lock.status);
    report("construct arms the lock", w.lock.active);
    report("the static initializer agrees", PMIX_ERR_INIT == static_lock.status);
    report("a statically initialized lock starts disarmed", !static_lock.active);

    PMIX_CONSTRUCT(&t, pmix_thread_t);
    t.t_run = waker_body;
    t.t_arg = &w;
    rc = pmix_thread_start(&t);
    report("waker thread started", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&t);
        PMIX_DESTRUCT_LOCK(&w.lock);
        return;
    }

    PMIX_WAIT_THREAD(&w.lock);
    report("wait returns once woken", !w.lock.active);
    report("waker's status crossed the handshake", PMIX_ERR_TIMEOUT == w.lock.status);
    report("the handshake overwrote the PMIX_ERR_INIT default",
           PMIX_ERR_INIT != w.lock.status);

    pmix_thread_join(&t, NULL);
    PMIX_DESTRUCT(&t);
    PMIX_DESTRUCT_LOCK(&w.lock);

    /* ACQUIRE/RELEASE is the other pairing: acquire re-arms the lock and
     * leaves the mutex held, release drops both. Two cycles, because a
     * release that failed to unlock would deadlock on the second */
    PMIX_CONSTRUCT_LOCK(&w.lock);
    w.lock.active = false;
    PMIX_ACQUIRE_THREAD(&w.lock);
    report("acquire re-arms the lock", w.lock.active);
    PMIX_RELEASE_THREAD(&w.lock);
    report("release disarms the lock", !w.lock.active);
    PMIX_ACQUIRE_THREAD(&w.lock);
    PMIX_RELEASE_THREAD(&w.lock);
    report("second acquire/release cycle completes", !w.lock.active);
    PMIX_DESTRUCT_LOCK(&w.lock);
}

/* ------------------------------------------------------------------ */
/* the TSD key registry                                               */
/* ------------------------------------------------------------------ */

static int dtor_calls = 0;
static void *dtor_value = NULL;
static int dtor_saw_null = 0;

static void counting_dtor(void *value)
{
    ++dtor_calls;
    dtor_value = value;
    if (NULL == value) {
        /* pthread never does this, so a destructor is entitled to
         * dereference. Record it rather than crash, so the test reports
         * the defect instead of taking the suite down with it */
        dtor_saw_null = 1;
    }
}

static void tsd_registry(void)
{
    pmix_tsd_key_t key;
    void *val;
    int rc;
    char payload[] = "per-thread value";

    /* Case A: this thread never sets a value for the key. The registry
     * walk must skip the destructor entirely */
    dtor_calls = 0;
    dtor_value = NULL;
    dtor_saw_null = 0;
    rc = pmix_tsd_key_create(&key, counting_dtor);
    report("key create succeeds", PMIX_SUCCESS == rc);

    rc = pmix_tsd_keys_destruct();
    report("keys_destruct succeeds with no value set", PMIX_SUCCESS == rc);
    report("destructor is not run for a key this thread never set", 0 == dtor_calls);
    report("destructor is never handed a NULL value", 0 == dtor_saw_null);

    /* Case B: a value is set, so the destructor must run exactly once
     * and receive it */
    dtor_calls = 0;
    dtor_value = NULL;
    dtor_saw_null = 0;
    rc = pmix_tsd_key_create(&key, counting_dtor);
    report("second key create succeeds", PMIX_SUCCESS == rc);
    rc = pmix_tsd_setspecific(key, payload);
    report("setspecific succeeds", 0 == rc);
    rc = pmix_tsd_getspecific(key, &val);
    report("getspecific returns what was set", PMIX_SUCCESS == rc && payload == val);

    rc = pmix_tsd_keys_destruct();
    report("keys_destruct succeeds with a value set", PMIX_SUCCESS == rc);
    report("destructor ran once", 1 == dtor_calls);
    report("destructor received the value", payload == dtor_value);
    report("destructor was not handed NULL", 0 == dtor_saw_null);

    /* the registry is emptied, so a second walk has nothing to do - if
     * it re-ran the destructor it would be operating on a key it already
     * deleted and a value it already freed */
    dtor_calls = 0;
    rc = pmix_tsd_keys_destruct();
    report("second keys_destruct is a no-op", PMIX_SUCCESS == rc && 0 == dtor_calls);

    /* and the slots come back: keys are deleted, not just forgotten, so
     * an init/finalize cycle does not walk the process toward
     * PTHREAD_KEYS_MAX */
    rc = pmix_tsd_key_create(&key, NULL);
    report("key create works again after destruct", PMIX_SUCCESS == rc);
    rc = pmix_tsd_keys_destruct();
    report("final keys_destruct succeeds", PMIX_SUCCESS == rc);
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    fprintf(stdout, "\nThread lifecycle:\n");
    thread_lifecycle();

    fprintf(stdout, "\nLock handshake:\n");
    lock_handshake();

    fprintf(stdout, "\nTSD key registry:\n");
    tsd_registry();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
