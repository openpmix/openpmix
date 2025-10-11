/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
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

#include "src/common/pmix_attributes.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

static void myinfocbfunc(pmix_status_t status,
                         pmix_info_t *info, size_t ninfo,
                         void *cbdata,
                         pmix_release_cbfunc_t release_fn,
                         void *release_cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, release_fn, release_cbdata);

    lock->status = status;
    PMIX_WAKEUP_THREAD(lock);
}

static void relcbfunc(void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:session_ctrl release callback");

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}
static void ssnctrlcbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_status_t rc;
    pmix_shift_caddy_t *results;
    int cnt;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:session ctrl cback from server");

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        /* release the caller */
        if (NULL != cd->cbfunc.infocbfunc) {
            cd->cbfunc.infocbfunc(PMIX_ERR_COMM_FAILURE, NULL, 0, cd->cbdata, NULL, NULL);
        }
        PMIX_RELEASE(cd);
        return;
    }

    results = PMIX_NEW(pmix_shift_caddy_t);

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &results->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (PMIX_SUCCESS != results->status) {
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
                        "pmix:session_ctrl cback from server releasing");
    /* release the caller */
    if (NULL != cd->cbfunc.infocbfunc) {
        cd->cbfunc.infocbfunc(results->status, results->info, results->ninfo, cd->cbdata, relcbfunc, results);
    } else {
        PMIX_RELEASE(results);
    }
    PMIX_RELEASE(cd);
}

static void _session_control(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_cmd_t cmd = PMIX_SESSION_CTRL_CMD;
    pmix_buffer_t *msg;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* if we are the system controller but not connected
     * to the scheduler, then nothing we can do */
    if (PMIX_PEER_IS_SYS_CTRLR(pmix_globals.mypeer)) {
        if (!PMIX_PEER_IS_SCHEDULER(pmix_client_globals.myserver)) {
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto errorrpt;
        }
        // otherwise send it to the scheduler
        goto sendit;
    }

sendit:
    /* for all other cases, we need to send this to someone
     *  if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        rc = PMIX_ERR_UNREACH;
        goto errorrpt;
    }

    /* all other cases, relay this request to our server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto errorrpt;
    }

    /* pack the sessionID */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cd->sessionid, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto errorrpt;
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cd->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto errorrpt;
    }
    if (0 < cd->ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cd->directives, cd->ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            goto errorrpt;
        }
    }

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, ssnctrlcbfunc, (void *) cd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
    }

    return;

errorrpt:
    /* release the caller */
    if (NULL != cd->cbfunc.infocbfunc) {
        cd->cbfunc.infocbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
    }
    PMIX_RELEASE(cd);
    return;
}

pmix_status_t PMIx_Session_control(uint32_t sessionID,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_shift_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server session control");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    cd = PMIX_NEW(pmix_shift_caddy_t);
    cd->sessionid = sessionID;
    cd->directives = (pmix_info_t*)directives;
    cd->ndirs = ndirs;
    cd->cbfunc.infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->cbfunc.infocbfunc = myinfocbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _session_control);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        PMIX_DESTRUCT_LOCK(&mylock);
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _session_control);
    return PMIX_SUCCESS;
}
