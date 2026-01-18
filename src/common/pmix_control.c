/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
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

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix.h"
#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/mca/bfrops/bfrops.h"
#include "src/mca/pstat/pstat.h"
#include "src/mca/ptl/ptl.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

static void relcbfunc(void *cbdata)
{
    pmix_cb_t *cd = (pmix_cb_t *) cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:job_ctrl release callback");

    PMIX_RELEASE(cd);
}

static void query_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_status_t rc;
    pmix_cb_t *results;
    int cnt;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:job_ctrl cback from server with %d bytes",
                        (int) buf->bytes_used);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        /* release the caller */
        if (NULL != cd->cbfunc) {
            cd->cbfunc(PMIX_ERR_COMM_FAILURE, NULL, 0, cd->cbdata, NULL, NULL);
        }
        PMIX_RELEASE(cd);
        return;
    }

    results = PMIX_NEW(pmix_cb_t);

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &results->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (PMIX_SUCCESS != results->status &&
        PMIX_ERR_PARTIAL_SUCCESS != results->status) {
        goto complete;
    }

    /* unpack any returned data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &results->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < results->ninfo) {
        results->infocopy = true;
        PMIX_INFO_CREATE(results->info, results->ninfo);
        cnt = results->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, results->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
    }

complete:
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:job_ctrl cback from server releasing");
    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(results->status, results->info, results->ninfo, cd->cbdata, relcbfunc, results);
    } else {
        PMIX_RELEASE(results);
    }
    PMIX_RELEASE(cd);
}

static void acb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    size_t n;

    cb->status = status;
    if (0 < ninfo) {
        // we ignore the info that was provided as that data belongs
        // to the original caller
        cb->infocopy = true;
        PMIX_INFO_CREATE(cb->info, ninfo);
        cb->ninfo = ninfo;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cb->info[n], &info[n]);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    if (NULL == cb->cbfunc.infofn) {
        PMIX_WAKEUP_THREAD(&cb->lock);
    } else {
        cb->cbfunc.infofn(cb->status, cb->info, cb->ninfo, cb->cbdata,
                          relcbfunc, (void*)cb);
    }
}

PMIX_EXPORT pmix_status_t PMIx_Job_control(const pmix_proc_t targets[], size_t ntargets,
                                           const pmix_info_t directives[], size_t ndirs,
                                           pmix_info_t **results, size_t *nresults)
{
    pmix_cb_t cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_globals.debug_output, "%s pmix:job_ctrl",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    if (PMIX_SUCCESS
        != (rc = PMIx_Job_control_nb(targets, ntargets, directives, ndirs, acb, &cb))) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the operation to complete */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (0 < cb.ninfo) {
        if (NULL != results && NULL != nresults) {
            *results = cb.info;
            *nresults = cb.ninfo;
            cb.info = NULL;
            cb.ninfo = 0;
        }
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix:job_ctrl completed");

    return rc;
}

static void _jctrlnb(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    rc = pmix_host_server.job_control(&pmix_globals.myid, cb->procs, cb->nprocs,
                                      cb->info, cb->ninfo, acb, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        if (NULL != cb->cbfunc.infofn) {
            cb->cbfunc.infofn(rc, NULL, 0, cb->cbdata, relcbfunc, (void*)cb);
        } else {
            PMIX_RELEASE(cb);
        }
    }
}

PMIX_EXPORT pmix_status_t PMIx_Job_control_nb(const pmix_proc_t targets[], size_t ntargets,
                                              const pmix_info_t directives[], size_t ndirs,
                                              pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_JOB_CONTROL_CMD;
    pmix_status_t rc;
    pmix_query_caddy_t *qcd;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: job control called with %d directives",
                        (int) ndirs);

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* if we are the server, then we just issue the request and
     * return the response - must do it in our progress thread */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        if (NULL == pmix_host_server.job_control) {
            /* nothing we can do */
            return PMIX_ERR_NOT_SUPPORTED;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:job_control handed to RM");
        cb = PMIX_NEW(pmix_cb_t);
        cb->procs = (pmix_proc_t*)targets;
        cb->nprocs = ntargets;
        cb->info = (pmix_info_t*)directives;
        cb->ninfo = ndirs;
        cb->cbfunc.infofn = cbfunc;
        cb->cbdata = cbdata;
        PMIX_THREADSHIFT(cb, _jctrlnb);
        return PMIX_SUCCESS;
    }

    /* we need to send, so check for connection */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the number of targets */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ntargets, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* remember, the targets can be NULL to indicate that the operation
     * is to be done against all members of our nspace */
    if (NULL != targets && 0 < ntargets) {
        /* pack the targets */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, targets, ntargets, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (NULL != directives && 0 < ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, directives, ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    qcd = PMIX_NEW(pmix_query_caddy_t);
    qcd->cbfunc = cbfunc;
    qcd->cbdata = cbdata;

    /* threadshift the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, query_cbfunc, (void *) qcd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(qcd);
    }

    return rc;
}
