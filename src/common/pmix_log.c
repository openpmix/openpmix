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
#include "src/mca/plog/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    cb->status = status;
    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:log opcbfunc called");
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static void log_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                       pmix_buffer_t *buf, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    int32_t m;
    pmix_status_t rc, status;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:log log_cbfunc called");
    /* unpack the return status */
    m = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &m, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        status = rc;
    }

    if (NULL != cd->cbfunc.opcbfn) {
        cd->cbfunc.opcbfn(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT pmix_status_t PMIx_Log(const pmix_info_t data[], size_t ndata,
                                   const pmix_info_t directives[], size_t ndirs)
{
    pmix_cb_t cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "%s pmix:log",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    rc = PMIx_Log_nb(data, ndata, directives, ndirs, opcbfunc, &cb);
    if (PMIX_SUCCESS == rc) {
        /* wait for the operation to complete */
        PMIX_WAIT_THREAD(&cb.lock);
    } else {
        PMIX_DESTRUCT(&cb);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            rc = PMIX_SUCCESS;
        }
        return rc;
    }

    rc = cb.status;
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:log completed");

    return rc;
}

void pmix_log_local_op(int sd, short args, void *cbdata_)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata_;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* call down to process the request - the various components
     * will thread shift as required */
    rc = pmix_plog.log(cd->proc, cd->info, cd->ninfo, cd->directives, cd->ndirs);
    // need to execute the caller's cbfunc to release them
    if (NULL != cd->cbfunc.opcbfn) {
        cd->cbfunc.opcbfn(rc, cd->cbdata);
    }
    PMIX_PROC_FREE(cd->proc, 1);
    PMIX_RELEASE(cd);
}

static void localcbfn(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:log local callback");
    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(status, cb->cbdata);
    }
    PMIX_PROC_FREE(cb->proc, 1);
    cb->proc = NULL;
    PMIX_RELEASE(cb);
}

PMIX_EXPORT pmix_status_t PMIx_Log_nb(const pmix_info_t data[], size_t ndata,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata)

{
    pmix_cmd_t cmd = PMIX_LOG_CMD;
    pmix_buffer_t *msg;
    pmix_status_t rc = PMIX_SUCCESS;
    time_t timestamp = 0;
    pmix_proc_t *source;
    bool found = false;
    pmix_shift_caddy_t *cd;
    pmix_cb_t *cb;
    size_t n;

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:log non-blocking");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (0 == ndata || NULL == data) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    PMIX_PROC_CREATE(source, 1);

    /* check the directives - if they requested a timestamp, then
     * get the time, also look for a source */
    if (NULL != directives) {
        for (n = 0; n < ndirs; n++) {
            if (0 == strncmp(directives[n].key, PMIX_LOG_GENERATE_TIMESTAMP, PMIX_MAX_KEYLEN)) {
                if (PMIX_INFO_TRUE(&directives[n])) {
                    /* pickup the timestamp */
                    timestamp = time(NULL);
                }
            } else if (0 == strncmp(directives[n].key, PMIX_LOG_SOURCE, PMIX_MAX_KEYLEN)) {
                memcpy(source, directives[n].value.data.proc, sizeof(pmix_proc_t));
                found = true;
            }
        }
    }
    if (!found) {
        memcpy(source, &pmix_globals.myid, sizeof(pmix_proc_t));
    }

    /* if we are a client or tool, pass this request to our
     * server for execution unless we are not connected */
    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) ||
        PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {

        // if we are not connected, then locally process
        if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
            goto local;
        }
        PMIX_PROC_FREE(source, 1); // don't need this value

        // otherwise, we send to our server
        cd = PMIX_NEW(pmix_shift_caddy_t);
        cd->cbfunc.opcbfn = cbfunc;
        cd->cbdata = cbdata;
        msg = PMIX_NEW(pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_RELEASE(cd);
            return rc;
        }
        if (!PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, PMIX_MINOR_WILDCARD,
                                  PMIX_RELEASE_WILDCARD)) {
            /* provide the timestamp - zero will indicate
             * that it wasn't taken */
            PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &timestamp, 1, PMIX_TIME);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIX_RELEASE(cd);
                return rc;
            }
        }
        /* pack the number of data entries */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ndata, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_RELEASE(cd);
            return rc;
        }
        if (0 < ndata) {
            PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, data, ndata, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIX_RELEASE(cd);
                return rc;
            }
        }
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ndirs, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_RELEASE(cd);
            return rc;
        }
        if (0 < ndirs) {
            PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, directives, ndirs, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIX_RELEASE(cd);
                return rc;
            }
        }

        pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                            "pmix:log sending to server");
        PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, log_cbfunc, (void *) cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    /* get here if we are a server - if we are not a gateway, then
     * we don't handle this ourselves if the host support is available */
    if (!PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
        cb = PMIX_NEW(pmix_cb_t);
        cb->cbfunc.opfn = cbfunc;
        cb->cbdata = cbdata;
        cb->proc = source;
        if (NULL != pmix_host_server.log2) {
            rc = pmix_host_server.log2(cb->proc, data, ndata,
                                       directives, ndirs, localcbfn, (void *) cb);
            if (PMIX_SUCCESS != rc) {
                PMIX_PROC_FREE(source, 1);
                cb->proc = NULL;
                PMIX_RELEASE(cb);
                return rc;
            }

        } else if (NULL != pmix_host_server.log) {
            pmix_host_server.log(cb->proc, data, ndata, directives, ndirs,
                                 localcbfn, (void *) cb);
        } else {
            // no choice but to process locally
            goto local;
        }
        return PMIX_SUCCESS;
    }

local:
    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:log processing locally");
    // threadshift this into our own progress thread
    cd = PMIX_NEW(pmix_shift_caddy_t);
    cd->info = (pmix_info_t*)data;
    cd->ninfo = ndata;
    cd->directives = (pmix_info_t*)directives;
    cd->ndirs = ndirs;
    cd->cbfunc.opcbfn = cbfunc;
    cd->cbdata = cbdata;
    cd->proc = source;
    PMIX_THREADSHIFT(cd, pmix_log_local_op);

    return PMIX_SUCCESS;
}
