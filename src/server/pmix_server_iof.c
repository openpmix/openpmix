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

/* Standard I/O forwarding: the inbound handler for output arriving from
 * our host or another server, and the public APIs by which a host pushes
 * output at us and throttles it. */

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

#include "src/common/pmix_iof.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

void pmix_server_iof_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                            pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_proc_t source;
    pmix_iof_channel_t channel;
    pmix_byte_object_t bo;
    int32_t cnt;
    pmix_status_t rc;
    size_t refid, ninfo = 0;
    pmix_iof_req_t *req;
    pmix_info_t *info = NULL;

    PMIX_HIDE_UNUSED_PARAMS(hdr, cbdata);

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "recvd IOF with %d bytes from %s",
                        (int) buf->bytes_used,
                        PMIX_PNAME_PRINT(&peer->info->pname));

    /* if the buffer is empty, they are simply closing the socket */
    if (0 == buf->bytes_used) {
        return;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &channel, &cnt, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &refid, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* lookup the handler for this IOF package */
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, refid);
    if (NULL != req && NULL != req->cbfunc) {
        req->cbfunc(refid, channel, &source, &bo, info, ninfo);
    } else {
        /* otherwise, simply write it out to the specified std IO channel */
        if (NULL != bo.bytes && 0 < bo.size) {
            pmix_iof_write_output(&source, channel, &bo);
        }
    }

cleanup:
    /* cleanup the memory */
    if (0 < ninfo) {
        PMIX_INFO_FREE(info, ninfo);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
}

/* callback to receive job info */

static void _iofdeliver(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_iof_req_t *req;
    bool found = false;
    bool outputlocal;
    pmix_iof_cache_t *iof;
    int i;
    size_t n;
    /* the local write used to be unconditional and was what first set this;
     * skipping it must not leave the completion status undefined */
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "PMIX:SERVER delivering IOF from %s on channel %s with %d bytes",
                        PMIX_NAME_PRINT(cd->procs),
                        PMIx_IOF_channel_string(cd->channels),
                        (int)cd->bo->size);

    /* The host may be handing us output that is not ours to emit - a runtime
     * relaying another node's output to us solely because a tool attached
     * here asked for it is the case this exists for. Writing it out again
     * here would duplicate whatever the server that owns those processes
     * already wrote, and with output-to-file directives in play that means
     * two servers writing the same file. So let the host say so per
     * delivery, using the same attribute that says it at registration. */
    outputlocal = true;
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_IOF_LOCAL_OUTPUT)) {
            outputlocal = PMIX_INFO_TRUE(&cd->info[n]);
            break;
        }
    }

    /* output it locally if requested */
    if (outputlocal) {
        rc = pmix_iof_write_output(cd->procs, cd->channels, cd->bo);
        if (0 > rc) {
            goto done;
        }
    }

    /* cycle across our list of IOF requests and see who wants
     * this channel from this source */
    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL == req) {
            continue;
        }
        rc = pmix_iof_process_iof(cd->channels, cd->procs, cd->bo, cd->info, cd->ninfo, req);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* flag that we do have at least one registrant for this info,
             * so there is no need to cache it */
            found = true;
            rc = PMIX_SUCCESS;
        }
    }

    /* if nobody has registered for this yet, then cache it */
    if (!found) {
        pmix_output_verbose(2, pmix_server_globals.iof_output,
                            "PMIx:SERVER caching IOF %d",
                            (int)cd->bo->size);
        if (pmix_server_globals.max_iof_cache == pmix_list_get_size(&pmix_server_globals.iof)) {
            /* remove the oldest cached message */
            iof = (pmix_iof_cache_t *) pmix_list_remove_first(&pmix_server_globals.iof);
            if (NULL != iof) {
                PMIX_RELEASE(iof);
            }
        }
        /* add this output to our cache so it is cached until someone
         * registers to receive it */
        iof = PMIX_NEW(pmix_iof_cache_t);
        memcpy(&iof->source, cd->procs, sizeof(pmix_proc_t));
        iof->channel = cd->channels;
        /* copy the data */
        PMIX_BYTE_OBJECT_CREATE(iof->bo, 1);
        if (0 < cd->bo->size) {
            iof->bo->bytes = (char *) malloc(cd->bo->size);
            memcpy(iof->bo->bytes, cd->bo->bytes, cd->bo->size);
        }
        iof->bo->size = cd->bo->size;
        if (0 < cd->ninfo) {
            PMIX_INFO_CREATE(iof->info, cd->ninfo);
            iof->ninfo = cd->ninfo;
            for (n = 0; n < iof->ninfo; n++) {
                PMIX_INFO_XFER(&iof->info[n], &cd->info[n]);
            }
        }
        pmix_list_append(&pmix_server_globals.iof, &iof->super);
        rc = PMIX_SUCCESS;
    }

done:
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }

    /* release the caddy */
    cd->procs = NULL;
    cd->nprocs = 0;
    cd->info = NULL;
    cd->ninfo = 0;
    cd->bo = NULL;
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_IOF_deliver(const pmix_proc_t *source, pmix_iof_channel_t channel,
                                      const pmix_byte_object_t *bo, const pmix_info_t info[],
                                      size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->procs = (pmix_proc_t *) source;
    cd->nprocs = 1;
    cd->channels = channel;
    cd->bo = (pmix_byte_object_t *) bo;
    cd->info = (pmix_info_t *) info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_IOF_deliver")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _iofdeliver);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, _iofdeliver);
    return PMIX_SUCCESS;
}

static void _iofflowcontrol(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    rc = pmix_iof_flow_control(cd->procs, cd->channels, cd->xoff,
                               cd->info, cd->ninfo);

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }

    /* the caddy is only borrowing the caller's arrays */
    cd->procs = NULL;
    cd->nprocs = 0;
    cd->info = NULL;
    cd->ninfo = 0;
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_IOF_flow_control(const pmix_proc_t *source,
                                           pmix_iof_channel_t channel,
                                           bool xoff,
                                           const pmix_info_t directives[], size_t ndirs,
                                           pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* only stdin can be flow controlled - reject anything else here so
     * the caller finds out before we go anywhere near the event base */
    if (!(PMIX_FWD_STDIN_CHANNEL & channel)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* need to threadshift this request - it walks the client array and
     * the stdin read event, both of which belong to the progress thread */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->procs = (pmix_proc_t *) source;
    cd->nprocs = (NULL == source) ? 0 : 1;
    cd->channels = channel;
    cd->xoff = xoff;
    cd->info = (pmix_info_t *) directives;
    cd->ninfo = ndirs;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_IOF_flow_control")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _iofflowcontrol);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, _iofflowcontrol);
    return PMIX_SUCCESS;
}
