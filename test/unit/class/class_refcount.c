/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Concurrency tests for the pmix_object_t reference count.
 *
 * The count is the one piece of src/class that is documented as safe to
 * touch from any thread: everything else in the directory (list links,
 * hash-table slots, pointer-array indices, hotel rooms) assumes the caller
 * serializes, which in PMIx means the progress thread.  So the count is
 * what is worth hammering, and it is what changed when the per-object
 * pthread_mutex_t was replaced by a C11 atomic.
 *
 * The three properties below are what pmix_obj_update() owes its callers
 * whatever it is built on, so this test is written against them rather
 * than against the mechanism.
 *
 *   1. Balanced retain/release from N threads leaves the count exactly
 *      where it started.  An unserialized read-modify-write loses
 *      updates here and the count drifts low.
 *   2. The thread that drives the count to zero is the only one that sees
 *      zero, so exactly one destructor runs -- this is what keeps
 *      PMIX_RELEASE from double-freeing.
 *   3. Whatever a thread wrote into the object before its final release
 *      is visible to the destructor.  Under a mechanism with no
 *      release/acquire ordering the destructor may read stale fields.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"

#define NTHREADS    8
#define NITERATIONS 20000

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* ------------------------------------------------------------------ */
/* A class that counts its own destructions and carries a payload the   */
/* destructor can check for visibility.                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_object_t super;
    /* One slot per thread, so the threads never write the same word and
     * the visibility check below is not itself a data race. */
    int payload[NTHREADS];
} tracked_t;

static int destruct_count = 0;
static int destruct_saw_payload = -1;
static int destruct_expect_payload = 0;

static void tracked_construct(tracked_t *t)
{
    memset(t->payload, 0, sizeof(t->payload));
}

static void tracked_destruct(tracked_t *t)
{
    int i;

    destruct_count++;
    /* Record whether the marker each releasing thread wrote into its own
     * slot is visible here.  That is what the acq_rel ordering on the
     * count buys: the thread that observes zero has acquired everything
     * the other threads released. */
    if (0 < destruct_expect_payload) {
        destruct_saw_payload = 1;
        for (i = 0; i < destruct_expect_payload; i++) {
            if (0xfeed != t->payload[i]) {
                destruct_saw_payload = 0;
            }
        }
    }
}

PMIX_CLASS_INSTANCE(tracked_t, pmix_object_t, tracked_construct, tracked_destruct);

/* ------------------------------------------------------------------ */
/* 1. Balanced retain/release preserves the count                       */
/* ------------------------------------------------------------------ */

static tracked_t *shared_obj;

static void *hammer(void *arg)
{
    long i;

    PMIX_HIDE_UNUSED_PARAMS(arg);

    for (i = 0; i < NITERATIONS; i++) {
        PMIX_RETAIN(shared_obj);
        /* Deliberately not PMIX_RELEASE: that would free the object out
         * from under the other threads the instant the count hit zero.
         * pmix_obj_update() is the same read-modify-write PMIX_RELEASE
         * performs, minus the teardown. */
        (void) pmix_obj_update((pmix_object_t *) shared_obj, -1);
    }
    return NULL;
}

static void test_balanced_retain_release(void)
{
    pthread_t threads[NTHREADS];
    int i, rc, started = 0;

    shared_obj = PMIX_NEW(tracked_t);
    report("concurrency: object allocated", NULL != shared_obj);
    if (NULL == shared_obj) {
        return;
    }

    for (i = 0; i < NTHREADS; i++) {
        rc = pthread_create(&threads[i], NULL, hammer, NULL);
        if (0 != rc) {
            break;
        }
        started++;
    }
    report("concurrency: threads started", NTHREADS == started);

    for (i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }

    /* NTHREADS * NITERATIONS balanced pairs.  A lost update shows up as a
     * count below 1; a duplicated one as a count above it. */
    report("concurrency: refcount is exactly 1 after balanced traffic",
           1 == ((pmix_object_t *) shared_obj)->obj_reference_count);

    destruct_count = 0;
    PMIX_RELEASE(shared_obj);
    report("concurrency: final release ran the destructor once", 1 == destruct_count);
    report("concurrency: final release nulled the handle", NULL == shared_obj);
}

/* ------------------------------------------------------------------ */
/* 2. Exactly one thread observes zero                                  */
/* ------------------------------------------------------------------ */

/* Every thread takes one reference up front and then drops it; whichever
 * thread drops the last one is the one that must free.  If the count were
 * not atomic, two threads could both read 1, both decrement to 0, and both
 * run the destructor. */

static tracked_t *race_obj;

/* macOS has no pthread_barrier_t; a condvar barrier is a few lines and
 * keeps this test portable. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count;
    int target;
} simple_barrier_t;

static simple_barrier_t race_barrier;

/* What each racing thread needs: which object, and which payload slot is
 * its own to write. */
typedef struct {
    tracked_t *obj;
    int slot;
} dropper_arg_t;

static void barrier_init(simple_barrier_t *b, int target)
{
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->count = 0;
    b->target = target;
}

static void barrier_wait(simple_barrier_t *b)
{
    pthread_mutex_lock(&b->lock);
    b->count++;
    if (b->count >= b->target) {
        pthread_cond_broadcast(&b->cond);
    } else {
        while (b->count < b->target) {
            pthread_cond_wait(&b->cond, &b->lock);
        }
    }
    pthread_mutex_unlock(&b->lock);
}

static void barrier_destroy(simple_barrier_t *b)
{
    pthread_mutex_destroy(&b->lock);
    pthread_cond_destroy(&b->cond);
}

static void *dropper(void *arg)
{
    dropper_arg_t *da = (dropper_arg_t *) arg;
    tracked_t *obj = da->obj;

    /* Publish a marker in this thread's own slot.  The destructor will
     * look for all of them; seeing them proves the releasing threads'
     * writes were acquired by whoever ran the teardown. */
    obj->payload[da->slot] = 0xfeed;

    /* Line every thread up so the releases land as close together as the
     * scheduler will allow. */
    barrier_wait(&race_barrier);

    PMIX_RELEASE(obj);
    return NULL;
}

static void test_single_destruct_on_race(void)
{
    pthread_t threads[NTHREADS];
    dropper_arg_t args[NTHREADS];
    int i, started = 0;

    race_obj = PMIX_NEW(tracked_t);
    if (NULL == race_obj) {
        report("race: object allocated", false);
        return;
    }

    /* One reference per thread: the object starts at 1, so take
     * NTHREADS - 1 more. */
    for (i = 1; i < NTHREADS; i++) {
        PMIX_RETAIN(race_obj);
    }
    report("race: refcount primed to NTHREADS",
           NTHREADS == ((pmix_object_t *) race_obj)->obj_reference_count);

    destruct_count = 0;
    destruct_saw_payload = -1;
    destruct_expect_payload = NTHREADS;
    barrier_init(&race_barrier, NTHREADS);

    for (i = 0; i < NTHREADS; i++) {
        args[i].obj = race_obj;
        args[i].slot = i;
        if (0 != pthread_create(&threads[i], NULL, dropper, &args[i])) {
            break;
        }
        started++;
    }
    report("race: threads started", NTHREADS == started);

    /* If a thread failed to start, release its reference here and let the
     * barrier through so the ones that did start are not stranded. */
    for (i = started; i < NTHREADS; i++) {
        race_obj->payload[i] = 0xfeed;
        barrier_wait(&race_barrier);
    }
    for (i = started; i < NTHREADS; i++) {
        (void) pmix_obj_update((pmix_object_t *) race_obj, -1);
    }

    for (i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }
    barrier_destroy(&race_barrier);

    report("race: destructor ran exactly once", 1 == destruct_count);
    report("race: destructor saw every thread's payload", 1 == destruct_saw_payload);
    destruct_expect_payload = 0;
}

/* ------------------------------------------------------------------ */
/* 3. Independent objects do not interfere                              */
/* ------------------------------------------------------------------ */

#define NOBJECTS 64

static tracked_t *objects[NOBJECTS];

static void *churn(void *arg)
{
    long seed = (long) (intptr_t) arg;
    long i;
    int idx;

    for (i = 0; i < NITERATIONS; i++) {
        idx = (int) ((seed + i) % NOBJECTS);
        PMIX_RETAIN(objects[idx]);
        (void) pmix_obj_update((pmix_object_t *) objects[idx], -1);
    }
    return NULL;
}

static void test_many_objects(void)
{
    pthread_t threads[NTHREADS];
    int i, started = 0;
    bool ok = true;

    for (i = 0; i < NOBJECTS; i++) {
        objects[i] = PMIX_NEW(tracked_t);
        if (NULL == objects[i]) {
            ok = false;
        }
    }
    report("many objects: all allocated", ok);
    if (!ok) {
        return;
    }

    for (i = 0; i < NTHREADS; i++) {
        if (0 != pthread_create(&threads[i], NULL, churn, (void *) (intptr_t) (i * 7 + 1))) {
            break;
        }
        started++;
    }
    report("many objects: threads started", NTHREADS == started);

    for (i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }

    ok = true;
    for (i = 0; i < NOBJECTS; i++) {
        if (1 != ((pmix_object_t *) objects[i])->obj_reference_count) {
            ok = false;
        }
    }
    report("many objects: every refcount back to 1", ok);

    destruct_count = 0;
    for (i = 0; i < NOBJECTS; i++) {
        PMIX_RELEASE(objects[i]);
    }
    report("many objects: one destructor per object", NOBJECTS == destruct_count);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 4. Concurrent lazy class initialization (the double-checked lock)    */
/* ------------------------------------------------------------------ */

/* pmix_class_initialize() is reached through a double-checked lock: every
 * PMIX_NEW/PMIX_CONSTRUCT reads cls->cls_initialized *without* the class
 * mutex, and only takes it when the class looks uninitialized. The flag
 * write used to be a plain int store, so a thread that observed the new
 * epoch had nothing ordering it against the cls_construct_array the
 * initializing thread had just built -- the textbook broken DCL. It is now
 * a release store paired with the acquire in PMIX_CLASS_IS_INITIALIZED().
 *
 * A caveat worth stating plainly: this case cannot reliably *fail* on
 * x86-64 or arm64 even with the ordering removed, because both give the
 * plain load enough ordering in practice. Its job is to put concurrent
 * first-use of a class under a race detector at all -- nothing else in the
 * suite does, so before this there was no thread ever exercising
 * pmix_class_initialize() concurrently. Run it under
 * "-fsanitize=thread" to get the real signal. */

#define NRACE_CLASSES 8

typedef struct {
    pmix_object_t super;
    int stamp;
} lazy_t;

static void lazy_construct(lazy_t *l)
{
    l->stamp = 0x5A5A;
}

PMIX_CLASS_INSTANCE(lazy_t, pmix_object_t, lazy_construct, NULL);

/* Deeper hierarchies mean longer constructor arrays, so more of the array
 * build is exposed to a reader that jumped the gun. */
typedef struct {
    lazy_t super;
    int stamp2;
} lazy2_t;

static void lazy2_construct(lazy2_t *l)
{
    l->stamp2 = 0xA5A5;
}

PMIX_CLASS_INSTANCE(lazy2_t, lazy_t, lazy2_construct, NULL);

static simple_barrier_t init_barrier;
static int init_ok[NTHREADS];

static void *first_use(void *arg)
{
    int slot = (int) (intptr_t) arg;
    int ok = 1;
    int i;

    /* Every thread arrives at the very first use of these classes at the
     * same moment, so they all see cls_initialized as stale and race into
     * pmix_class_initialize(). */
    barrier_wait(&init_barrier);

    for (i = 0; i < NRACE_CLASSES; i++) {
        lazy2_t *o = PMIX_NEW(lazy2_t);
        if (NULL == o) {
            ok = 0;
            break;
        }
        /* Both constructors must have run, in parent-first order. If a
         * thread called through a half-built constructor array, one of
         * these stamps is missing. */
        if (0x5A5A != o->super.stamp || 0xA5A5 != o->stamp2) {
            ok = 0;
        }
        if (((pmix_object_t *) o)->obj_class->cls_depth < 3) {
            ok = 0;
        }
        PMIX_RELEASE(o);
    }
    init_ok[slot] = ok;
    return NULL;
}

static void test_concurrent_class_initialization(void)
{
    pthread_t threads[NTHREADS];
    int i, started = 0;
    bool all = true;

    for (i = 0; i < NTHREADS; i++) {
        init_ok[i] = 0;
    }
    barrier_init(&init_barrier, NTHREADS);

    for (i = 0; i < NTHREADS; i++) {
        if (0 != pthread_create(&threads[i], NULL, first_use, (void *) (intptr_t) i)) {
            break;
        }
        started++;
    }
    report("class init race: threads started", NTHREADS == started);

    /* Let the barrier through if some thread never started. */
    for (i = started; i < NTHREADS; i++) {
        barrier_wait(&init_barrier);
    }
    for (i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
    }
    barrier_destroy(&init_barrier);

    for (i = 0; i < started; i++) {
        if (!init_ok[i]) {
            all = false;
        }
    }
    report("class init race: every thread got fully constructed objects", all);

    /* And the class really is initialized once, for this epoch. */
    report("class init race: class reports initialized",
           PMIX_CLASS_IS_INITIALIZED(PMIX_CLASS(lazy2_t)));
    report("class init race: hierarchy depth is 3 (lazy2 -> lazy -> object)",
           3 == PMIX_CLASS(lazy2_t)->cls_depth);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_object_t reference-count concurrency tests ===\n");
    fprintf(stdout, "    (%d threads, %d iterations each)\n\n", NTHREADS, NITERATIONS);

    test_balanced_retain_release();
    test_single_destruct_on_race();
    test_many_objects();
    test_concurrent_class_initialization();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
