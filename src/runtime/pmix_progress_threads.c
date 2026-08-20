/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <pthread.h>
#ifdef HAVE_PTHREAD_NP_H
#    include <pthread_np.h>
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdatomic.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/runtime/pmix_rte.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_show_help.h"

/* create a tracking object for progress threads */
typedef struct {
    pmix_list_item_t super;

    int refcount;
    char *name;

    pmix_event_base_t *ev_base;

    /* This will be set to false when it is time for the progress
       thread to exit */
    pmix_atomic_bool_t ev_active;

    /* This event will always be set on the ev_base (so that the
       ev_base is not empty!) */
    pmix_event_t block;
    bool block_assigned;
    bool engine_constructed;
    pmix_thread_t engine;
} pmix_progress_tracker_t;

static void tracker_constructor(pmix_progress_tracker_t *p)
{
    p->refcount = 1; // start at one since someone created it
    p->name = NULL;
    p->ev_base = NULL;
    p->ev_active = false;
    p->block_assigned = false;
    p->engine_constructed = false;
}

static void tracker_destructor(pmix_progress_tracker_t *p)
{
    /* PMIX_NEW does not zero the object, so 'block' holds garbage until
     * pmix_event_assign has run. A tracker released before that point -
     * an allocation failure in pmix_progress_thread_init - must not hand
     * that garbage to libevent */
    if (p->block_assigned) {
        pmix_event_del(&p->block);
    }

    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->ev_base) {
        pmix_event_base_free(p->ev_base);
    }
    if (p->engine_constructed) {
        PMIX_DESTRUCT(&p->engine);
    }
}

static PMIX_CLASS_INSTANCE(pmix_progress_tracker_t,
                           pmix_list_item_t,
                           tracker_constructor,
                           tracker_destructor);

/* LOCAL VARIABLES */
static bool inited = false;
static pmix_list_t tracking;
static struct timeval long_timeout = {.tv_sec = 3600, .tv_usec = 0};
static const char *shared_thread_name = "PMIX-wide async progress thread";
static pmix_progress_tracker_t *shared_thread_tracker = NULL;

/* Identity of the thread currently running the shared progress loop.
 *
 * Only that thread ever writes this, which is what makes reading it
 * without a lock correct for the one question it is asked. A thread
 * comparing itself against it either wrote the value itself - and so
 * reads its own write, getting a true answer - or is some other thread,
 * which correctly fails to match. A stale value can therefore never
 * produce a false "yes", only a false "no", and a false "no" just leaves
 * the caller with the behavior it had before this check existed.
 *
 * The sentinel follows the convention already used for pmix_thread_t in
 * src/threads/thread.c.
 */
static pthread_t shared_loop_thread = (pthread_t) -1;

bool pmix_progress_thread_is_current(void)
{
    pthread_t owner = shared_loop_thread;

    if ((pthread_t) -1 == owner) {
        /* nobody is in the loop */
        return false;
    }
    return (0 != pthread_equal(owner, pthread_self()));
}

bool pmix_progress_thread_check_blocking(const char *api)
{
    if (!pmix_progress_thread_is_current()) {
        return false;
    }
    pmix_show_help("help-pmix-runtime.txt", "blocking-call-from-progress-thread",
                   true, (NULL == api) ? "a blocking PMIx API" : api);
    return true;
}

/*
 * If this event is fired, just restart it so that this event base
 * continues to have something to block on.
 */
static void dummy_timeout_cb(int sd, short args, void *cbdata)
{
    pmix_progress_tracker_t *trk = (pmix_progress_tracker_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_event_add(&trk->block, &long_timeout);
}

/*
 * Main for the progress thread
 */
static void *progress_engine(pmix_object_t *obj)
{
    pmix_thread_t *t = (pmix_thread_t *) obj;
    pmix_progress_tracker_t *trk = (pmix_progress_tracker_t *) t->t_arg;
    bool shared = (NULL != trk->name && 0 == strcmp(trk->name, shared_thread_name));

    /* record who we are, so a blocking PMIx API called from inside one of
     * the callbacks this loop dispatches can tell that it is about to wait
     * on itself. Only the shared loop is tracked - it is the one
     * PMIX_THREADSHIFT posts to */
    if (shared) {
        shared_loop_thread = pthread_self();
    }

    while (trk->ev_active) {
        pmix_event_loop(trk->ev_base, PMIX_EVLOOP_ONCE);
    }

    if (shared) {
        shared_loop_thread = (pthread_t) -1;
    }

    return PMIX_THREAD_CANCELLED;
}

void PMIx_Progress(void)
{
    pthread_t save;

    if (NULL == shared_thread_tracker) {
        return;
    }
    /* the host is driving progress itself, so for the duration of this
     * pass *we* are the progress thread - see progress_engine above.
     * Restore rather than clear: a host that calls this from inside a
     * callback of its own would otherwise erase the real owner */
    save = shared_loop_thread;
    shared_loop_thread = pthread_self();
    pmix_event_loop(shared_thread_tracker->ev_base, PMIX_EVLOOP_ONCE);
    shared_loop_thread = save;
}

static void stop_progress_engine(pmix_progress_tracker_t *trk)
{
    if (!trk->ev_active) {
        return;
    }

    if (0 == strcmp(trk->name, shared_thread_name)) {
        // mark progress thread as stopped to prevent new entries from being
        // added via PMIx API
        pmix_atomic_set_bool(&pmix_globals.progress_thread_stopped);
    }

    trk->ev_active = false;
    /* break the event loop - this will cause the loop to exit upon
       completion of any current event */
    pmix_event_base_loopexit(trk->ev_base);
    pmix_thread_join(&trk->engine, NULL);
}

static void checkev(int fd, short args, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t*)cbdata;
    PMIX_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_WAKEUP_THREAD(lock);
}

void PMIx_Progress_thread_stop(const pmix_info_t *info, size_t ninfo)
{
    size_t n;
    bool flush = true;
    pmix_lock_t lock;
    pmix_event_t ev;
    char *key;
    const char *name = NULL;
    pmix_progress_tracker_t *trk;

    if (!inited) {
        /* the tracking list has not been constructed, so there is
         * nothing to stop - and walking it would dereference NULL */
        return;
    }

    for (n=0; n < ninfo; n++) {
        key = (char*)info[n].key;
        if (PMIx_Check_key(key, PMIX_PROGRESS_THREAD_FLUSH)) {
            flush = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIx_Check_key(key, PMIX_PROGRESS_THREAD_NAME)) {
            /* a caller that gives us the key with the wrong type, or with
             * a NULL string, must not send strcmp a NULL below */
            if (PMIX_STRING == info[n].value.type &&
                NULL != info[n].value.data.string) {
                name = info[n].value.data.string;
            }
        }
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* If the progress thread is active, stop it */
            if (trk->ev_active) {
                if (flush) {
                    // put a marker event at the end of the event list
                    PMIX_CONSTRUCT_LOCK(&lock);
                    pmix_event_assign(&ev, trk->ev_base, -1, EV_WRITE, checkev, &lock);
                    PMIX_POST_OBJECT(&lock);
                    pmix_event_active(&ev, EV_WRITE, 1);
                    PMIX_WAIT_THREAD(&lock);
                    PMIX_DESTRUCT_LOCK(&lock);
                }
                stop_progress_engine(trk);
            }
        }
    }

}

#ifdef HAVE_PTHREAD_SETAFFINITY_NP
/*
 * Parse one entry of the progress_thread_cpus list.
 *
 * An entry is either a single cpu number or an inclusive "start-end"
 * range. Returns true, and fills in start/end, only if the whole entry
 * parsed and every cpu it names is representable in a cpu_set_t.
 *
 * strtoul on its own cannot answer that question: it reports 0 for a
 * token containing no digits at all, so a typo silently becomes "bind to
 * cpu 0", and it happily returns values beyond CPU_SETSIZE, which CPU_SET
 * either drops on the floor (glibc, musl) or writes past the end of the
 * mask for (BSD). Reject the entry here instead and tell the user.
 */
static bool parse_cpu_range(const char *entry, long *start, long *end)
{
    char *endp;
    const char *rest;
    long lo, hi;

    if (NULL == entry || '\0' == entry[0]) {
        return false;
    }

    errno = 0;
    lo = strtol(entry, &endp, 10);
    if (endp == entry || 0 != errno) {
        return false;
    }

    if ('\0' == *endp) {
        /* a bare entry is a single cpu */
        hi = lo;
    } else if ('-' == *endp) {
        rest = endp + 1;
        errno = 0;
        hi = strtol(rest, &endp, 10);
        if (endp == rest || '\0' != *endp || 0 != errno) {
            return false;
        }
    } else {
        /* trailing garbage */
        return false;
    }

    if (0 > lo || hi < lo || CPU_SETSIZE <= hi) {
        return false;
    }

    *start = lo;
    *end = hi;
    return true;
}
#endif

static int start_progress_engine(pmix_progress_tracker_t *trk)
{
#ifdef HAVE_PTHREAD_SETAFFINITY_NP
    cpu_set_t cpuset;
    char **ranges;
    int k, n, ncpus;
    long start, end;
#endif

    assert(!trk->ev_active);
    trk->ev_active = true;

    /* fork off a thread to progress it */
    trk->engine.t_run = progress_engine;
    trk->engine.t_arg = trk;

    if (0 == strcmp(trk->name, shared_thread_name)) {
        // mark progress thread as running to enable PMIx APIs
        pmix_atomic_unset_bool(&pmix_globals.progress_thread_stopped);
    }

    int rc = pmix_thread_start(&trk->engine);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* we claimed the tracker was active before creating the thread,
         * so take that back - there is no engine. The resume path leaves
         * a failed tracker on the list, and everything that later stops
         * or finalizes it gates on ev_active: it would post a flush event
         * to a base nobody is driving and then wait forever for it, and
         * then ask to join a thread that was never started */
        trk->ev_active = false;
        if (0 == strcmp(trk->name, shared_thread_name)) {
            pmix_atomic_set_bool(&pmix_globals.progress_thread_stopped);
        }
        return rc;
    }

#ifdef HAVE_PTHREAD_SETAFFINITY_NP
    if (NULL != pmix_progress_thread_cpus) {
        CPU_ZERO(&cpuset);
        ncpus = 0;
        // comma-delimited list of cpu ranges
        ranges = PMIx_Argv_split(pmix_progress_thread_cpus, ',');
        for (n=0; NULL != ranges && NULL != ranges[n]; n++) {
            if (!parse_cpu_range(ranges[n], &start, &end)) {
                pmix_show_help("help-pmix-runtime.txt", "progress-thread:bad-cpu-list",
                               true, ranges[n], pmix_progress_thread_cpus,
                               (int) CPU_SETSIZE - 1);
                continue;
            }
            // the range is inclusive of both endpoints
            for (k = (int) start; k <= (int) end; k++) {
                CPU_SET(k, &cpuset);
                ++ncpus;
            }
        }
        PMIx_Argv_free(ranges);

        if (0 == ncpus) {
            /* every entry was rejected. Calling setaffinity with an empty
             * mask just fails with EINVAL, which would be reported as a
             * binding failure and send the user looking in the wrong
             * place - we have already said what is actually wrong */
            rc = PMIX_ERR_BAD_PARAM;
        } else {
            rc = (0 == pthread_setaffinity_np(trk->engine.t_handle, sizeof(cpu_set_t), &cpuset))
                 ? PMIX_SUCCESS : PMIX_ERR_NOT_SUPPORTED;
        }
        if (PMIX_SUCCESS != rc && pmix_bind_progress_thread_reqd) {
            pmix_output(0, "Failed to bind progress thread %s", trk->name);
            /* the engine thread is already running - stop it before we
             * report failure so the caller can safely tear down the
             * tracker without freeing a live event base */
            stop_progress_engine(trk);
        } else {
            rc = PMIX_SUCCESS;
        }
    }
#endif
    return rc;
}

pmix_event_base_t *pmix_progress_thread_init(const char *name)
{
    pmix_progress_tracker_t *trk;

    if (!inited) {
        PMIX_CONSTRUCT(&tracking, pmix_list_t);
        inited = true;
    }

    if (NULL == name) {
        name = shared_thread_name;
    }

    /* check if we already have this thread */
    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* we do, so up the refcount on it */
            ++trk->refcount;
            /* return the existing base */
            return trk->ev_base;
        }
    }

    trk = PMIX_NEW(pmix_progress_tracker_t);
    if (NULL == trk) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return NULL;
    }

    trk->name = strdup(name);
    if (NULL == trk->name) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        PMIX_RELEASE(trk);
        return NULL;
    }

    if (NULL == (trk->ev_base = pmix_event_base_create())) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        PMIX_RELEASE(trk);
        return NULL;
    }

    /* add an event to the new event base (if there are no events,
       pmix_event_loop() will return immediately) */
    pmix_event_assign(&trk->block, trk->ev_base, -1, PMIX_EV_PERSIST, dummy_timeout_cb, trk);
    trk->block_assigned = true;
    pmix_event_add(&trk->block, &long_timeout);

    /* construct the thread object */
    PMIX_CONSTRUCT(&trk->engine, pmix_thread_t);
    trk->engine_constructed = true;
    pmix_list_append(&tracking, &trk->super);

    if (0 == strcmp(name, shared_thread_name)) {
        shared_thread_tracker = trk;
    }

    return trk->ev_base;
}

pmix_status_t pmix_progress_thread_start(const char *name)
{
    pmix_progress_tracker_t *trk;
    pmix_status_t rc;

    if (!inited) {
        /* nothing we can do */
        return PMIX_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
        if (pmix_globals.external_progress) {
            /* the host drives our event base itself, so there is no
             * engine to spin - but the library is now open for business,
             * and the public APIs gate on this flag */
            pmix_atomic_unset_bool(&pmix_globals.progress_thread_stopped);
            return PMIX_SUCCESS;
        }
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* If the progress thread is active, ignore the request */
            if (trk->ev_active) {
                return PMIX_SUCCESS;
            }
            if (PMIX_SUCCESS != (rc = start_progress_engine(trk))) {
                PMIX_ERROR_LOG(rc);
                /* the engine failed to start (start_progress_engine has
                 * already stopped it if it had begun running); drop the
                 * tracker from the list and clear the shared cache so
                 * nothing later dereferences the freed tracker */
                pmix_list_remove_item(&tracking, &trk->super);
                if (trk == shared_thread_tracker) {
                    shared_thread_tracker = NULL;
                }
                PMIX_RELEASE(trk);
            }
            return rc;
        }
    }

    return PMIX_ERR_NOT_FOUND;
}

pmix_status_t pmix_progress_thread_stop(const char *name)
{
    pmix_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PMIX_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
        /* NOTE: deliberately no external_progress early-out here. In that
         * mode there is no engine to join, but the refcount still has to
         * be dropped and the base still has to be freed - otherwise the
         * shared tracker outlives finalize, PMIx_Progress can still be
         * pointed at a base whose components have all been closed, and
         * the base is never returned to the system. The stop below is
         * already gated on ev_active, which is false in that mode. */
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* decrement the refcount */
            --trk->refcount;

            /* If the refcount is still above 0, we're done here */
            if (trk->refcount > 0) {
                return PMIX_SUCCESS;
            }

            /* If the progress thread is active, stop it */
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }
            pmix_list_remove_item(&tracking, &trk->super);
            /* if this is the shared tracker, clear the cached pointer so a
             * PMIx_Progress call between finalize and the next init does
             * not dereference freed memory */
            if (trk == shared_thread_tracker) {
                shared_thread_tracker = NULL;
                /* nothing may be posted to a base we are about to free.
                 * stop_progress_engine sets this too, but only when there
                 * was a running engine to stop - which is not the case
                 * under external progress */
                pmix_atomic_set_bool(&pmix_globals.progress_thread_stopped);
            }
            PMIX_RELEASE(trk);
            return PMIX_SUCCESS;
        }
    }

    return PMIX_ERR_NOT_FOUND;
}

/*
 * Stop the progress thread, but don't delete the tracker (or event base)
 */
pmix_status_t pmix_progress_thread_pause(const char *name)
{
    pmix_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PMIX_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
        if (pmix_globals.external_progress) {
            /* there is no engine to stop, but the shared loop is no
             * longer to be fed - say so, or the APIs go on accepting
             * work for a loop nobody is going to drive again */
            pmix_atomic_set_bool(&pmix_globals.progress_thread_stopped);
            return PMIX_SUCCESS;
        }
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                stop_progress_engine(trk);
            }

            return PMIX_SUCCESS;
        }
    }

    return PMIX_ERR_NOT_FOUND;
}

pmix_status_t pmix_progress_thread_resume(const char *name)
{
    pmix_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PMIX_ERR_NOT_FOUND;
    }

    if (NULL == name) {
        name = shared_thread_name;
        if (pmix_globals.external_progress) {
            /* mirror of pause: no engine to re-spin, but the shared loop
             * is being fed again */
            pmix_atomic_unset_bool(&pmix_globals.progress_thread_stopped);
            return PMIX_SUCCESS;
        }
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            if (trk->ev_active) {
                return PMIX_ERR_RESOURCE_BUSY;
            }

            return start_progress_engine(trk);
        }
    }

    return PMIX_ERR_NOT_FOUND;
}
