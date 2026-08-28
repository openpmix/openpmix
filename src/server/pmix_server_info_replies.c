/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Replies that hand host-supplied results back to the requester. These
 * all share one shape - the host returns an array of pmix_info_t (or a
 * credential or a set of device distances), we pack it and queue it to
 * the caller - so they are near-identical to each other by design: when
 * adding one, diff it against its neighbor here.
 *
 * These are callbacks the host server invokes, so they can run in either
 * the host's thread context or our own if the host answers immediately.
 * Anything touching a global entity is therefore pushed into an event
 * before it is used. The switchyard in pmix_server_switchyard.c is the
 * only caller that hands these to a host up-call. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

static void _alloccbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *)scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        if (NULL != qcd->queries) {
            PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
        }
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:alloc callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }


cleanup:
    // cleanup
    if (NULL != qcd->queries) {
        PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
    }
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }

    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(cd);
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(scd);
}

void pmix_server_alloc_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                              pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        if (NULL != qcd->queries) {
            PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
        }
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:alloc_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process. We cannot answer the requestor, but
         * we can at least let go of the caddies - and the peer reference
         * the server caddy holds - exactly as the arm above does. */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _alloccbfunc);
}

static void _qrycbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:query callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    // cleanup
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

void pmix_server_query_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                              pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:query_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process. We cannot answer the requestor, but
         * we can at least let go of the caddies - and the peer reference
         * the server caddy holds - exactly as the arm above does. */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _qrycbfunc);
}

static void _sctrl_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t*)cbdata;
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *)scdwrapper->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scdwrapper->cbfunc.relfn) {
            scdwrapper->cbfunc.relfn(scdwrapper->relcbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        PMIX_RELEASE(scdwrapper);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:session_ctrl callback with status %s",
                        PMIx_Error_string(scdwrapper->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scdwrapper->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scdwrapper->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scdwrapper->info, scdwrapper->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != scd->info) {
        PMIX_INFO_FREE(scd->info, scd->ninfo);
    }
    if (NULL != scdwrapper->cbfunc.relfn) {
        scdwrapper->cbfunc.relfn(scdwrapper->relcbdata);
    }
    PMIX_RELEASE(scd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scdwrapper);
}

void pmix_server_sessctrl_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                 void *cbdata, pmix_release_cbfunc_t release_fn,
                                 void *release_cbdata)
{
    pmix_shift_caddy_t *scdwrapper;
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:sessctrl_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scdwrapper = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scdwrapper) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process. We cannot answer the requestor, but
         * we can at least let go of the caddies - and the peer reference
         * the server caddy holds - exactly as the arm above does. */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }
    scdwrapper->status = status;
    scdwrapper->info = info;
    scdwrapper->ninfo = ninfo;
    scdwrapper->cbdata = cbdata;
    scdwrapper->cbfunc.relfn = release_fn;
    scdwrapper->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scdwrapper, _sctrl_cbfunc);
}

static void _jctrl_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:_jctrl_cbfunc callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->queries) {
        PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
    }
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }

    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

void pmix_server_jctrl_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                              pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:jctrl_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process. We cannot answer the requestor, but
         * we can at least let go of the caddies - and the peer reference
         * the server caddy holds - exactly as the arm above does. */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _jctrl_cbfunc);
}

static void _mon_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:_mon_cbfunc callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

void pmix_server_monitor_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                                pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:monitor_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process. We cannot answer the requestor, but
         * we can at least let go of the caddies - and the peer reference
         * the server caddy holds - exactly as the arm above does. */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _mon_cbfunc);
}

/* Pack a reply that carries nothing but its status.
 *
 * This is the whole of the answer to a request that failed, and it is
 * also the fallback when the payload of a successful one cannot be
 * packed: a body that was only half written is not something the client
 * can unpack, so the buffer is discarded and rebuilt to carry just the
 * failing status. */
static pmix_buffer_t *pack_status_reply(pmix_peer_t *peer, pmix_status_t status)
{
    pmix_buffer_t *reply;
    pmix_status_t rc;

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, peer, reply, &status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return NULL;
    }
    return reply;
}

/* Add an info array to a reply that already carries its status */
static pmix_status_t pack_reply_info(pmix_peer_t *peer, pmix_buffer_t *reply,
                                     pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;

    PMIX_BFROPS_PACK(rc, peer, reply, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, peer, reply, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    return rc;
}

/* Send the reply the credential and validation callbacks packed.
 *
 * pmix_credential_cbfunc_t and pmix_validation_cbfunc_t carry no
 * (release_fn, release_cbdata) pair, so nothing the host hands up
 * survives the return of the up-call - and the library cannot dispose of
 * that data itself, having no way to tell a host stack frame from
 * something malloc'd. The host therefore keeps ownership, and all the
 * library owes it is to be finished by the time the call returns. That
 * does not require a copy: both callbacks pack the reply on the host's
 * own thread, while its data is still valid, and shift only the finished
 * buffer. Packing is safe there - it writes into a buffer of our own and
 * reads the peer's bfrops module. Queueing is not, because it touches
 * the peer's send queue and the send event, so it is left here. */
static void _queue_sec_reply(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:security callback with status %s",
                        PMIx_Error_string(scd->status));

    if (!pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped) &&
        NULL != scd->buf) {
        PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, scd->buf);
        if (PMIX_SUCCESS == rc) {
            /* the send owns the buffer now */
            scd->buf = NULL;
        }
    }

    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    /* a buffer that was never queued goes back in the destructor */
    PMIX_RELEASE(scd);
}

void pmix_server_cred_cbfunc(pmix_status_t status, pmix_byte_object_t *credential,
                             pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:cred_cbfunc called");

    /* need to thread-shift the send as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* we cannot answer the requestor, but we can at least let go of
         * the caddies - and the peer reference the server caddy holds */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }
    scd->status = status;

    /* A host that could not issue a credential answers with an error
     * status and no credential - the contract on
     * pmix_credential_cbfunc_t says exactly that, and the credential is
     * packed only under a success status. The reverse, a success with
     * nothing to point at, would leave the client's unpack reading past
     * the end of the reply, so it is refused here.
     *
     * Every failure below records itself in scd->status and falls
     * through to the thread-shift rather than returning. This callback
     * is the ONLY thing that will ever answer the client: the handler
     * up-called the host and returned PMIX_SUCCESS, so the switchyard
     * has let go of the request and will synthesize nothing. Returning
     * here left the client blocked in PMIx_Get_credential for good. */
    if (PMIX_SUCCESS == scd->status && NULL == credential) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        scd->status = PMIX_ERR_BAD_PARAM;
    }

    scd->buf = pack_status_reply(cd->peer, scd->status);
    if (NULL != scd->buf && PMIX_SUCCESS == scd->status) {
        /* pack the credential the host gave us, and whatever it told us
         * about it - all of it read here, none of it kept */
        PMIX_BFROPS_PACK(rc, cd->peer, scd->buf, credential, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS == rc) {
            rc = pack_reply_info(cd->peer, scd->buf, info, ninfo);
        } else {
            PMIX_ERROR_LOG(rc);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(scd->buf);
            scd->status = rc;
            scd->buf = pack_status_reply(cd->peer, scd->status);
        }
    }

    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _queue_sec_reply);
}

void pmix_server_validate_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                 void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:validate_cbfunc called");

    /* need to thread-shift the send as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* we cannot answer the requestor, but we can at least let go of
         * the caddies - and the peer reference the server caddy holds */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }
    scd->status = status;

    /* as in pmix_server_cred_cbfunc, the host's array is packed here
     * rather than copied: it is ours only until we return, and a failure
     * records itself in scd->status and falls through to the shift so
     * that the client is answered either way */
    scd->buf = pack_status_reply(cd->peer, scd->status);
    if (NULL != scd->buf) {
        rc = pack_reply_info(cd->peer, scd->buf, info, ninfo);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(scd->buf);
            if (PMIX_SUCCESS == scd->status) {
                scd->status = rc;
            }
            scd->buf = pack_status_reply(cd->peer, scd->status);
        }
    }

    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _queue_sec_reply);
}

static void _fabcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:fabric callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->queries) {
        PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
    }
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(cd);
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(scd);
}

void pmix_server_fabric_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                               pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:fabric_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the caddies - and the peer reference the server caddy holds -
         * exactly as the progress-thread-stopped arm above does */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _fabcbfunc);
}

static void _distcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:device distances callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ndist, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ndist) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->dist, scd->ndist, PMIX_DEVICE_DIST);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

void pmix_server_dist_cbfunc(pmix_status_t status, pmix_device_distance_t *dist, size_t ndist,
                             void *cbdata, pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:dist_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the caddy - and the peer reference it holds - exactly as the
         * progress-thread-stopped arm above does */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->dist = dist;
    scd->ndist = ndist;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _distcbfunc);
}

static void _respeerscbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;
    pmix_value_t *val;
    pmix_proc_t *pa = NULL;
    size_t np = 0;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    ret = scd->status;
    if (PMIX_SUCCESS == ret) {
        // array return should be in first info
        if (0 == scd->ninfo) {
            // they didn't return anything
            ret = PMIX_ERR_NOT_FOUND;
            goto done;
        }
        val = &scd->info[0].value;
        /* this is a host-supplied structure, so check its shape before
         * indexing it - the array pointer is as much the host's to get
         * wrong as the type tag is */
        if (PMIX_DATA_ARRAY != val->type ||
            NULL == val->data.darray ||
            PMIX_PROC != val->data.darray->type) {
            PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
            ret = PMIX_ERR_INVALID_VAL;
            goto done;
        }
        pa = (pmix_proc_t*)val->data.darray->array;
        np = val->data.darray->size;
        if (NULL == pa) {
            np = 0;
        }
    } else {
        /* attempt to locally resolve the request */
        PMIX_THREADSHIFT(cd, pmix_server_locally_resolve_peers);
        // give the host its release
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

done:
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }

    if (PMIX_SUCCESS == ret) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &np, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
        if (0 < np) {
            PMIX_BFROPS_PACK(rc, cd->peer, reply, pa, np, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto complete;
            }
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

void pmix_server_respeers_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                 void *cbdata, pmix_release_cbfunc_t release_fn,
                                 void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:respeers_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the caddy - and the peer reference it holds */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _respeerscbfunc);
}

static void _resnodescbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;
    pmix_value_t *val;
    char *nodelist = NULL;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    ret = scd->status;
    if (PMIX_SUCCESS == ret) {
        // array return should be in first info
        if (0 == scd->ninfo) {
            // they didn't return anything
            ret = PMIX_ERR_NOT_FOUND;
            goto done;
        }
        val = &scd->info[0].value;
        if (PMIX_STRING != val->type) {
            PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
            ret = PMIX_ERR_INVALID_VAL;
            goto done;
        }
        nodelist = val->data.string;
    } else {
        /* attempt to locally resolve the request */
        PMIX_THREADSHIFT(cd, pmix_server_locally_resolve_node);
        // give the host its release
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

done:
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }

    if (PMIX_SUCCESS == ret) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &nodelist, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);

    // give the caller their release
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

void pmix_server_resnodes_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                 void *cbdata, pmix_release_cbfunc_t release_fn,
                                 void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is still the host's to reclaim, even here */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:resnodes_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the caddy - and the peer reference it holds */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _resnodescbfunc);
}
