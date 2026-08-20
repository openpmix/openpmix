/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 *
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#    include <string.h>
#endif /* HAVE_STRING_H */
#include <pthread.h>
#include <stdio.h>
#include <event.h>

#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

#include "psensor_heartbeat.h"
#include "src/mca/psensor/base/base.h"

/* declare the API functions */
static pmix_status_t heartbeat_start(pmix_peer_t *requestor, pmix_status_t error,
                                     const pmix_info_t *monitor, const pmix_info_t directives[],
                                     size_t ndirs);
static pmix_status_t heartbeat_stop(pmix_peer_t *requestor, char *id);

/* instantiate the module */
pmix_psensor_base_module_t pmix_psensor_heartbeat_module = {
    .start = heartbeat_start,
    .stop = heartbeat_stop
};

/* tracker object */
typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *requestor;
    char *id;
    bool event_active;
    pmix_event_t ev;
    pmix_event_t cdev;
    struct timeval tv;
    uint32_t nbeats;
    uint32_t ndrops;
    uint32_t nmissed;
    pmix_status_t error;
    pmix_data_range_t range;
    pmix_proc_t source;
    pmix_info_t *info;
    size_t ninfo;
    bool stopped;
} pmix_heartbeat_trkr_t;

static void ft_constructor(pmix_heartbeat_trkr_t *ft)
{
    ft->requestor = NULL;
    ft->id = NULL;
    ft->event_active = false;
    ft->tv.tv_sec = 0;
    ft->tv.tv_usec = 0;
    ft->nbeats = 0;
    ft->ndrops = 0;
    ft->nmissed = 0;
    ft->error = PMIX_SUCCESS;
    ft->range = PMIX_RANGE_NAMESPACE;
    PMIX_PROC_CONSTRUCT(&ft->source);
    ft->info = NULL;
    ft->ninfo = 0;
    ft->stopped = false;
}
static void ft_destructor(pmix_heartbeat_trkr_t *ft)
{
    if (NULL != ft->requestor) {
        PMIX_RELEASE(ft->requestor);
    }
    if (NULL != ft->id) {
        free(ft->id);
    }
    if (ft->event_active) {
        pmix_event_del(&ft->ev);
    }
    if (NULL != ft->info) {
        PMIX_INFO_FREE(ft->info, ft->ninfo);
    }
}
PMIX_CLASS_INSTANCE(pmix_heartbeat_trkr_t, pmix_list_item_t, ft_constructor, ft_destructor);

/* define a local caddy */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_peer_t *requestor;
    char *id;
} heartbeat_caddy_t;
static void cd_con(heartbeat_caddy_t *p)
{
    p->requestor = NULL;
    p->id = NULL;
}
static void cd_des(heartbeat_caddy_t *p)
{
    if (NULL != (p->requestor)) {
        PMIX_RELEASE(p->requestor);
    }
    if (NULL != p->id) {
        free(p->id);
    }
}
PMIX_CLASS_INSTANCE(heartbeat_caddy_t, pmix_object_t, cd_con, cd_des);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_peer_t *peer;
} pmix_psensor_beat_t;

static void bcon(pmix_psensor_beat_t *p)
{
    p->peer = NULL;
}
static void bdes(pmix_psensor_beat_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
}
PMIX_CLASS_INSTANCE(pmix_psensor_beat_t, pmix_object_t, bcon, bdes);

static void check_heartbeat(int fd, short dummy, void *arg);

static void add_tracker(int sd, short flags, void *cbdata)
{
    pmix_heartbeat_trkr_t *ft = (pmix_heartbeat_trkr_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(ft);
    PMIX_HIDE_UNUSED_PARAMS(sd, flags);

    /* add the tracker to our list */
    pmix_list_append(&pmix_mca_psensor_heartbeat_component.trackers, &ft->super);

    /* Setup the timer event. The timer is PERSISTENT, and that is a
     * correctness requirement rather than a convenience: the tracker can
     * be released from a thread that is not this base's, so a one-shot
     * that check_heartbeat re-arms on its way out can be re-armed after
     * the destructor's pmix_event_del has already removed it - leaving a
     * live timer pointing at freed memory. See "The tracker timer is
     * persistent" in ../AGENTS.md; do not simplify this back to
     * pmix_event_evtimer_set(), which asks for flags 0. */
    pmix_event_assign(&ft->ev, pmix_psensor_base.evbase, -1, PMIX_EV_PERSIST,
                      check_heartbeat, ft);
    pmix_event_add(&ft->ev, &ft->tv);
    ft->event_active = true;
}

static pmix_status_t heartbeat_start(pmix_peer_t *requestor, pmix_status_t error,
                                     const pmix_info_t *monitor, const pmix_info_t directives[],
                                     size_t ndirs)
{
    pmix_heartbeat_trkr_t *ft;
    size_t n;
    uint32_t u32;
    pmix_status_t rc;
    pmix_ptl_posted_recv_t *rcv;

    pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] checking heartbeat monitoring for requestor %s:%d",
                         pmix_globals.myid.nspace, pmix_globals.myid.rank,
                         requestor->info->pname.nspace, requestor->info->pname.rank);

    /* A cancel names the monitor by the id its original request carried,
     * and our stop() already does exactly that matching - so service it
     * here, but still decline. The id may name a pstat op rather than one
     * of ours, and pmix_psensor_base_start stops walking the moment a
     * module claims a request: claiming a cancel we know nothing about
     * would strand it. Both ends are tolerant of an id they do not hold,
     * so offering it to everyone is the right shape. PMIX_MONITOR_CANCEL
     * is a char*, and a NULL one means "cancel everything this requestor
     * started" - which is what stop() already does with a NULL id. A
     * value of some other type is a malformed request rather than a
     * request to cancel everything, so leave it alone and let the pstat
     * path reject it. */
    if (0 == strcmp(monitor->key, PMIX_MONITOR_CANCEL)) {
        if (PMIX_STRING == monitor->value.type) {
            (void) heartbeat_stop(requestor, monitor->value.data.string);
        }
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* if they didn't ask for heartbeats, then nothing for us to do */
    if (0 != strcmp(monitor->key, PMIX_MONITOR_HEARTBEAT)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* setup to track this monitoring operation */
    ft = PMIX_NEW(pmix_heartbeat_trkr_t);
    PMIX_RETAIN(requestor);
    ft->requestor = requestor;
    ft->error = error;

    /* Check the directives to see what they want monitored. Every value
     * here arrives off the wire from a client, so read the numbers
     * through PMIx_Value_get_number rather than reaching into the union
     * for a type the sender never promised to have sent. */
    for (n = 0; n < ndirs; n++) {
        if (0 == strcmp(directives[n].key, PMIX_MONITOR_HEARTBEAT_TIME)) {
            rc = PMIx_Value_get_number(&directives[n].value, (void *) &u32, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(ft);
                return rc;
            }
            ft->tv.tv_sec = u32;
        } else if (0 == strcmp(directives[n].key, PMIX_MONITOR_HEARTBEAT_DROPS)) {
            rc = PMIx_Value_get_number(&directives[n].value, (void *) &u32, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(ft);
                return rc;
            }
            ft->ndrops = u32;
        } else if (0 == strcmp(directives[n].key, PMIX_MONITOR_ID)) {
            /* the cancel handle. Nothing stops a caller from naming the
             * monitor twice; the first name wins, as overwriting would
             * lose the string we already took */
            if (PMIX_STRING != directives[n].value.type ||
                NULL == directives[n].value.data.string) {
                PMIX_RELEASE(ft);
                return PMIX_ERR_BAD_PARAM;
            }
            if (NULL == ft->id) {
                ft->id = strdup(directives[n].value.data.string);
            }
        } else if (0 == strcmp(directives[n].key, PMIX_RANGE)) {
            if (PMIX_DATA_RANGE != directives[n].value.type) {
                PMIX_RELEASE(ft);
                return PMIX_ERR_BAD_PARAM;
            }
            ft->range = directives[n].value.data.range;
        }
    }

    if (0 == ft->tv.tv_sec) {
        /* didn't specify a sample rate, or what should be sampled */
        PMIX_RELEASE(ft);
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the recv hasn't been posted, so so now */
    if (!pmix_mca_psensor_heartbeat_component.recv_active) {
        /* setup to receive heartbeats */
        rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
        rcv->tag = PMIX_PTL_TAG_HEARTBEAT;
        rcv->cbfunc = pmix_psensor_heartbeat_recv_beats;
        /* add it to the beginning of the list of recvs */
        pmix_list_prepend(&pmix_ptl_base.posted_recvs, &rcv->super);
        pmix_mca_psensor_heartbeat_component.recv_active = true;
    }

    /* need to push into our event base to add this to our trackers */
    pmix_event_assign(&ft->cdev, pmix_psensor_base.evbase, -1, EV_WRITE, add_tracker, ft);
    PMIX_POST_OBJECT(ft);
    pmix_event_active(&ft->cdev, EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void del_tracker(int sd, short flags, void *cbdata)
{
    heartbeat_caddy_t *cd = (heartbeat_caddy_t *) cbdata;
    pmix_heartbeat_trkr_t *ft, *ftnext;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, flags);

    /* remove the tracker from our list */
    PMIX_LIST_FOREACH_SAFE (ft, ftnext, &pmix_mca_psensor_heartbeat_component.trackers,
                            pmix_heartbeat_trkr_t) {
        if (ft->requestor != cd->requestor) {
            continue;
        }
        if (NULL == cd->id || (NULL != ft->id && 0 == strcmp(ft->id, cd->id))) {
            pmix_list_remove_item(&pmix_mca_psensor_heartbeat_component.trackers, &ft->super);
            PMIX_RELEASE(ft);
        }
    }
    PMIX_RELEASE(cd);
}

static pmix_status_t heartbeat_stop(pmix_peer_t *requestor, char *id)
{
    heartbeat_caddy_t *cd;

    cd = PMIX_NEW(heartbeat_caddy_t);
    PMIX_RETAIN(requestor);
    cd->requestor = requestor;
    if (NULL != id) {
        cd->id = strdup(id);
    }

    /* need to push into our event base to remove this from our trackers */
    pmix_event_assign(&cd->ev, pmix_psensor_base.evbase, -1, EV_WRITE, del_tracker, cd);
    PMIX_POST_OBJECT(cd);
    pmix_event_active(&cd->ev, EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_heartbeat_trkr_t *ft = (pmix_heartbeat_trkr_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_RELEASE(ft); // maintain accounting
}

/* this function automatically gets periodically called
 * by the event library so we can check on the state
 * of the various procs we are monitoring
 */
static void check_heartbeat(int fd, short dummy, void *cbdata)
{
    pmix_heartbeat_trkr_t *ft = (pmix_heartbeat_trkr_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(ft);
    PMIX_HIDE_UNUSED_PARAMS(fd, dummy);


    pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] sensor:check_heartbeat for proc %s:%d", pmix_globals.myid.nspace,
                         pmix_globals.myid.rank, ft->requestor->info->pname.nspace,
                         ft->requestor->info->pname.rank);

    if (0 == ft->nbeats && !ft->stopped) {
        /* no heartbeat recvd in last window */
        ++ft->nmissed;
        pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                             "[%s:%d] sensor:check_heartbeat missed %u of %u allowed for proc %s:%d",
                             pmix_globals.myid.nspace, pmix_globals.myid.rank,
                             ft->nmissed, ft->ndrops,
                             ft->requestor->info->pname.nspace, ft->requestor->info->pname.rank);
        /* PMIX_MONITOR_HEARTBEAT_DROPS says how many windows the
         * requestor is willing to have go by empty before we call the
         * process dead. It was parsed and then thrown away, so a request
         * that asked for slack got none: the very first empty window
         * alerted. Leaving ndrops at its default of 0 keeps that
         * behavior, which is what a request that never named the
         * attribute is asking for. add_beat clears the count, so this
         * counts *consecutive* empty windows. */
        if (ft->nmissed <= ft->ndrops) {
            ft->nbeats = 0;
            return;
        }
        /* generate an event - the source proc lives in the tracker (not
         * on the stack) because PMIx_Notify_event borrows the pointer and
         * processes the event asynchronously, after we return */
        pmix_strncpy(ft->source.nspace, ft->requestor->info->pname.nspace, PMIX_MAX_NSLEN);
        ft->source.rank = ft->requestor->info->pname.rank;
        /* ensure the tracker remains throughout the process */
        PMIX_RETAIN(ft);
        /* mark that the process appears stopped so we don't
         * continue to report it */
        ft->stopped = true;
        rc = PMIx_Notify_event((PMIX_SUCCESS == ft->error) ? PMIX_MONITOR_HEARTBEAT_ALERT
                                                            : ft->error,
                               &ft->source, ft->range, ft->info,
                               ft->ninfo, opcbfunc, ft);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            /* opcbfunc is not going to run - PMIx_Notify_event rejects a
             * request outright (PMIX_ERR_NOT_AVAILABLE once the progress
             * thread has stopped, which finalize does while our sampler
             * may still be firing) without ever reaching the callback.
             * Give back the reference we just took; the list still holds
             * the tracker's own. */
            PMIX_RELEASE(ft);
        }
    } else {
        pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                             "[%s:%d] sensor:check_heartbeat detected %d beats for proc %s:%d",
                             pmix_globals.myid.nspace, pmix_globals.myid.rank, ft->nbeats,
                             ft->requestor->info->pname.nspace, ft->requestor->info->pname.rank);
    }
    /* reset for next period. The timer re-arms itself - see the comment
     * in add_tracker for why the sampler must not do it. */
    ft->nbeats = 0;
}

static void add_beat(int sd, short args, void *cbdata)
{
    pmix_psensor_beat_t *b = (pmix_psensor_beat_t *) cbdata;
    pmix_heartbeat_trkr_t *ft;

    PMIX_ACQUIRE_OBJECT(b);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* find this peer in our trackers */
    PMIX_LIST_FOREACH (ft, &pmix_mca_psensor_heartbeat_component.trackers, pmix_heartbeat_trkr_t) {
        if (ft->requestor == b->peer) {
            /* increment the beat count */
            ++ft->nbeats;
            /* ensure we know that the proc is alive */
            ft->stopped = false;
            /* the drop allowance is spent on *consecutive* empty
             * windows, so a beat gives it all back */
            ft->nmissed = 0;
            break;
        }
    }

    PMIX_RELEASE(b);
}

void pmix_psensor_heartbeat_recv_beats(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                                       pmix_buffer_t *buf, void *cbdata)
{
    pmix_psensor_beat_t *b;

    PMIX_HIDE_UNUSED_PARAMS(hdr, buf, cbdata);

    b = PMIX_NEW(pmix_psensor_beat_t);
    PMIX_RETAIN(peer);
    b->peer = peer;

    /* shift this to our thread for processing */
    pmix_event_assign(&b->ev, pmix_psensor_base.evbase, -1, EV_WRITE, add_beat, b);
    PMIX_POST_OBJECT(b);
    pmix_event_active(&b->ev, EV_WRITE, 1);
}
