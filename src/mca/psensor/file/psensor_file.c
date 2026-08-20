/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 *
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "psensor_file.h"
#include "src/mca/psensor/base/base.h"

/* declare the API functions */
static pmix_status_t start(pmix_peer_t *requestor, pmix_status_t error, const pmix_info_t *monitor,
                           const pmix_info_t directives[], size_t ndirs);
static pmix_status_t stop(pmix_peer_t *requestor, char *id);

/* instantiate the module */
pmix_psensor_base_module_t pmix_psensor_file_module = {
    .start = start,
    .stop = stop
};

/* define a tracking object */
typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *requestor;
    char *id;
    bool event_active;
    pmix_event_t ev;
    pmix_event_t cdev;
    struct timeval tv;
    int tick;
    char *file;
    bool sampled;
    bool file_size;
    bool file_access;
    bool file_mod;
    off_t last_size;
    time_t last_access;
    time_t last_mod;
    uint32_t ndrops;
    uint32_t nmisses;
    pmix_status_t error;
    pmix_data_range_t range;
    pmix_proc_t source;
    pmix_info_t *info;
    size_t ninfo;
} file_tracker_t;
static void ft_constructor(file_tracker_t *ft)
{
    ft->requestor = NULL;
    ft->id = NULL;
    ft->event_active = false;
    ft->tv.tv_sec = 0;
    ft->tv.tv_usec = 0;
    ft->tick = 0;
    ft->file = NULL;
    ft->sampled = false;
    ft->file_size = false;
    ft->file_access = false;
    ft->file_mod = false;
    ft->last_size = 0;
    ft->last_access = 0;
    ft->last_mod = 0;
    ft->ndrops = 0;
    ft->nmisses = 0;
    ft->error = PMIX_SUCCESS;
    ft->range = PMIX_RANGE_NAMESPACE;
    PMIX_PROC_CONSTRUCT(&ft->source);
    ft->info = NULL;
    ft->ninfo = 0;
}
static void ft_destructor(file_tracker_t *ft)
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
    if (NULL != ft->file) {
        free(ft->file);
    }
    if (NULL != ft->info) {
        PMIX_INFO_FREE(ft->info, ft->ninfo);
    }
}
PMIX_CLASS_INSTANCE(file_tracker_t,
                    pmix_list_item_t,
                    ft_constructor, ft_destructor);

/* define a local caddy */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_peer_t *requestor;
    char *id;
} file_caddy_t;
static void cd_con(file_caddy_t *p)
{
    p->requestor = NULL;
    p->id = NULL;
}
static void cd_des(file_caddy_t *p)
{
    if (NULL != (p->requestor)) {
        PMIX_RELEASE(p->requestor);
    }
    if (NULL != p->id) {
        free(p->id);
    }
}
PMIX_CLASS_INSTANCE(file_caddy_t, pmix_object_t, cd_con, cd_des);

static void file_sample(int sd, short args, void *cbdata);

static void add_tracker(int sd, short flags, void *cbdata)
{
    file_tracker_t *ft = (file_tracker_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(ft);

    PMIX_HIDE_UNUSED_PARAMS(sd, flags);

    /* add the tracker to our list */
    pmix_list_append(&pmix_mca_psensor_file_component.trackers, &ft->super);

    /* Setup the timer event. The timer is PERSISTENT, and that is a
     * correctness requirement rather than a convenience: the tracker can
     * be released from a thread that is not this base's, so a one-shot
     * that file_sample re-arms on its way out can be re-armed after the
     * destructor's pmix_event_del has already removed it - leaving a live
     * timer pointing at freed memory. See "The tracker timer is
     * persistent" in ../AGENTS.md; do not simplify this back to
     * pmix_event_evtimer_set(), which asks for flags 0. */
    pmix_event_assign(&ft->ev, pmix_psensor_base.evbase, -1, PMIX_EV_PERSIST,
                      file_sample, ft);
    pmix_event_add(&ft->ev, &ft->tv);
    ft->event_active = true;
}

/*
 * Start monitoring of local processes
 */
static pmix_status_t start(pmix_peer_t *requestor, pmix_status_t error, const pmix_info_t *monitor,
                           const pmix_info_t directives[], size_t ndirs)
{
    file_tracker_t *ft;
    size_t n;
    uint32_t u32;
    pmix_status_t rc;

    pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] checking file monitoring for requestor %s:%d",
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
            (void) stop(requestor, monitor->value.data.string);
        }
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* if they didn't ask to monitor a file, then nothing for us to do */
    if (0 != strcmp(monitor->key, PMIX_MONITOR_FILE)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* the request names the file in the monitor's value, and it arrives
     * off the wire from a client - so the type has to be checked and the
     * string can be NULL. We claim the request (the key was ours) and
     * reject it rather than handing strdup a NULL. */
    if (PMIX_STRING != monitor->value.type || NULL == monitor->value.data.string) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* setup to track this monitoring operation */
    ft = PMIX_NEW(file_tracker_t);
    PMIX_RETAIN(requestor);
    ft->requestor = requestor;
    ft->file = strdup(monitor->value.data.string);
    /* the status the requestor wants raised when this trips - see
     * PMIx_Process_monitor(3). PMIX_SUCCESS means they expressed no
     * preference, and then the framework's own alert code stands. */
    ft->error = error;

    /* Check the directives to see what they want monitored. Every value
     * here arrives off the wire from a client, so read the numbers
     * through PMIx_Value_get_number rather than reaching into the union
     * for a type the sender never promised to have sent. */
    for (n = 0; n < ndirs; n++) {
        if (0 == strcmp(directives[n].key, PMIX_MONITOR_FILE_SIZE)) {
            ft->file_size = PMIX_INFO_TRUE(&directives[n]);
        } else if (0 == strcmp(directives[n].key, PMIX_MONITOR_FILE_ACCESS)) {
            ft->file_access = PMIX_INFO_TRUE(&directives[n]);
        } else if (0 == strcmp(directives[n].key, PMIX_MONITOR_FILE_MODIFY)) {
            ft->file_mod = PMIX_INFO_TRUE(&directives[n]);
        } else if (0 == strcmp(directives[n].key, PMIX_MONITOR_FILE_DROPS)) {
            rc = PMIx_Value_get_number(&directives[n].value, (void *) &u32, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(ft);
                return rc;
            }
            ft->ndrops = u32;
        } else if (0 == strcmp(directives[n].key, PMIX_MONITOR_FILE_CHECK_TIME)) {
            rc = PMIx_Value_get_number(&directives[n].value, (void *) &u32, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(ft);
                return rc;
            }
            ft->tv.tv_sec = u32;
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

    if (0 == ft->tv.tv_sec || (!ft->file_size && !ft->file_access && !ft->file_mod)) {
        /* didn't specify a sample rate, or what should be sampled */
        PMIX_RELEASE(ft);
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to push into our event base to add this to our trackers */
    pmix_event_assign(&ft->cdev, pmix_psensor_base.evbase, -1, EV_WRITE, add_tracker, ft);
    PMIX_POST_OBJECT(ft);
    pmix_event_active(&ft->cdev, EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void del_tracker(int sd, short flags, void *cbdata)
{
    file_caddy_t *cd = (file_caddy_t *) cbdata;
    file_tracker_t *ft, *ftnext;

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_HIDE_UNUSED_PARAMS(sd, flags);

    /* remove the tracker from our list */
    PMIX_LIST_FOREACH_SAFE (ft, ftnext, &pmix_mca_psensor_file_component.trackers, file_tracker_t) {
        if (ft->requestor != cd->requestor) {
            continue;
        }
        if (NULL == cd->id || (NULL != ft->id && 0 == strcmp(ft->id, cd->id))) {
            pmix_list_remove_item(&pmix_mca_psensor_file_component.trackers, &ft->super);
            PMIX_RELEASE(ft);
        }
    }
    PMIX_RELEASE(cd);
}

static pmix_status_t stop(pmix_peer_t *requestor, char *id)
{
    file_caddy_t *cd;

    cd = PMIX_NEW(file_caddy_t);
    PMIX_RETAIN(requestor);
    cd->requestor = requestor;
    if (NULL != id) {
        cd->id = strdup(id);
    }

    /* need to push into our event base to add this to our trackers */
    pmix_event_assign(&cd->ev, pmix_psensor_base.evbase, -1, EV_WRITE, del_tracker, cd);
    PMIX_POST_OBJECT(cd);
    pmix_event_active(&cd->ev, EV_WRITE, 1);

    return PMIX_SUCCESS;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    file_tracker_t *ft = (file_tracker_t *) cbdata;

    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_RELEASE(ft);
}

/* Render a time_t the way ctime() would, but into the caller's buffer.
 * ctime() hands back a process-wide static, so the two calls this file
 * used to make in one argument list both pointed at the same bytes and
 * printed the same timestamp twice - and, with the monitors on their own
 * "PSENSOR" thread, they also scribbled on any ctime/asctime result the
 * application was holding elsewhere. The trailing newline ctime() emits
 * is deliberately kept: help-pmix-psensor-file.txt runs the next label
 * straight up against this conversion and relies on it. */
static const char *render_time(time_t when, char *buf, size_t sz)
{
    if (NULL == ctime_r(&when, buf)) {
        /* ctime_r is allowed to decline a time it cannot represent */
        pmix_snprintf(buf, sz, "unknown\n");
    }
    return buf;
}

static void file_sample(int sd, short args, void *cbdata)
{
    file_tracker_t *ft = (file_tracker_t *) cbdata;
    struct stat buf;
    pmix_status_t rc;
    time_t atime, mtime;
    char atod[32], mtod[32];
    bool changed;

    PMIX_ACQUIRE_OBJECT(ft);

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] sampling file %s", pmix_globals.myid.nspace,
                         pmix_globals.myid.rank, ft->file);

    /* stat the file and get its info */
    /* coverity[TOCTOU] */
    if (0 > stat(ft->file, &buf)) {
        /* cannot stat file */
        pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                             "[%s:%d] could not stat %s", pmix_globals.myid.nspace,
                             pmix_globals.myid.rank, ft->file);
        /* the timer re-arms itself, so we simply come back and look
         * again in case this file shows up */
        return;
    }

    atime = buf.st_atime;
    mtime = buf.st_mtime;
    pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] size %llu access %s\tmod %s", pmix_globals.myid.nspace,
                         pmix_globals.myid.rank, (unsigned long long) buf.st_size,
                         render_time(atime, atod, sizeof(atod)),
                         render_time(mtime, mtod, sizeof(mtod)));

    /* The first sample has nothing to compare against - it establishes
     * the baseline and nothing more. Without this the constructor's
     * zeroes stand in for "what the file looked like last time", which is
     * a value a real file can genuinely have: a monitor watching the size
     * of a file that is legitimately empty counted a miss on its very
     * first look. */
    if (!ft->sampled) {
        ft->sampled = true;
        ft->last_size = buf.st_size;
        ft->last_access = atime;
        ft->last_mod = mtime;
        return;
    }

    /* Every attribute the request named is watched, and any one of them
     * moving means the application is alive. An if/else-if chain used to
     * check only the first flag that was set, so a request naming both a
     * size and a modification time silently got one of them - and would
     * report a stall in a file whose mtime was moving all along. */
    changed = false;
    if (ft->file_size && buf.st_size != ft->last_size) {
        ft->last_size = buf.st_size;
        changed = true;
    }
    if (ft->file_access && atime != ft->last_access) {
        ft->last_access = atime;
        changed = true;
    }
    if (ft->file_mod && mtime != ft->last_mod) {
        ft->last_mod = mtime;
        changed = true;
    }
    if (changed) {
        ft->nmisses = 0;
    } else {
        ft->nmisses++;
    }

    pmix_output_verbose(1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] sampled file %s misses %d", pmix_globals.myid.nspace,
                         pmix_globals.myid.rank, ft->file, ft->nmisses);

    /* Alert once the watched attribute has sat still for as many checks
     * as the requestor said it would tolerate. The miss count has to be
     * non-zero for this to mean anything: a request that named no
     * PMIX_MONITOR_FILE_DROPS leaves ndrops at 0, and the first sample of
     * a perfectly healthy file also leaves nmisses at 0 - so an equality
     * test alerted immediately on a file that had just been written. */
    if (0 < ft->nmisses && ft->nmisses >= ft->ndrops) {
        if (4 < pmix_output_get_verbosity(pmix_psensor_base_framework.framework_output)) {
            /* the topic renders the size with %llu; off_t is 64 bits
             * wherever large-file support is on, and varargs will not
             * widen it for us */
            pmix_show_help("help-pmix-psensor-file.txt", "file-stalled", true, ft->file,
                           (unsigned long long) ft->last_size,
                           render_time(ft->last_access, atod, sizeof(atod)),
                           render_time(ft->last_mod, mtod, sizeof(mtod)));
        }
        /* stop monitoring this client. Disarm before we let go of the
         * tracker: the timer is persistent, so it is still armed even
         * though we are inside its callback, and the tracker is about to
         * belong to the notification below. Deleting it here is safe
         * because we are on the base's own thread - libevent re-armed
         * the timeout under the base lock before entering us, so this
         * removes that re-armed one. */
        if (ft->event_active) {
            pmix_event_del(&ft->ev);
            ft->event_active = false;
        }
        pmix_list_remove_item(&pmix_mca_psensor_file_component.trackers, &ft->super);
        /* generate an event - the source proc lives in the tracker (not
         * on the stack) because PMIx_Notify_event borrows the pointer and
         * processes the event asynchronously, after we return */
        pmix_strncpy(ft->source.nspace, ft->requestor->info->pname.nspace, PMIX_MAX_NSLEN);
        ft->source.rank = ft->requestor->info->pname.rank;
        rc = PMIx_Notify_event((PMIX_SUCCESS == ft->error) ? PMIX_MONITOR_FILE_ALERT : ft->error,
                               &ft->source, ft->range, ft->info, ft->ninfo,
                               opcbfunc, ft);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            /* opcbfunc is not going to run - PMIx_Notify_event rejects a
             * request outright (PMIX_ERR_NOT_AVAILABLE once the progress
             * thread has stopped, which finalize does while our sampler
             * may still be firing) without ever reaching the callback.
             * The tracker has just left the list, so the reference we
             * handed the notification is its last one. */
            PMIX_RELEASE(ft);
        }
        return;
    }

    /* nothing to do - the timer re-arms itself. See the comment in
     * add_tracker for why the sampler must not do it. */
}
