/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Event registration and notification on behalf of local clients: the
 * registration store (pmix_server_globals.events), the bookkeeping that
 * decides when our host must be asked to start or stop forwarding a code,
 * the replay of cached events to a late registrant, and the handling of
 * an event a client asks us to notify. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_TIME_H
#    include <time.h>
#endif

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

static void undo_activations(pmix_status_t *codes, size_t nactive);

static void _check_cached_events(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *scd = (pmix_setup_caddy_t *) cbdata;
    pmix_notify_caddy_t *cd;
    pmix_range_trkr_t rngtrk;
    pmix_proc_t proc;
    int i;
    size_t k, n;
    bool found, matched;
    pmix_buffer_t *relay;
    pmix_status_t ret = PMIX_SUCCESS;
    pmix_cmd_t cmd = PMIX_NOTIFY_CMD;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* check if any matching notifications have been cached */
    rngtrk.procs = NULL;
    rngtrk.nprocs = 0;
    for (i = 0; i < pmix_globals.max_events; i++) {
        pmix_hotel_knock(&pmix_globals.notifications, i, (void **) &cd);
        if (NULL == cd) {
            continue;
        }
        found = false;
        if (NULL == scd->codes) {
            if (!cd->nondefault) {
                /* they registered a default event handler - always matches */
                found = true;
            }
        } else {
            for (k = 0; k < scd->ncodes; k++) {
                if (scd->codes[k] == cd->status) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            continue;
        }
        /* check if the affected procs (if given) match those they
         * wanted to know about */
        if (!pmix_notify_check_affected(cd->affected, cd->naffected, scd->procs, scd->nprocs)) {
            continue;
        }
        /* check the range */
        if (NULL == cd->targets) {
            rngtrk.procs = &cd->source;
            rngtrk.nprocs = 1;
        } else {
            rngtrk.procs = cd->targets;
            rngtrk.nprocs = cd->ntargets;
        }
        rngtrk.range = cd->range;
        PMIX_LOAD_PROCID(&proc, scd->peer->info->pname.nspace, scd->peer->info->pname.rank);
        if (!pmix_notify_check_range(&rngtrk, &proc)) {
            continue;
        }
        /* if we were given specific targets, check if this is one */
        found = false;
        if (NULL != cd->targets) {
            matched = false;
            for (n = 0; n < cd->ntargets; n++) {
                /* if the source of the event is the same peer just registered, then ignore it
                 * as the event notification system will have already locally
                 * processed it */
                if (PMIX_CHECK_NAMES(&cd->source, &scd->peer->info->pname)) {
                    continue;
                }
                if (PMIX_CHECK_NAMES(&scd->peer->info->pname, &cd->targets[n])) {
                    matched = true;
                    /* track the number of targets we have left to notify */
                    --cd->nleft;
                    /* if this is the last one, then evict this event
                     * from the cache */
                    if (0 == cd->nleft) {
                        pmix_hotel_checkout(&pmix_globals.notifications, cd->room);
                        found = true; // mark that we should release cd
                    }
                    break;
                }
            }
            if (!matched) {
                /* do not notify this one */
                continue;
            }
        }

        /* all matches - notify */
        relay = PMIX_NEW(pmix_buffer_t);
        if (NULL == relay) {
            /* nothing we can do */
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            ret = PMIX_ERR_NOMEM;
            if (found) {
                /* this event was checked out of the hotel above */
                PMIX_RELEASE(cd);
            }
            break;
        }
        /* pack the info data stored in the event */
        PMIX_BFROPS_PACK(ret, scd->peer, relay, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS == ret) {
            PMIX_BFROPS_PACK(ret, scd->peer, relay, &cd->status, 1, PMIX_STATUS);
        }
        if (PMIX_SUCCESS == ret) {
            PMIX_BFROPS_PACK(ret, scd->peer, relay, &cd->source, 1, PMIX_PROC);
        }
        if (PMIX_SUCCESS == ret) {
            PMIX_BFROPS_PACK(ret, scd->peer, relay, &cd->ninfo, 1, PMIX_SIZE);
        }
        if (PMIX_SUCCESS == ret && 0 < cd->ninfo) {
            PMIX_BFROPS_PACK(ret, scd->peer, relay, cd->info, cd->ninfo, PMIX_INFO);
        }
        if (PMIX_SUCCESS != ret) {
            /* release the relay we could not fill, and the event if it was
             * checked out of the hotel, before abandoning the loop */
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            if (found) {
                PMIX_RELEASE(cd);
            }
            break;
        }
        PMIX_SERVER_QUEUE_REPLY(ret, scd->peer, 0, relay);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(relay);
        }
        if (found) {
            PMIX_RELEASE(cd);
        }
    }
    /* release the caddy */
    if (NULL != scd->codes) {
        free(scd->codes);
    }
    if (NULL != scd->info) {
        PMIX_INFO_FREE(scd->info, scd->ninfo);
    }
    /* Answer the requestor with the status of its _registration_, not
     * with whatever the replay above ran into. The only caller that
     * leaves an opcbfunc on the caddy is the host-completed registration
     * path, and the host said that succeeded - failing to hand a client a
     * stale cached event does not undo it. Reporting the replay status
     * would tell the client its handler was not registered while the
     * server had in fact registered it, leaving a store entry the client
     * would never deregister. The replay failure is logged above. */
    if (NULL != scd->opcbfunc) {
        scd->opcbfunc(scd->status, scd->cbdata);
    }
    PMIX_RELEASE(scd);
}

/* our host refused a registration it had accepted for processing, so it
 * is not forwarding those codes after all - give back the interest we
 * recorded on its behalf when we made the request, or the next
 * registrant for one of them will be told it succeeded and then never
 * see the event. This mirrors what the server's registration of its
 * _own_ handlers does when the host refuses asynchronously (see
 * _reg_hdlr_complete in src/event/pmix_event_registration.c). The peer
 * entries the request added to the store are deliberately left alone: a
 * peer may hold several handlers for the same code, so there is no way
 * to tell from here which entry belongs to the failed request. */
static void _failed_registration(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    undo_activations(cd->codes, cd->nactive);

    /* cleanup and execute the callback so we don't hang */
    if (NULL != cd->codes) {
        free(cd->codes);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(cd->status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

/* provide a callback function for the host when it finishes
 * processing the registration. This may be called on the host's own
 * thread, so it does nothing here but thread-shift - the registration
 * store it has to correct on failure is progress-thread state */
static void regevopcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the caddy carries the codes and info arrays, but its destructor
         * does not free them - so release them here */
        if (NULL != cd->codes) {
            free(cd->codes);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
        return;
    }

    cd->status = status;

    /* if the registration succeeded, then check local cache */
    if (PMIX_SUCCESS == status) {
        PMIX_THREADSHIFT(cd, _check_cached_events);
        return;
    }

    PMIX_THREADSHIFT(cd, _failed_registration);
}

/* the host is done processing our request to stop forwarding a code -
 * all we carried across the up-call was the array of codes itself */
static void deregevopcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_status_t *codes = (pmix_status_t *) cbdata;

    PMIX_HIDE_UNUSED_PARAMS(status);

    free(codes);
}

/* ask our host to stop forwarding the given event codes - takes
 * ownership of the (malloc'd) codes array */
static void stop_forwarding(pmix_status_t *codes, size_t ncodes)
{
    pmix_status_t rc;

    if (NULL == pmix_host_server.deregister_events) {
        free(codes);
        return;
    }
    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "server deregister events: telling host to stop forwarding %lu code(s)",
                        (unsigned long) ncodes);
    rc = pmix_host_server.deregister_events(codes, ncodes, deregevopcbfunc, codes);
    if (PMIX_SUCCESS != rc) {
        /* the host either completed the operation itself or rejected
         * it - either way, no callback is coming */
        free(codes);
    }
}

bool pmix_server_prune_reginfo(pmix_regevents_info_t *reginfo)
{
    pmix_status_t *codes;

    if (0 < pmix_list_get_size(&reginfo->peers) || 0 < reginfo->nmine) {
        /* somebody still wants this code */
        return false;
    }

    pmix_list_remove_item(&pmix_server_globals.events, &reginfo->super);
    if (reginfo->active) {
        codes = (pmix_status_t *) malloc(sizeof(pmix_status_t));
        if (NULL == codes) {
            /* we cannot ask our host to stop, so it will keep forwarding
             * a code nobody here wants - note it and carry on, as the
             * registration itself is still correctly gone */
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        } else {
            codes[0] = reginfo->code;
            stop_forwarding(codes, 1);
        }
    }
    PMIX_RELEASE(reginfo);
    return true;
}

/* find the tracking object for the given code, creating it if necessary */
static pmix_regevents_info_t *get_reginfo(pmix_status_t code)
{
    pmix_regevents_info_t *reginfo;

    PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
        if (code == reginfo->code) {
            return reginfo;
        }
    }
    reginfo = PMIX_NEW(pmix_regevents_info_t);
    if (NULL == reginfo) {
        return NULL;
    }
    reginfo->code = code;
    pmix_list_append(&pmix_server_globals.events, &reginfo->super);
    return reginfo;
}

/* Mark the codes our host is not already forwarding, sorting them to
 * the front of the array, and return their number. The array is treated
 * as an unordered set by everything downstream, so the reordering only
 * lets us hand the host the subset it needs to act upon. */
static size_t mark_activations(pmix_status_t *codes, size_t ncodes)
{
    pmix_regevents_info_t *reginfo;
    pmix_status_t tmp;
    size_t n, nactive = 0;

    for (n = 0; n < ncodes; n++) {
        if (!PMIX_SYSTEM_EVENT(codes[n])) {
            continue;
        }
        PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
            if (codes[n] != reginfo->code) {
                continue;
            }
            if (!reginfo->active) {
                reginfo->active = true;
                tmp = codes[nactive];
                codes[nactive] = codes[n];
                codes[n] = tmp;
                ++nactive;
            }
            break;
        }
    }
    return nactive;
}

/* our host rejected the request, so it is not forwarding these codes
 * after all - let the next registration ask it again */
static void undo_activations(pmix_status_t *codes, size_t nactive)
{
    pmix_regevents_info_t *reginfo;
    size_t n;

    for (n = 0; n < nactive; n++) {
        PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
            if (codes[n] == reginfo->code) {
                reginfo->active = false;
                break;
            }
        }
    }
}

size_t pmix_server_activate_events(pmix_status_t *codes, size_t ncodes)
{
    pmix_regevents_info_t *reginfo;
    size_t n;

    if (NULL == codes) {
        return 0;
    }

    /* record our own interest in each of the system codes */
    for (n = 0; n < ncodes; n++) {
        if (!PMIX_SYSTEM_EVENT(codes[n])) {
            continue;
        }
        reginfo = get_reginfo(codes[n]);
        if (NULL == reginfo) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            continue;
        }
        ++reginfo->nmine;
    }

    return mark_activations(codes, ncodes);
}

void pmix_server_deactivate_events(pmix_status_t *codes, size_t ncodes)
{
    pmix_regevents_info_t *reginfo, *regnext;
    size_t n;

    if (NULL == codes) {
        return;
    }

    for (n = 0; n < ncodes; n++) {
        if (!PMIX_SYSTEM_EVENT(codes[n])) {
            continue;
        }
        PMIX_LIST_FOREACH_SAFE (reginfo, regnext, &pmix_server_globals.events,
                                pmix_regevents_info_t) {
            if (codes[n] != reginfo->code) {
                continue;
            }
            if (0 < reginfo->nmine) {
                --reginfo->nmine;
            }
            pmix_server_prune_reginfo(reginfo);
            break;
        }
    }
}

pmix_status_t pmix_server_register_events(pmix_peer_t *peer, pmix_buffer_t *buf,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_status_t *codes = NULL;
    pmix_info_t *info = NULL;
    size_t ninfo = 0, ncodes, n;
    pmix_regevents_info_t *reginfo, *rptr;
    pmix_peer_events_info_t *prev = NULL;
    pmix_setup_caddy_t *scd;
    bool enviro_events = false;
    bool found;
    pmix_proc_t *affected = NULL;
    size_t naffected = 0;
    size_t nactive = 0;

    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "recvd register events for peer %s:%d",
                        peer->info->pname.nspace, peer->info->pname.rank);

    /* unpack the number of codes */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ncodes, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* The count arrives off the wire in a size_t while the unpack below
     * consumes it through an int32_t, so screen the truncation here.
     * Every loop in this function walks the array for "ncodes" entries
     * while only "cnt" of them were ever unpacked into it - and unlike an
     * info array, this one comes from a bare malloc, so the entries past
     * the unpack are uninitialized rather than constructed */
    cnt = ncodes;
    if (0 > cnt || (size_t) cnt != ncodes) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of codes */
    if (0 < ncodes) {
        codes = (pmix_status_t *) malloc(ncodes * sizeof(pmix_status_t));
        if (NULL == codes) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = ncodes;
        PMIX_BFROPS_UNPACK(rc, peer, buf, codes, &cnt, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        if (NULL == info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* Check the directives. These came off the wire, so the type tag on
     * each value is whatever the peer said it was - screen it before
     * reading the union. A mistyped PMIX_EVENT_AFFECTED_PROC has us copy
     * a pmix_proc_t out of whatever a scalar or a string pointer happens
     * to be, and a mistyped PMIX_EVENT_AFFECTED_PROCS dereferences the
     * same garbage as a pmix_data_array_t; even a correctly-typed array
     * of some other element type would have us read past its end. The
     * equivalent checks the client library makes run in the requestor's
     * process, so they buy us nothing here. */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            if (NULL != affected ||
                PMIX_PROC != info[n].value.type ||
                NULL == info[n].value.data.proc) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto cleanup;
            }
            naffected = 1;
            PMIX_PROC_CREATE(affected, naffected);
            if (NULL == affected) {
                naffected = 0;
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            memcpy(affected, info[n].value.data.proc, sizeof(pmix_proc_t));
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROCS)) {
            if (NULL != affected ||
                PMIX_DATA_ARRAY != info[n].value.type ||
                NULL == info[n].value.data.darray ||
                PMIX_PROC != info[n].value.data.darray->type ||
                NULL == info[n].value.data.darray->array ||
                0 == info[n].value.data.darray->size) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto cleanup;
            }
            naffected = info[n].value.data.darray->size;
            PMIX_PROC_CREATE(affected, naffected);
            if (NULL == affected) {
                naffected = 0;
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            memcpy(affected, info[n].value.data.darray->array, naffected * sizeof(pmix_proc_t));
        }
    }

    /* check the codes for system events */
    for (n = 0; n < ncodes; n++) {
        if (PMIX_SYSTEM_EVENT(codes[n])) {
            enviro_events = true;
            break;
        }
    }

    /* if they asked for enviro events, and our host doesn't support
     * register_events, then we cannot meet the request */
    if (enviro_events && NULL == pmix_host_server.register_events) {
        enviro_events = false;
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    /* if they didn't send us any codes, then they are registering a
     * default event handler. In that case, look only at the default
     * entry - creating it if this is the first default registration we
     * have seen - and add this request to it.
     *
     * Creating it is not optional. This used to walk the list and, if it
     * found no PMIX_MAX_ERR_CONSTANT entry, fall out of the loop and
     * return PMIX_OPERATION_SUCCEEDED having recorded nothing at all: the
     * peer was told its registration succeeded and was never added to any
     * dispatch list, so _notify_client_event() could not find it. The
     * whole point of a default handler is to catch codes nobody asked for
     * by name, so there is frequently no other registration to have
     * created the entry first - and the very first client or tool to
     * attach to a server necessarily finds the list empty. That client
     * then silently received no default-routed event for its lifetime.
     * The code != 0 path below has always created a missing entry; this
     * one simply has to do the same. */
    if (0 == ncodes) {
        reginfo = NULL;
        PMIX_LIST_FOREACH (rptr, &pmix_server_globals.events, pmix_regevents_info_t) {
            if (PMIX_MAX_ERR_CONSTANT == rptr->code) {
                reginfo = rptr;
                break;
            }
        }
        if (NULL == reginfo) {
            reginfo = PMIX_NEW(pmix_regevents_info_t);
            if (NULL == reginfo) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            reginfo->code = PMIX_MAX_ERR_CONSTANT;
            pmix_list_append(&pmix_server_globals.events, &reginfo->super);
        }
        prev = PMIX_NEW(pmix_peer_events_info_t);
        if (NULL == prev) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        PMIX_RETAIN(peer);
        prev->peer = peer;
        if (NULL != affected) {
            PMIX_PROC_CREATE(prev->affected, naffected);
            if (NULL == prev->affected) {
                PMIX_RELEASE(prev);
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            prev->naffected = naffected;
            memcpy(prev->affected, affected, naffected * sizeof(pmix_proc_t));
        }
        pmix_list_append(&reginfo->peers, &prev->super);
        /* fall through rather than returning here: a default handler must
         * still be given any matching notification already sitting in the
         * cache, and _check_cached_events has a dedicated arm for exactly
         * this case (scd->codes == NULL matches every non-nondefault event).
         * Returning early meant a default handler silently missed every
         * event cached before it registered. The loops below are no-ops
         * with ncodes == 0, and the trailing else branch schedules the
         * cached-event check and returns PMIX_OPERATION_SUCCEEDED. */
    }

    /* store the event registration info so we can call the registered
     * client when the server notifies the event */
    for (n = 0; n < ncodes; n++) {
        found = false;
        PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
            if (PMIX_MAX_ERR_CONSTANT == reginfo->code) {
                continue;
            } else if (codes[n] == reginfo->code) {
                found = true;
                break;
            }
        }
        if (found) {
            /* found it - add this request */
            prev = PMIX_NEW(pmix_peer_events_info_t);
            if (NULL == prev) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            PMIX_RETAIN(peer);
            prev->peer = peer;
            if (NULL != affected) {
                PMIX_PROC_CREATE(prev->affected, naffected);
                if (NULL == prev->affected) {
                    PMIX_RELEASE(prev);
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                prev->naffected = naffected;
                memcpy(prev->affected, affected, naffected * sizeof(pmix_proc_t));
            }
            prev->enviro_events = enviro_events;
            pmix_list_append(&reginfo->peers, &prev->super);
        } else {
            /* if we get here, then we didn't find an existing registration for this code */
            rptr = PMIX_NEW(pmix_regevents_info_t);
            if (NULL == rptr) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            rptr->code = codes[n];
            pmix_list_append(&pmix_server_globals.events, &rptr->super);
            prev = PMIX_NEW(pmix_peer_events_info_t);
            if (NULL == prev) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            PMIX_RETAIN(peer);
            prev->peer = peer;
            if (NULL != affected) {
                PMIX_PROC_CREATE(prev->affected, naffected);
                if (NULL == prev->affected) {
                    PMIX_RELEASE(prev);
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                prev->naffected = naffected;
                memcpy(prev->affected, affected, naffected * sizeof(pmix_proc_t));
            }
            prev->enviro_events = enviro_events;
            pmix_list_append(&rptr->peers, &prev->super);
        }
    }

    /* find the system codes in this request that our host is not
     * already forwarding on behalf of another local registrant (or of
     * ourselves) - those are the only ones we need to ask it about */
    if (enviro_events) {
        nactive = mark_activations(codes, ncodes);
    }

    /* if they asked for enviro events our host isn't already sending
     * us, then call the local server */
    if (enviro_events && 0 < nactive) {
        /* if they don't support this, then we cannot do it */
        if (NULL == pmix_host_server.register_events) {
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        /* need to ensure the arrays don't go away until after the
         * host RM is done with them */
        scd = PMIX_NEW(pmix_setup_caddy_t);
        if (NULL == scd) {
            undo_activations(codes, nactive);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        PMIX_RETAIN(peer);
        scd->peer = peer;
        scd->codes = codes;
        scd->ncodes = ncodes;
        scd->nactive = nactive;
        scd->info = info;
        scd->ninfo = ninfo;
        scd->opcbfunc = cbfunc;
        scd->cbdata = cbdata;
        /* hand the caddy the procs this registrant restricted its interest
         * to, so the cached-event replay filters on them exactly as the
         * two completed-locally paths below do. This has to happen before
         * the up-call: the host is free to complete on another thread the
         * moment it has the caddy, and the replay reads these */
        scd->procs = affected;
        scd->nprocs = naffected;
        affected = NULL;
        naffected = 0;
        /* only the codes needing activation were sorted to the front of
         * the array, so that is all we hand the host - the caddy still
         * carries the full array as the cached-event check needs it */
        if (PMIX_SUCCESS
            == (rc = pmix_host_server.register_events(scd->codes, nactive, scd->info,
                                                      scd->ninfo, regevopcbfunc, scd))) {
            /* the host will call us back when completed */
            pmix_output_verbose(
                2, pmix_server_globals.event_output,
                "server register events: host server processing event registration");
            return rc;
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* we need to check cached notifications, but we want to ensure
             * that occurs _after_ the client returns from registering the
             * event handler in case the event is flagged for do_not_cache.
             * Setup an event to fire after we return as that means it will
             * occur after we send the registration response back to the client,
             * thus guaranteeing that the client will get their registration
             * callback prior to delivery of an event notification */
            if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
                /* the caddy carries the codes and info arrays, but its
                 * destructor does not free them - so release them here.
                 * The affected array it now owns goes with the caddy */
                if (NULL != scd->codes) {
                    free(scd->codes);
                }
                if (NULL != scd->info) {
                    PMIX_INFO_FREE(scd->info, scd->ninfo);
                }
                PMIX_RELEASE(scd);
                return PMIX_ERR_NOT_AVAILABLE;
            }

            /* scd->peer and scd->procs were already set (with the peer's
             * retain) above - taking a second reference here leaked one on
             * every registration the host completed atomically */
            scd->opcbfunc = NULL;
            scd->cbdata = NULL;
            PMIX_THREADSHIFT(scd, _check_cached_events);
            return rc;
        } else {
            /* host returned a genuine error and won't be calling the callback function */
            pmix_output_verbose(2, pmix_server_globals.event_output,
                                "server register events: host server reg events returned rc =%d",
                                rc);
            undo_activations(codes, nactive);
            PMIX_RELEASE(scd);
            goto cleanup;
        }
    } else {
        rc = PMIX_OPERATION_SUCCEEDED;
        /* we need to check cached notifications, but we want to ensure
         * that occurs _after_ the client returns from registering the
         * event handler in case the event is flagged for do_not_cache.
         * Setup an event to fire after we return as that means it will
         * occur after we send the registration response back to the client,
         * thus guaranteeing that the client will get their registration
         * callback prior to delivery of an event notification */
        if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
            rc = PMIX_ERR_NOT_AVAILABLE;
            goto cleanup;
        }

        scd = PMIX_NEW(pmix_setup_caddy_t);
        if (NULL == scd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        PMIX_RETAIN(peer);
        scd->peer = peer;
        scd->codes = codes;
        scd->ncodes = ncodes;
        scd->procs = affected;
        scd->nprocs = naffected;
        scd->opcbfunc = NULL;
        scd->cbdata = NULL;
        PMIX_THREADSHIFT(scd, _check_cached_events);
        if (NULL != info) {
            PMIX_INFO_FREE(info, ninfo);
        }
        return rc;
    }

cleanup:
    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "server register events: ninfo =%lu rc =%d",
                        (unsigned long) ninfo, rc);
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    if (NULL != codes) {
        free(codes);
    }
    if (NULL != affected) {
        PMIX_PROC_FREE(affected, naffected);
    }
    return rc;
}

void pmix_server_deregister_events(pmix_peer_t *peer, pmix_buffer_t *buf)
{
    int32_t cnt;
    pmix_status_t rc, code;
    pmix_regevents_info_t *reginfo = NULL;
    pmix_regevents_info_t *reginfo_next;
    pmix_peer_events_info_t *prev, *prev_next;

    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "%s recvd deregister events from %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(peer));

    /* unpack codes and process until done */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &code, &cnt, PMIX_STATUS);
    while (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH_SAFE (reginfo, reginfo_next, &pmix_server_globals.events,
                                pmix_regevents_info_t) {
            if (code == reginfo->code) {
                /* Found it - remove _every_ registration this peer holds
                 * for the code, not just the first. A peer can hold more
                 * than one: its library packs the whole code list of a
                 * handler whenever any code in that list is new to us, so
                 * a second handler naming an already-registered code adds
                 * a second entry here. It sends the deregistration only
                 * when its last handler for the code is gone, so anything
                 * of this peer's still sitting on the list is stale -
                 * stopping at the first left one behind, which kept us
                 * forwarding the code to a peer that had no handler for it
                 * and kept the reginfo from ever being pruned. */
                PMIX_LIST_FOREACH_SAFE (prev, prev_next, &reginfo->peers,
                                        pmix_peer_events_info_t) {
                    if (prev->peer == peer) {
                        pmix_list_remove_item(&reginfo->peers, &prev->super);
                        PMIX_RELEASE(prev);
                    }
                }
                /* if nobody is left registered for this code - not the
                 * server itself, nor any of our local clients - then
                 * remove it and tell our host to stop forwarding it */
                pmix_server_prune_reginfo(reginfo);
            }
        }
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &code, &cnt, PMIX_STATUS);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }
}

static void local_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_notify_caddy_t *cd = (pmix_notify_caddy_t *) cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

static void intermed_step(pmix_status_t status, void *cbdata)
{
    pmix_notify_caddy_t *cd = (pmix_notify_caddy_t *) cbdata;
    pmix_status_t rc;

    if (PMIX_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    /* check the range directive - if it is LOCAL, then we are
     * done. Otherwise, it needs to go up to our
     * host for dissemination */
    if (PMIX_RANGE_LOCAL == cd->range) {
        rc = PMIX_SUCCESS;
        goto complete;
    }

    /* pass it to our host RM for distribution */
    if (NULL != pmix_host_server.notify_event) {
        rc = pmix_host_server.notify_event(cd->status, &cd->source, cd->range,
                                           cd->info, cd->ninfo,
                                           local_cbfunc, cd);
        if (PMIX_SUCCESS == rc) {
            /* let the callback function respond for us */
            return;
        }
        if (PMIX_OPERATION_SUCCEEDED == rc ||
            PMIX_ERR_NOT_SUPPORTED == rc) {
            rc = PMIX_SUCCESS; // local_cbfunc will not be called
        }
    } else {
        rc = PMIX_SUCCESS; // local_cbfunc will not be called
    }

complete:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

/* Receive an event sent by the client library. Since it was sent
 * to us by one client, we have to both process it locally to ensure
 * we notify all relevant local clients AND (assuming a range other
 * than LOCAL) deliver to our host, requesting that they send it
 * to all peer servers in the current session */
pmix_status_t pmix_server_event_recvd_from_client(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_notify_caddy_t *cd;
    size_t ninfo, n;

    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "%s:%d recvd event notification from client %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, peer->info->pname.nspace,
                        peer->info->pname.rank);

    cd = PMIX_NEW(pmix_notify_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* set the source */
    PMIX_LOAD_PROCID(&cd->source, peer->info->pname.nspace, peer->info->pname.rank);

    /* unpack status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the range */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->range, &cnt, PMIX_DATA_RANGE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the info keys */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* if a local member is voluntarily leaving a group, account for its
     * departure in any in-flight group construct/destruct collective for
     * that group so the collective completes on the survivors rather than
     * hanging - the deliberate cousin of a lost participant. We do this
     * before the loop-detection check below because only the originating
     * server (the one that received the leave from its client) sees this
     * command; peer servers receive the relayed event with the
     * PMIX_SERVER_INTERNAL_NOTIFY marker and bail out. */
    if (PMIX_GROUP_LEFT == cd->status) {
        char *grpid = NULL;
        pmix_proc_t *affected = NULL;
        /* both of these are read out of the union, so confirm the type
         * the peer tagged each with - a mistyped PMIX_GROUP_ID would be
         * strcmp'd as a pointer built from a scalar, and a mistyped
         * affected proc dereferenced the same way */
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_ID)) {
                if (PMIX_STRING != cd->info[n].value.type) {
                    rc = PMIX_ERR_BAD_PARAM;
                    PMIX_ERROR_LOG(rc);
                    goto exit;
                }
                grpid = cd->info[n].value.data.string;
            } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_EVENT_AFFECTED_PROC)) {
                if (PMIX_PROC != cd->info[n].value.type) {
                    rc = PMIX_ERR_BAD_PARAM;
                    PMIX_ERROR_LOG(rc);
                    goto exit;
                }
                affected = cd->info[n].value.data.proc;
            }
        }
        if (NULL != grpid && NULL != affected) {
            pmix_server_grp_member_left(grpid, affected);
        }
    }

    /* check to see if we already processed this event - it is possible
     * that a local client "echoed" it back to us and we want to avoid
     * a potential infinite loop */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_SERVER_INTERNAL_NOTIFY)) {
            /* yep, we did - so don't do it again! */
            rc = PMIX_OPERATION_SUCCEEDED;
            goto exit;
        }
    }

    /* add an info object to mark that we recvd this internally */
    PMIX_INFO_LOAD(&cd->info[cd->ninfo - 1], PMIX_SERVER_INTERNAL_NOTIFY, NULL, PMIX_BOOL);

    /* process it */
    rc = pmix_server_notify_client_of_event(cd->status, &cd->source, cd->range, cd->info,
                                            cd->ninfo, intermed_step, cd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return rc;

exit:
    PMIX_RELEASE(cd);
    return rc;
}
