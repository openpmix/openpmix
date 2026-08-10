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
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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

    /* cache the data for any future requests */

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
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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

static void _cred_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:get credential callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }

    /* pack the status */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }

    if (PMIX_SUCCESS == scd->status) {
        /* pack the returned credential */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }

        /* pack any returned data */
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
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->bo) {
        PMIX_BYTE_OBJECT_FREE(scd->bo, 1);
    }
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

void pmix_server_cred_cbfunc(pmix_status_t status, pmix_byte_object_t *credential,
                             pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;
    size_t n;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:cred_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    /* this signature carries no release function, so nothing here is
     * ours past the return - both the credential and the info array
     * have to be copied before we hand them to another thread */
    if (NULL != info && 0 < ninfo) {
        PMIX_INFO_CREATE(scd->info, ninfo);
        if (NULL == scd->info) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            PMIX_RELEASE(cd);
            PMIX_RELEASE(qcd);
            PMIX_RELEASE(scd);
            return;
        }
        scd->ninfo = ninfo;
        /* the caddy owns the copy - flagging it here covers the early
         * returns that never reach _cred_cbfunc's cleanup */
        scd->infocopy = true;
        for (n=0; n < scd->ninfo; n++) {
            rc = PMIx_Info_xfer(&scd->info[n], &info[n]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(cd);
                PMIX_RELEASE(qcd);
                PMIX_RELEASE(scd);
                return;
            }
        }
    }
    PMIX_BFROPS_COPY(rc, cd->peer, (void**)&scd->bo, credential, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _cred_cbfunc);
}

static void _valcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:validate credential callback with status %s",
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
    /* pack any returned data */
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
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

void pmix_server_validate_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                 void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;
    size_t n;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:validate_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    // need to copy the info as they may not hold it for us
    if (NULL != info && 0 < ninfo) {
        PMIX_INFO_CREATE(scd->info, ninfo);
        if (NULL == scd->info) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            PMIX_RELEASE(cd);
            PMIX_RELEASE(qcd);
            PMIX_RELEASE(scd);
            return;
        }
        scd->ninfo = ninfo;
        /* the caddy owns the copy - flagging it here covers the early
         * returns that never reach _valcbfunc's cleanup */
        scd->infocopy = true;
        for (n=0; n < scd->ninfo; n++) {
            rc = PMIx_Info_xfer(&scd->info[n], &info[n]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(cd);
                PMIX_RELEASE(qcd);
                PMIX_RELEASE(scd);
                return;
            }
        }
    }
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _valcbfunc);
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
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
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
