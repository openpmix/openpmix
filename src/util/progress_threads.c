/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "pmix_config.h"
#include "constants.h"

#include <unistd.h>
#include <event.h>
#include <pthread.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "src/class/pmix_list.h"
#include "src/util/error.h"
#include "src/util/fd.h"

#include "src/util/progress_threads.h"

/* create a tracking object for progress threads */
typedef struct {
    pmix_list_item_t super;
    char *name;
    struct event_base *ev_base;
    volatile bool ev_active;
    bool block_active;
    struct event block;
    pthread_t engine;
    int pipe[2];
} pmix_progress_tracker_t;
static void trkcon(pmix_progress_tracker_t *p)
{
    p->name = NULL;
    p->ev_base = NULL;
    p->ev_active = true;
    p->block_active = false;
    p->pipe[0] = -1;
    p->pipe[1] = -1;
}
static void trkdes(pmix_progress_tracker_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    if (p->block_active) {
        event_del(&p->block);
    }
    if (NULL != p->ev_base) {
        event_base_free(p->ev_base);
    }
    if (0 <= p->pipe[0]) {
        close(p->pipe[0]);
    }
    if (0 <= p->pipe[1]) {
        close(p->pipe[1]);
    }
}
static OBJ_CLASS_INSTANCE(pmix_progress_tracker_t,
                          pmix_list_item_t,
                          trkcon, trkdes);

static pmix_list_t tracking;
static bool inited = false;
static void wakeup(int fd, short args, void *cbdata)
{
    pmix_progress_tracker_t *trk = (pmix_progress_tracker_t*)cbdata;

    /* if this event fired, then the blocker event will
     * be deleted from the event base by libevent, so flag
     * it so we don't try to delete it again */
    trk->block_active = false;
}
static void* progress_engine(void *obj)
{
    pmix_progress_tracker_t *trk = (pmix_progress_tracker_t*)obj;

    while (trk->ev_active) {
        event_base_loop(trk->ev_base, EVLOOP_ONCE);
    }
    return NULL;
}

struct event_base *pmix_start_progress_thread(char *name,
                                              bool create_block)
{
    pmix_progress_tracker_t *trk;
    int rc;

    trk = OBJ_NEW(pmix_progress_tracker_t);
    trk->name = strdup(name);
    if (NULL == (trk->ev_base = event_base_new())) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        OBJ_RELEASE(trk);
        return NULL;
    }

    if (create_block) {
        /* add an event it can block on */
        if (0 > pipe(trk->pipe)) {
            PMIX_ERROR_LOG(PMIX_ERR_IN_ERRNO);
            OBJ_RELEASE(trk);
            return NULL;
        }
        /* Make sure the pipe FDs are set to close-on-exec so that
           they don't leak into children */
        if (pmix_fd_set_cloexec(trk->pipe[0]) != PMIX_SUCCESS ||
            pmix_fd_set_cloexec(trk->pipe[1]) != PMIX_SUCCESS) {
            PMIX_ERROR_LOG(PMIX_ERR_IN_ERRNO);
            OBJ_RELEASE(trk);
            return NULL;
        }
        event_assign(&trk->block, trk->ev_base, trk->pipe[0], EV_READ, wakeup, trk);
        event_add(&trk->block, 0);
        trk->block_active = true;
    }

    /* fork off a thread to progress it */
    if (PMIX_SUCCESS != (rc = pthread_create(&trk->engine, NULL, progress_engine, (void*)trk))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(trk);
        return NULL;
    }
    if (!inited) {
        OBJ_CONSTRUCT(&tracking, pmix_list_t);
        inited = true;
    }
    pmix_list_append(&tracking, &trk->super);
    return trk->ev_base;
}

void pmix_stop_progress_thread(char *name, bool cleanup)
{
    pmix_progress_tracker_t *trk;
    int i;

    if (!inited) {
        /* nothing we can do */
        return;
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH(trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* mark it as inactive */
            trk->ev_active = false;
            /* break the event loop - this will cause the loop to exit
             * upon completion of any current event */
            event_base_loopbreak(trk->ev_base);
            /* if present, use the block to break it loose just in
             * case the thread is blocked in a call to select for
             * a long time */
            if (trk->block_active) {
                i=1;
                write(trk->pipe[1], &i, sizeof(int));
            }
            /* wait for thread to exit */
            pthread_join(trk->engine, NULL);
            /* cleanup, if they indicated they are done with this event base */
            if (cleanup) {
                pmix_list_remove_item(&tracking, &trk->super);
                OBJ_RELEASE(trk);
            }
            return;
        }
    }
}

int pmix_restart_progress_thread(char *name)
{
    pmix_progress_tracker_t *trk;
    int rc;

    if (!inited) {
        /* nothing we can do */
        return PMIX_ERROR;
    }

    /* find the specified engine */
    PMIX_LIST_FOREACH(trk, &tracking, pmix_progress_tracker_t) {
        if (0 == strcmp(name, trk->name)) {
            /* ensure the block is set, if requested */
            if (0 <= trk->pipe[0] && !trk->block_active) {
                event_add(&trk->block, 0);
                trk->block_active = true;
            }
            /* start the thread again */
            if (PMIX_SUCCESS != (rc = pthread_create(&trk->engine, NULL, progress_engine, (void*)trk))) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_FOUND;
}
