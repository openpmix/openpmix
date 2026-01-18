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
    bool engine_constructed;
    pmix_thread_t engine;
} pmix_progress_tracker_t;

static void tracker_constructor(pmix_progress_tracker_t *p)
{
    p->refcount = 1; // start at one since someone created it
    p->name = NULL;
    p->ev_base = NULL;
    p->ev_active = false;
    p->engine_constructed = false;
}

static void tracker_destructor(pmix_progress_tracker_t *p)
{
    pmix_event_del(&p->block);

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

    while (trk->ev_active) {
        pmix_event_loop(trk->ev_base, PMIX_EVLOOP_ONCE);
    }

    return PMIX_THREAD_CANCELLED;
}

void PMIx_Progress(void)
{
    if (NULL == shared_thread_tracker) {
        return;
    }
    pmix_event_loop(shared_thread_tracker->ev_base, PMIX_EVLOOP_ONCE);
}

static void stop_progress_engine(pmix_progress_tracker_t *trk)
{
    if (!trk->ev_active) {
        return;
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
    PMIX_HIDE_UNUSED_PARAMS(fd, args, cbdata);

    PMIX_WAKEUP_THREAD(lock);
}

void PMIx_Progress_thread_stop(const pmix_info_t *info, size_t ninfo)
{
    size_t n;
    bool flush = true;
    pmix_lock_t lock;
    pmix_event_t ev;
    char *key;

    if (!shared_thread_tracker->ev_active) {
        return;
    }

    for (n=0; n < ninfo; n++) {
        key = (char*)info[n].key;
        if (PMIx_Check_key(key, PMIX_PROGRESS_THREAD_FLUSH)) {
            flush = PMIX_INFO_TRUE(&info[n]);
        }
    }

    // mark progress thread as stopped to prevent new entries from being
    // added via PMIx API
    pmix_atomic_set_bool(&pmix_globals.progress_thread_stopped);

    if (flush) {
        // put a marker event at the end of the event list
        PMIX_CONSTRUCT_LOCK(&lock);
        pmix_event_assign(&ev, pmix_globals.evbase, -1, EV_WRITE, checkev, &lock);
        PMIX_POST_OBJECT(&lock);
        pmix_event_active(&ev, EV_WRITE, 1);
        PMIX_WAIT_THREAD(&lock);
        PMIX_DESTRUCT_LOCK(&lock);
    }

    if (shared_thread_tracker->ev_active) {
        stop_progress_engine(shared_thread_tracker);
    }
}

static int start_progress_engine(pmix_progress_tracker_t *trk)
{
#ifdef HAVE_PTHREAD_SETAFFINITY_NP
    cpu_set_t cpuset;
    char **ranges, *dash;
    int k, n, start, end;
#endif

    assert(!trk->ev_active);
    trk->ev_active = true;

    /* fork off a thread to progress it */
    trk->engine.t_run = progress_engine;
    trk->engine.t_arg = trk;

    int rc = pmix_thread_start(&trk->engine);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

#ifdef HAVE_PTHREAD_SETAFFINITY_NP
    if (NULL != pmix_progress_thread_cpus) {
        CPU_ZERO(&cpuset);
        // comma-delimited list of cpu ranges
        ranges = PMIx_Argv_split(pmix_progress_thread_cpus, ',');
        for (n=0; NULL != ranges[n]; n++) {
            // look for '-'
            start = strtoul(ranges[n], &dash, 10);
            if (NULL == dash) {
                CPU_SET(start, &cpuset);
            } else {
                ++dash;  // skip over the '-'
                end = strtoul(dash, NULL, 10);
                for (k=start; k < end; k++) {
                    CPU_SET(k, &cpuset);
                }
            }
        }
        rc = pthread_setaffinity_np(trk->engine.t_handle, sizeof(cpu_set_t), &cpuset);
        if (0 != rc && pmix_bind_progress_thread_reqd) {
            pmix_output(0, "Failed to bind progress thread %s",
                        (NULL == trk->name) ? "NULL" : trk->name);
            rc = PMIX_ERR_NOT_SUPPORTED;
        } else {
            rc = PMIX_SUCCESS;
        }
        PMIx_Argv_free(ranges);
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

    if (NULL == name || 0 == strcmp(name, shared_thread_name)) {
        if (pmix_globals.external_progress) {
            return PMIX_SUCCESS;
        }
        name = shared_thread_name;
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

    if (NULL == name || 0 == strcmp(name, shared_thread_name)) {
        if (pmix_globals.external_progress) {
            return PMIX_SUCCESS;
        }
        name = shared_thread_name;
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
            PMIX_RELEASE(trk);
            return PMIX_SUCCESS;
        }
    }

    return PMIX_ERR_NOT_FOUND;
}

pmix_status_t pmix_progress_thread_finalize(const char *name)
{
    pmix_progress_tracker_t *trk;

    if (!inited) {
        /* nothing we can do */
        return PMIX_ERR_NOT_FOUND;
    }

    if (NULL == name || 0 == strcmp(name, shared_thread_name)) {
        if (pmix_globals.external_progress) {
            return PMIX_SUCCESS;
        }
        name = shared_thread_name;
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH (trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* If the refcount is still above 0, we're done here */
            if (trk->refcount > 0) {
                return PMIX_SUCCESS;
            }

            pmix_list_remove_item(&tracking, &trk->super);
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

    if (NULL == name || 0 == strcmp(name, shared_thread_name)) {
        if (pmix_globals.external_progress) {
            return PMIX_SUCCESS;
        }
        name = shared_thread_name;
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

    if (NULL == name || 0 == strcmp(name, shared_thread_name)) {
        if (pmix_globals.external_progress) {
            return PMIX_SUCCESS;
        }
        name = shared_thread_name;
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
