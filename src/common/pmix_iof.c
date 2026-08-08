/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2021 IBM Corporation.  All rights reserved.
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

#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#else
#    ifdef HAVE_SYS_FCNTL_H
#        include <sys/fcntl.h>
#    endif
#endif
#include <ctype.h>

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix.h"
#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/mca/bfrops/bfrops.h"
#include "src/common/pmix_pfexec.h"
#include "src/mca/ptl/ptl.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_printf.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

static void myregcbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_iof_req_t *req = (pmix_iof_req_t *) cbdata;
    int32_t m;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    PMIX_ACQUIRE_OBJECT(req);

    /* a zero-byte buffer indicates that this recv is being completed
     * due to a lost connection - say so, rather than reporting the
     * unpack failure that reading an empty buffer produces */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        req->status = PMIX_ERR_COMM_FAILURE;
        goto report;
    }

    /* unpack the return status */
    m = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &req->status, &m, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        /* Ignore short buffer/premature connection disconnect */
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            req->status = PMIX_SUCCESS;
        } else {
            req->status = rc;
        }
    }

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "pmix:iof_register returned status %s",
                        PMIx_Error_string(req->status));

    if (PMIX_SUCCESS == req->status) {
        /* get the reference ID */
        m = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &req->remote_id, &m, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            req->status = rc;
        }
    }

report:
    if (PMIX_SUCCESS != req->status) {
        /* the registration did not take, so it must not be left in our
         * array where it would go on matching incoming IO. myreg's own
         * error paths do the same, and leave the release to whichever
         * side owns the request from here */
        pmix_pointer_array_set_item(&pmix_globals.iof_requests, req->local_id, NULL);
        if (NULL != req->regcbfunc) {
            req->regcbfunc(req->status, req->local_id, req->cbdata);
            PMIX_RELEASE(req);
        } else {
            PMIX_WAKEUP_THREAD(&req->lock);
        }
        return;
    }

    if (NULL == req->regcbfunc) {
        PMIX_WAKEUP_THREAD(&req->lock);
    } else {
        req->regcbfunc(req->status, req->local_id, req->cbdata);
    }
    return;
}

static void process_cache(int sd, short args, void *cbdata)
{
    pmix_iof_req_t *req = (pmix_iof_req_t *) cbdata;
    pmix_iof_cache_t *iof, *ionext;
    bool found;
    size_t n;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* a request registered by a client or a launcher that has an
     * upstream server carries no requestor peer - its IO is delivered
     * through its own callback when the message arrives, not by
     * packing one here - so there is nothing for us to forward */
    if (NULL == req->requestor) {
        return;
    }
    /* and nothing to forward to a peer that has no identity yet or has
     * already gone - the same two screens pmix_iof_process_iof applies
     * before it reaches into requestor->info */
    if (NULL == req->requestor->info || req->requestor->finalized) {
        return;
    }

    /* NOTE: the forwarding below is currently unreachable, and knowing
     * that is worth more than the code. The only requests that get here
     * are the ones PMIx_IOF_pull built in this process, and it sets
     * requestor only when we are our own server - in which case the
     * requestor IS us, so the "never forward to myself" test below skips
     * every entry. A request with no requestor returned above. So the
     * delayed cache scan the blocking form of PMIx_IOF_pull arms delivers
     * nothing: such a request is served through req->cbfunc when the IO
     * arrives, and cached IO has no path to that callback. Settle what
     * the right delivery is before making this live. */

    PMIX_LIST_FOREACH_SAFE (iof, ionext, &pmix_server_globals.iof, pmix_iof_cache_t) {
        /* if the channels don't match, then ignore it */
        if (!(iof->channel & req->channels)) {
            continue;
        }
        /* never forward back to the source! This can happen if the source
         * is a launcher */
        if (PMIX_CHECK_NAMES(&iof->source, &req->requestor->info->pname)) {
            continue;
        }
        /* never forward to myself */
        if (PMIX_CHECK_NAMES(&req->requestor->info->pname, &pmix_globals.myid)) {
            continue;
        }
        /* if the source does not match the request, then ignore it */
        found = false;
        for (n = 0; n < req->nprocs; n++) {
            if (PMIX_CHECK_PROCID(&iof->source, &req->procs[n])) {
                found = true;
                break;
            }
        }
        if (found) {
            /* setup the msg */
            if (NULL == (msg = PMIX_NEW(pmix_buffer_t))) {
                PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
                return;
            }
            /* provide the source */
            PMIX_BFROPS_PACK(rc, req->requestor, msg, &iof->source, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                return;
            }
            /* provide the channel */
            PMIX_BFROPS_PACK(rc, req->requestor, msg, &iof->channel, 1, PMIX_IOF_CHANNEL);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                return;
            }
            /* Provide the handler ID *the requestor* knows this request
             * by. The receiver uses it as an index into its own
             * iof_requests array, so it has to be remote_id - local_id is
             * our index into ours, and names a different request entirely.
             * pmix_iof_process_iof, which packs the identical message for
             * the identical receiver, gets this right. (Nothing reaches
             * this today - see the note below - which is how the two
             * drifted apart.) */
            PMIX_BFROPS_PACK(rc, req->requestor, msg, &req->remote_id, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                return;
            }
            /* pack the number of info's provided */
            PMIX_BFROPS_PACK(rc, req->requestor, msg, &iof->ninfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                return;
            }
            /* if some were provided, then pack them too */
            if (0 < iof->ninfo) {
                PMIX_BFROPS_PACK(rc, req->requestor, msg, iof->info, iof->ninfo, PMIX_INFO);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(msg);
                    return;
                }
            }
            /* pack the data */
            PMIX_BFROPS_PACK(rc, req->requestor, msg, iof->bo, 1, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                return;
            }
            /* send it to the requestor */
            PMIX_PTL_SEND_ONEWAY(rc, req->requestor, msg, PMIX_PTL_TAG_IOF);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
            }
        }
    }
}

static void myreg(int sd, short args, void *cbdata)
{
    pmix_iof_req_t *req = (pmix_iof_req_t *) cbdata;
    pmix_cmd_t cmd = PMIX_IOF_PULL_CMD;
    pmix_buffer_t *msg = NULL;
    pmix_status_t rc;
    int idx;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // store the request
    idx = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    if (0 > idx) {
        /* the array could not be grown, so the request was never stored -
         * there is no local id to hand back and nothing to remove. The
         * negative return must not be passed off as one: it lands in a
         * size_t, goes on the wire to the server as a huge value, and is
         * reported to the caller as the handle of a live registration */
        req->local_id = SIZE_MAX;
        req->status = PMIX_ERR_NOMEM;
        if (NULL != req->regcbfunc) {
            req->regcbfunc(req->status, req->local_id, req->cbdata);
            PMIX_RELEASE(req);
        } else {
            PMIX_WAKEUP_THREAD(&req->lock);
        }
        return;
    }
    req->local_id = (size_t) idx;
    req->status = PMIX_SUCCESS;

    if (NULL != req->requestor) {
        // local request as we are a server
        /* if there is a registration callback function, call it */
        if (NULL != req->regcbfunc) {
            req->regcbfunc(req->status, req->local_id, req->cbdata);
            // process any cached IO
            process_cache(0, 0, req);
        } else {
            PMIX_WAKEUP_THREAD(&req->lock);
        }
        return;
    }

    /* setup the registration cmd for the server */
    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL == msg) {
        pmix_pointer_array_set_item(&pmix_globals.iof_requests, req->local_id, NULL);
        req->status = PMIX_ERR_NOMEM;
        if (NULL != req->regcbfunc) {
            req->regcbfunc(req->status, req->local_id, req->cbdata);
            PMIX_RELEASE(req);
        } else {
            PMIX_WAKEUP_THREAD(&req->lock);
        }
        return;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &req->nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < req->nprocs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, req->procs, req->nprocs, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &req->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < req->ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, req->directives, req->ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &req->channels, 1, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &req->local_id, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "pmix:iof:PULL sending request to server");
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, myregcbfunc, (void *) req);
    if (PMIX_SUCCESS == rc) {
        return;
    }

cleanup:
    // handle an error
    if (NULL != msg) {
        PMIX_RELEASE(msg);
    }
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, req->local_id, NULL);
    req->status = rc;
    if (NULL != req->regcbfunc) {
        req->regcbfunc(req->status, req->local_id, req->cbdata);
        PMIX_RELEASE(req);
    } else {
        PMIX_WAKEUP_THREAD(&req->lock);
    }
    return;

}

PMIX_EXPORT pmix_status_t PMIx_IOF_pull(const pmix_proc_t procs[], size_t nprocs,
                                        const pmix_info_t directives[], size_t ndirs,
                                        pmix_iof_channel_t channel, pmix_iof_cbfunc_t cbfunc,
                                        pmix_hdlr_reg_cbfunc_t regcbfunc, void *regcbdata)
{
    pmix_iof_req_t *req;

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "pmix:iof:PULL");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* we don't allow stdin to flow thru this path */
    if (PMIX_FWD_STDIN_CHANNEL & channel) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* the sources are copied wholesale below, so there has to be an
     * array to copy from */
    if (0 == nprocs || NULL == procs) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* the blocking form posts to the progress thread and then waits on
     * it, so calling it FROM that thread - from an event handler, or
     * from the IOF delivery callback itself - is waiting for ourselves,
     * and the progress thread never runs again. Say so instead, exactly
     * as the server-side IOF entry points do */
    if (NULL == regcbfunc && pmix_progress_thread_check_blocking("PMIx_IOF_pull")) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    req = PMIX_NEW(pmix_iof_req_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }

    /* if I am my own active server, then just register
     * the request */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        pmix_client_globals.myserver == pmix_globals.mypeer) {
        // identify ourselves as the requestor
        PMIX_RETAIN(pmix_globals.mypeer);
        req->requestor = pmix_globals.mypeer;
    } else {
        // this is going to have to be sent to the server -
        // if we aren't connected, don't attempt to send
        if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
            PMIX_RELEASE(req);
            return PMIX_ERR_UNREACH;
        }
    }

    // need to threadshift this for processing
    req->nprocs = nprocs;
    PMIX_PROC_CREATE(req->procs, req->nprocs);
    memcpy(req->procs, procs, nprocs * sizeof(pmix_proc_t));
    req->channels = channel;
    req->directives = (pmix_info_t*)directives;
    req->ndirs = ndirs;
    req->cbfunc = cbfunc;
    req->regcbfunc = regcbfunc;
    req->cbdata = regcbdata;
    PMIX_THREADSHIFT(req, myreg);
    if (NULL == regcbfunc) {
        // blocking call, so wait here
        pmix_status_t status;
        PMIX_WAIT_THREAD(&req->lock);
        if (PMIX_SUCCESS == req->status) {
            // we don't want the IO to arrive before we return from
            // this call, so delay it a little
            PMIX_THREADSHIFT_DELAY(req, process_cache, 1.0);
            return PMIX_OPERATION_SUCCEEDED;
        } else {
            // the request failed and myreg removed it from the
            // iof_requests array without releasing it (the blocking
            // path only wakes us), so release it here to avoid a leak
            status = req->status;
            PMIX_RELEASE(req);
            return status;
        }
    }
    return PMIX_SUCCESS;
}

static void deregcbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t*)cbdata;
    int32_t m;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    PMIX_ACQUIRE_OBJECT(cd);

    /* a zero-byte buffer indicates that this recv is being completed
     * due to a lost connection - the local removal already happened in
     * mydereg, but say what actually became of the request */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        cd->status = PMIX_ERR_COMM_FAILURE;
        goto report;
    }

    /* unpack the return status */
    m = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->status, &m, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        /* Ignore short buffer/premature connection disconnect */
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            cd->status = PMIX_SUCCESS;
        } else {
            cd->status = rc;
        }
    }

report:

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "pmix:iof_deregister returned status %s",
                        PMIx_Error_string(cd->status));

    if (NULL == cd->cbfunc.opcbfn) {
        // release the blocking call
        PMIX_WAKEUP_THREAD(&cd->lock);
        return;
    }

    // give them their callback
    cd->cbfunc.opcbfn(cd->status, cd->cbdata);
    PMIX_RELEASE(cd);
}

static void mydereg(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t*)cbdata;
    pmix_cmd_t cmd = PMIX_IOF_DEREG_CMD;
    pmix_buffer_t *msg = NULL;
    pmix_status_t rc;
    pmix_iof_req_t *req;
    size_t remote_id;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, cd->ncodes);
    if (NULL == req) {
        /* bad value */
        rc = PMIX_ERR_BAD_PARAM;
        goto cleanup;
    }
    remote_id = req->remote_id;
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, cd->ncodes, NULL);
    PMIX_RELEASE(req);


    /* setup the deregistration cmd */
    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL == msg) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cd->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < cd->ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cd->directives, cd->ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* pack the remote handler ID */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &remote_id, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "pmix:iof_dereg sending to server");
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, deregcbfunc, (void *) cd);
    if (PMIX_SUCCESS == rc) {
        return;
    }

cleanup:
    // handle an error
    if (NULL != msg) {
        PMIX_RELEASE(msg);
    }
    if (NULL != cd->cbfunc.opcbfn) {
        cd->cbfunc.opcbfn(rc, cd->cbdata);
        PMIX_RELEASE(cd);
    } else {
        cd->status = rc;
        PMIX_WAKEUP_THREAD(&cd->lock);
    }
}

PMIX_EXPORT pmix_status_t PMIx_IOF_deregister(size_t iofhdlr, const pmix_info_t directives[],
                                              size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_shift_caddy_t *cd;
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "pmix:iof_deregister");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* if we are a server, we cannot do this */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    /* see the note on PMIx_IOF_pull - a tool that decides it has seen
     * enough output and deregisters from inside its own IOF callback is
     * calling this from the progress thread */
    if (NULL == cbfunc && pmix_progress_thread_check_blocking("PMIx_IOF_deregister")) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    // need to threadshift to access global data
    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->ncodes = iofhdlr;
    cd->directives = (pmix_info_t*)directives;
    cd->ndirs = ndirs;
    cd->cbfunc.opcbfn = cbfunc;
    cd->cbdata = cbdata;
    PMIX_THREADSHIFT(cd, mydereg);
    if (NULL == cbfunc) {
        // blocking call
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        PMIX_RELEASE(cd);
    }

    return rc;
}

static pmix_event_t stdinsig_ev;
static bool stdinsig_active = false;
static pmix_iof_read_event_t *stdinev_global = NULL;

/* Give back what the stdin machinery here holds process-wide.
 *
 * Both of these live for the life of the library rather than of a
 * request, so nothing else ever released them. The read event was simply
 * leaked; worse, a second PMIx_Init would build another one over the top
 * of the pointer while the first still named an event registered on the
 * base the previous cycle tore down - which anything reaching
 * stdinev_global in between (a flow-control request, say) would walk.
 *
 * Called from pmix_rte_finalize, which runs it while the event base is
 * still standing - deleting these afterwards would be the very
 * use-after-free it exists to prevent.
 */
void pmix_iof_finalize(void)
{
    if (stdinsig_active) {
        pmix_event_del(&stdinsig_ev);
        stdinsig_active = false;
    }
    if (NULL != stdinev_global) {
        PMIX_RELEASE(stdinev_global);
        stdinev_global = NULL;
    }
}

/* Start reading our own stdin into stdinev_global, arming the SIGCONT
 * handler when we are looking at a terminal.
 *
 * Both of the roles that forward their own stdin come through here:
 * PMIx_IOF_push with PMIX_IOF_PUSH_STDIN, and a tool told PMIX_FWD_STDIN
 * at init. They used to keep a copy each and the copies drifted - the
 * tool's never added its SIGCONT event (so a backgrounded tool never
 * resumed reading), never gave its read event back, and kept it in a
 * file-scope struct of its own that pmix_iof_flow_control cannot see, so
 * nothing could suspend a tool's stdin. Sharing stdinev_global also
 * enforces the thing that matters in its own right: one read event per
 * descriptor. Two of them on fd 0 steal bytes from each other.
 */
pmix_status_t pmix_iof_setup_stdin_read(int fd, pmix_proc_t procs[], size_t nprocs,
                                        pmix_info_t directives[], size_t ndirs)
{
    int flags;

    if (NULL != stdinev_global) {
        /* we are already reading it - see above */
        return PMIX_SUCCESS;
    }
    pmix_globals.pushstdin = true;

    /* We don't want to set nonblocking on our
     * stdio stream.  If we do so, we set the file descriptor to
     * non-blocking for everyone that has that file descriptor, which
     * includes everyone else in our shell pipeline chain.  (See
     * http://lists.freebsd.org/pipermail/freebsd-hackers/2005-January/009742.html).
     * This causes things like "prun -np 1 big_app | cat" to lose
     * output, because cat's stdout is then ALSO non-blocking and cat
     * isn't built to deal with that case (same with almost all other
     * unix text utils).
     */
    if (0 != fd) {
        if ((flags = fcntl(fd, F_GETFL, 0)) < 0) {
            pmix_output(pmix_client_globals.iof_output,
                        "[%s:%d]: fcntl(F_GETFL) failed with errno=%d\n",
                        __FILE__, __LINE__, errno);
        } else {
            flags |= O_NONBLOCK;
            if (0 != fcntl(fd, F_SETFL, flags)) {
                pmix_output(pmix_client_globals.iof_output,
                            "[%s:%d]: fcntl(F_SETFL) failed with errno=%d\n",
                            __FILE__, __LINE__, errno);
            }
        }
    }

    if (isatty(fd)) {
        /* We should avoid trying to read from stdin if we
         * have a terminal, but are backgrounded.  Catch the
         * signals that are commonly used when we switch
         * between being backgrounded and not.  If the
         * filedescriptor is not a tty, don't worry about it
         * and always stay connected.
         *
         * The handler resolves and manipulates stdinev_global and its
         * libevent registration, both of which belong to evbase - so the
         * signal event goes there too rather than on evauxbase, which a
         * host may have supplied as a separate thread. And it has to be
         * *added*: setting a signal event only assigns it.
         */
        pmix_event_signal_set(pmix_globals.evbase, &stdinsig_ev, SIGCONT,
                              pmix_iof_stdin_cb, NULL);
        if (0 == pmix_event_add(&stdinsig_ev, NULL)) {
            stdinsig_active = true;
        }

        /* setup a read event to read stdin, but don't activate it yet. The
         * dst_name indicates who should receive the stdin. If that recipient
         * doesn't do a corresponding pull, however, then the stdin will
         * be dropped upon receipt at the local daemon
         */
        PMIX_IOF_READ_EVENT(&stdinev_global, procs, nprocs, directives, ndirs, fd,
                            pmix_iof_read_local_handler, false);

        /* check to see if we want the stdin read event to be
         * active - we will always at least define the event,
         * but may delay its activation
         */
        if (pmix_iof_stdin_check(fd)) {
            PMIX_IOF_READ_ACTIVATE(stdinev_global);
        }
    } else {
        /* if we are not looking at a tty, just setup a read event
         * and activate it
         */
        PMIX_IOF_READ_EVENT(&stdinev_global, procs, nprocs, directives, ndirs, fd,
                            pmix_iof_read_local_handler, true);
    }
    return PMIX_SUCCESS;
}

static void stdincbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    int cnt;
    pmix_status_t rc, status;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        /* release the caller */
        if (NULL != cb->cbfunc.opfn) {
            cb->cbfunc.opfn(PMIX_ERR_COMM_FAILURE, cb->cbdata);
            PMIX_RELEASE(cb);
        } else {
            PMIX_WAKEUP_THREAD(&cb->lock);
        }
        return;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        status = rc;
    }
    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(status, cb->cbdata);
        PMIX_RELEASE(cb);
    } else {
        cb->status = status;
        PMIX_WAKEUP_THREAD(&cb->lock);
    }
}

static void mysrvrcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;

    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(status, cb->cbdata);
        PMIX_RELEASE(cb);
    } else {
        cb->status = status;
        PMIX_WAKEUP_THREAD(&cb->lock);
    }
}

static void exec_push(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_IOF_PUSH_CMD;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n;
    bool begincollecting, stopcollecting;
    int fd = fileno(stdin);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL == cb->bo) {
        /* check the directives */
        for (n = 0; n < cb->ndirs; n++) {
            if (PMIX_CHECK_KEY(&cb->directives[n], PMIX_IOF_PUSH_STDIN)) {
                /* we are to start collecting our stdin and pushing
                 * it to the specified targets */
                begincollecting = PMIX_INFO_TRUE(&cb->directives[n]);
                if (begincollecting) {
                    /* add these targets to our list */
                    if (!pmix_globals.pushstdin) {
                        /* not already collecting, so start */
                        pmix_iof_setup_stdin_read(fd, cb->procs, cb->nprocs,
                                                  cb->directives, cb->ndirs);
                    }
                } else {
                    if (pmix_globals.pushstdin) {
                        /* remove these targets from the list of
                         * recipients - if the list is then empty,
                         * stop collecting. If the targets param
                         * is NULL, then remove all targets and stop.
                         * Flush any cached input before calling
                         * the cbfunc */
                    }
                }
            } else if (PMIX_CHECK_KEY(&cb->directives[n], PMIX_IOF_COMPLETE)) {
                /* if we are collecting our stdin for the specified
                 * targets, then stop - a NULL for targets indicates
                 * stop for everyone. Flush any remaining cached input
                 * before calling the cbfunc */
                stopcollecting = PMIX_INFO_TRUE(&cb->directives[n]);
                if (stopcollecting) {
                    if (pmix_globals.pushstdin) {
                        /* remove these targets from the list of
                         * recipients - if the list is then empty,
                         * stop collecting */
                    }
                }
            }
        }
        if (NULL != cb->cbfunc.opfn) {
            cb->cbfunc.opfn(PMIX_SUCCESS, cb->cbdata);
            PMIX_RELEASE(cb);
        } else {
            cb->status = PMIX_SUCCESS;
            PMIX_WAKEUP_THREAD(&cb->lock);
        }
        return;
    }

    /* if we are a server and not a launcher, just pass the data up to our host */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        if (NULL == pmix_host_server.push_stdin) {
            rc = PMIX_ERR_NOT_SUPPORTED;
        } else {
            rc = pmix_host_server.push_stdin(&pmix_globals.myid, cb->procs, cb->nprocs,
                                             cb->directives, cb->ndirs, cb->bo,
                                             mysrvrcbfunc, cb);
        }
        if (PMIX_SUCCESS != rc) {
            if (NULL != cb->cbfunc.opfn) {
                cb->cbfunc.opfn(rc, cb->cbdata);
                PMIX_RELEASE(cb);
            } else {
                cb->status = rc;
                PMIX_WAKEUP_THREAD(&cb->lock);
            }
        }
        return;
    }

    /* if we are not a server, then we send the provided
     * data to our server for processing */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        rc = PMIX_ERR_UNREACH;
        goto reply;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL == msg) {
        rc = PMIX_ERR_NOMEM;
        goto reply;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto reply;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cb->nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto reply;
    }
    if (0 < cb->nprocs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cb->procs, cb->nprocs, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            goto reply;
        }
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cb->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto reply;
    }
    if (0 < cb->ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cb->directives, cb->ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            goto reply;
        }
    }
    if (NULL != cb->bo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cb->bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            goto reply;
        }
    }

    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, stdincbfunc, cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto reply;
    }
    return;

reply:
    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(rc, cb->cbdata);
    } else {
        cb->status = rc;
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }
    PMIX_RELEASE(cb);
    return;

}

PMIX_EXPORT pmix_status_t PMIx_IOF_push(const pmix_proc_t targets[], size_t ntargets, pmix_byte_object_t *bo,
                            const pmix_info_t directives[], size_t ndirs, pmix_op_cbfunc_t cbfunc,
                            void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* see the note on PMIx_IOF_pull */
    if (NULL == cbfunc && pmix_progress_thread_check_blocking("PMIx_IOF_push")) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    // need to threadshift this request to process it since it accesses
    // global structures
    cb = PMIX_NEW(pmix_cb_t);
    if (NULL == cb) {
        return PMIX_ERR_NOMEM;
    }
    cb->procs = (pmix_proc_t*)targets;
    cb->nprocs = ntargets;
    cb->bo = bo;
    cb->directives = (pmix_info_t*)directives;
    cb->ndirs = ndirs;
    cb->cbfunc.opfn = cbfunc;
    cb->cbdata = cbdata;
    PMIX_THREADSHIFT(cb, exec_push);
    if (NULL == cbfunc) {
        PMIX_WAIT_THREAD(&cb->lock);
        rc = cb->status;
        PMIX_RELEASE(cb);
        return rc;
    } else {
        // the implementation will callback the cbfunc with the answer
        return PMIX_SUCCESS;
    }

}

/*
 * Grow the accumulated name, replacing it with the formatted result and
 * releasing the old buffer. The format is expected to lead with a "%s" fed
 * by the current *out - i.e. it appends. On allocation failure the
 * accumulator is freed and set to NULL, so a caller need only test the
 * return value to know it owns nothing.
 */
static bool append_to_pattern(char **out, const char *fmt, ...)
__pmix_attribute_format__(__printf__, 2, 3);

static bool append_to_pattern(char **out, const char *fmt, ...)
{
    char *tmp = NULL;
    va_list ap;

    va_start(ap, fmt);
    /* pmix_vasprintf guarantees tmp is NULL on error */
    pmix_vasprintf(&tmp, fmt, ap);
    va_end(ap);

    free(*out);
    *out = tmp;
    return (NULL != tmp);
}

/*
 * Expand the '%' conversions in an output-file pattern - see the contract
 * on pmix_iof_check_pattern/pmix_iof_expand_pattern in pmix_iof.h.
 *
 * Both entry points share this one walker so a pattern can never be
 * accepted by the check and then rejected (or, worse, expanded differently)
 * when the file is actually opened. With expand == false it only validates,
 * which is why nspace/rank/numdigs may be absent in that mode.
 */
static pmix_status_t process_pattern(const char *pattern, bool expand,
                                     const char *nspace, pmix_rank_t rank,
                                     int numdigs, const char *suffix,
                                     char **result, char **bad)
{
    const char *p;
    char *out = NULL;
    char conv[3];

    if (NULL != bad) {
        *bad = NULL;
    }
    if (NULL != result) {
        *result = NULL;
    }
    if (NULL == pattern) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* validation mode has no answer to hand back, but expansion does -
     * refuse rather than expand a name into nowhere */
    if (expand && NULL == result) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (expand) {
        out = strdup("");
        if (NULL == out) {
            return PMIX_ERR_NOMEM;
        }
    }

    for (p = pattern; '\0' != *p; ++p) {
        if ('%' != *p) {
            if (expand) {
                if (!append_to_pattern(&out, "%s%c", out, *p)) {
                    return PMIX_ERR_NOMEM;
                }
            }
            continue;
        }
        ++p;
        switch (*p) {
            case 'n':
                if (expand) {
                    if (!append_to_pattern(&out, "%s%s", out,
                                           (NULL == nspace) ? "" : nspace)) {
                        return PMIX_ERR_NOMEM;
                    }
                }
                break;
            case 'r':
                if (expand) {
                    if (!append_to_pattern(&out, "%s%u", out, rank)) {
                        return PMIX_ERR_NOMEM;
                    }
                }
                break;
            case 'R':
                if (expand) {
                    if (!append_to_pattern(&out, "%s%0*u", out, numdigs, rank)) {
                        return PMIX_ERR_NOMEM;
                    }
                }
                break;
            case 'h':
                if (expand) {
                    if (!append_to_pattern(&out, "%s%s", out,
                                           (NULL == pmix_globals.hostname)
                                               ? "" : pmix_globals.hostname)) {
                        return PMIX_ERR_NOMEM;
                    }
                }
                break;
            case '%':
                if (expand) {
                    if (!append_to_pattern(&out, "%s%%", out)) {
                        return PMIX_ERR_NOMEM;
                    }
                }
                break;
            default:
                /* a trailing '%' leaves *p at the terminator - report the
                 * bare '%' rather than reading past the end of the string */
                conv[0] = '%';
                conv[1] = *p;
                conv[2] = '\0';
                if ('\0' == *p) {
                    conv[1] = '\0';
                }
                if (NULL != bad) {
                    *bad = strdup(conv);
                }
                if (NULL != out) {
                    free(out);
                }
                return PMIX_ERR_BAD_PARAM;
        }
    }

    if (expand) {
        if (NULL != suffix) {
            if (!append_to_pattern(&out, "%s%s", out, suffix)) {
                return PMIX_ERR_NOMEM;
            }
        }
        *result = out;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_iof_check_pattern(const char *pattern, char **bad)
{
    return process_pattern(pattern, false, NULL, 0, 0, NULL, NULL, bad);
}

pmix_status_t pmix_iof_expand_pattern(const char *pattern, const char *nspace,
                                      pmix_rank_t rank, int numdigs,
                                      const char *suffix, char **result)
{
    return process_pattern(pattern, true, nspace, rank, numdigs, suffix, result, NULL);
}

static pmix_iof_write_event_t* pmix_iof_setup(pmix_namespace_t *nptr,
                                              pmix_rank_t rank,
                                              pmix_iof_channel_t stream)
{
    int rc;
    char *outdir, *outfile;
    int np, numdigs, fdout;
    pmix_iof_sink_t *snk;
    pmix_proc_t src;

    pmix_output_verbose(5, pmix_server_globals.iof_output,
                        "IOF SETUP %s %u",
                        nptr->nspace, rank);
    PMIX_LOAD_PROCID(&src, nptr->nspace, rank);

    np = nptr->nprocs / 10;
    /* determine the number of digits required for max vpid */
    numdigs = 1;
    while (np > 0) {
        numdigs++;
        np = np / 10;
    }

    /* see if we are to output to a directory */
    if (NULL != nptr->iof_flags.directory) {
        /* construct the directory where the output files will go */
        pmix_asprintf(&outdir, "%s/%s/rank.%0*u", nptr->iof_flags.directory,
                      nptr->nspace, numdigs, rank);
        /* ensure the directory exists. An existing directory is fine:
         * the job may legitimately be writing into one that is already
         * there (a rerun, or another rank that got here first) */
        rc = pmix_os_dirpath_create(outdir, S_IRWXU | S_IRGRP | S_IXGRP);
        if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
            PMIX_ERROR_LOG(rc);
            free(outdir);
            return NULL;
        }
        if (PMIX_FWD_STDOUT_CHANNEL & stream ||
            nptr->iof_flags.merge) {
            /* setup the stdout sink */
            pmix_asprintf(&outfile, "%s/stdout", outdir);
            fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
                free(outdir);
                return NULL;
            }
            /* define a sink to that file descriptor */
            snk = PMIX_NEW(pmix_iof_sink_t);
            if (nptr->iof_flags.merge) {
                PMIX_IOF_SINK_DEFINE(snk, &src, fdout,
                                     PMIX_FWD_ALL_CHANNELS, pmix_iof_write_handler);
            } else {
                PMIX_IOF_SINK_DEFINE(snk, &src, fdout,
                                     PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
            }
            pmix_list_append(&nptr->sinks, &snk->super);
            free(outdir);
            return &snk->wev;
        } else {
            /* setup the stderr sink */
            pmix_asprintf(&outfile, "%s/stderr", outdir);
            fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
                free(outdir);
                return NULL;
            }
            /* define a sink to that file descriptor. It is tagged with
             * stddiag as well as stderr because that is where stddiag is
             * written (see pmix_iof_write_output): a sink that claims only
             * stderr is never matched by the stddiag lookup, so every
             * stddiag chunk would build another sink - re-opening this same
             * file O_TRUNC and discarding what is already in it */
            snk = PMIX_NEW(pmix_iof_sink_t);
            PMIX_IOF_SINK_DEFINE(snk, &src, fdout,
                                 PMIX_FWD_STDERR_CHANNEL | PMIX_FWD_STDDIAG_CHANNEL,
                                 pmix_iof_write_handler);
            pmix_list_append(&nptr->sinks, &snk->super);
            free(outdir);
            return &snk->wev;
        }
    }

    /* see if we are to output to a file */
    if (NULL != nptr->iof_flags.file) {
        if (PMIX_FWD_STDOUT_CHANNEL & stream ||
            nptr->iof_flags.merge) {
            /* setup the stdout sink */
            if (nptr->iof_flags.pattern) {
                /* the name is the caller's to compose - expand its
                 * conversions and annotate it with nothing but the stream */
                rc = pmix_iof_expand_pattern(nptr->iof_flags.file, nptr->nspace,
                                             rank, numdigs, ".out", &outfile);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return NULL;
                }
            } else {
                /* setup the file */
                pmix_asprintf(&outfile, "%s.%s.%0*u.out", nptr->iof_flags.file,
                              nptr->nspace, numdigs, rank);
            }
            /* pmix_asprintf leaves the pointer NULL when it fails, and
             * everything below this point takes the name apart */
            if (NULL == outfile) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                return NULL;
            }
            /* ensure the directory the file lands in exists.  This is taken
             * from the FINAL name, not from the name we were handed: a
             * pattern may put conversions in the directory part
             * ("%h/rank-%R"), and creating the raw name's dirname would
             * create a directory literally called "%h" and then fail to open
             * the file in the one the pattern actually named. */
            outdir = pmix_dirname(outfile);
            rc = pmix_os_dirpath_create(outdir, S_IRWXU | S_IRGRP | S_IXGRP);
            free(outdir);
            if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
                PMIX_ERROR_LOG(rc);
                free(outfile);
                return NULL;
            }
            fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
                return NULL;
            }
            /* define a sink to that file descriptor */
            snk = PMIX_NEW(pmix_iof_sink_t);
            if (nptr->iof_flags.merge) {
                PMIX_IOF_SINK_DEFINE(snk, &src, fdout,
                                     PMIX_FWD_ALL_CHANNELS, pmix_iof_write_handler);
            } else {
                PMIX_IOF_SINK_DEFINE(snk, &src, fdout,
                                     PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
            }
            pmix_list_append(&nptr->sinks, &snk->super);
            return &snk->wev;
        } else {
            /* setup the stderr sink */
            if (nptr->iof_flags.pattern) {
                rc = pmix_iof_expand_pattern(nptr->iof_flags.file, nptr->nspace,
                                             rank, numdigs, ".err", &outfile);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return NULL;
                }
            } else {
                /* setup the file */
                pmix_asprintf(&outfile, "%s.%s.%0*u.err", nptr->iof_flags.file,
                              nptr->nspace, numdigs, rank);
            }
            if (NULL == outfile) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                return NULL;
            }
            /* see the note on the stdout sink above */
            outdir = pmix_dirname(outfile);
            rc = pmix_os_dirpath_create(outdir, S_IRWXU | S_IRGRP | S_IXGRP);
            free(outdir);
            if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
                PMIX_ERROR_LOG(rc);
                free(outfile);
                return NULL;
            }
            fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
                return NULL;
            }
            /* see the note on the directory sink above for why stddiag is
             * tagged here too */
            snk = PMIX_NEW(pmix_iof_sink_t);
            PMIX_IOF_SINK_DEFINE(snk, &src, fdout,
                                 PMIX_FWD_STDERR_CHANNEL | PMIX_FWD_STDDIAG_CHANNEL,
                                 pmix_iof_write_handler);
            pmix_list_append(&nptr->sinks, &snk->super);
            return &snk->wev;
        }
    }

    return NULL;
}

void pmix_iof_init_flags(pmix_iof_flags_t *flags)
{
    memset(flags, 0, sizeof(pmix_iof_flags_t));
    flags->nocopy = true;
}

void pmix_iof_check_flags(pmix_info_t *info, pmix_iof_flags_t *flags)
{
    if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_OUTPUT) ||
        PMIX_CHECK_KEY(info, PMIX_TAG_OUTPUT)) {
        flags->tag = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_DETAILED_OUTPUT)) {
        flags->tag_detailed = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TAG_FULLNAME_OUTPUT)) {
        flags->tag_fullname = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_RANK_OUTPUT)) {
        flags->rank = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_TIMESTAMP_OUTPUT) ||
               PMIX_CHECK_KEY(info, PMIX_TIMESTAMP_OUTPUT)) {
        flags->timestamp = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_XML_OUTPUT)) {
        flags->xml = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_OUTPUT_TO_FILE) ||
               PMIX_CHECK_KEY(info, PMIX_OUTPUT_TO_FILE)) {
        /* the value's type is under the caller's control - a name that
         * is not a string must be ignored rather than strdup'd from
         * whatever else shares the union */
        if (PMIX_STRING == info->value.type && NULL != info->value.data.string) {
            /* this walks an array the caller composed, so the key can
             * appear more than once - the last one wins, but only if the
             * one before it is given back first */
            if (NULL != flags->file) {
                free(flags->file);
            }
            flags->file = strdup(info->value.data.string);
            flags->set = true;
            flags->local_output = true;
            flags->local_output_given = true;
        }
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_OUTPUT_TO_DIRECTORY) ||
               PMIX_CHECK_KEY(info, PMIX_OUTPUT_TO_DIRECTORY)) {
        if (PMIX_STRING == info->value.type && NULL != info->value.data.string) {
            /* see the note on the file case above */
            if (NULL != flags->directory) {
                free(flags->directory);
            }
            flags->directory = strdup(info->value.data.string);
            flags->set = true;
            flags->local_output = true;
            flags->local_output_given = true;
        }
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_FILE_ONLY) ||
               PMIX_CHECK_KEY(info, PMIX_OUTPUT_NOCOPY)) {
        flags->nocopy = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_MERGE_STDERR_STDOUT) ||
               PMIX_CHECK_KEY(info, PMIX_MERGE_STDERR_STDOUT)) {
        flags->merge = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_LOCAL_OUTPUT)) {
        flags->local_output = PMIX_INFO_TRUE(info);
        flags->set = true;
        flags->local_output_given = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_OUTPUT_RAW)) {
        flags->raw = PMIX_INFO_TRUE(info);
        flags->set = true;
    } else if (PMIX_CHECK_KEY(info, PMIX_IOF_FILE_PATTERN)) {
        flags->pattern = PMIX_INFO_TRUE(info);
        /* don't mark as set here as this is just a qualifier */
    }
}

pmix_status_t pmix_iof_process_iof(pmix_iof_channel_t channels, const pmix_proc_t *source,
                                   const pmix_byte_object_t *bo, const pmix_info_t *info,
                                   size_t ninfo, pmix_iof_req_t *req)
{
    bool match;
    size_t m;
    pmix_buffer_t *msg;
    pmix_status_t rc;

    /* if the channel wasn't included, then ignore it */
    if (!(channels & req->channels)) {
        return PMIX_SUCCESS;
    }
    /* see if the source matches the request */
    match = false;
    for (m = 0; m < req->nprocs; m++) {
        if (PMIX_CHECK_PROCID(source, &req->procs[m])) {
            match = true;
            break;
        }
    }
    if (!match) {
        return PMIX_SUCCESS;
    }
    /* a request registered locally by a client - or by a launcher that
     * has an upstream server of its own - has no requestor peer to send
     * to; its IO reaches it through its own callback instead. The
     * server's request array holds both kinds */
    if (NULL == req->requestor) {
        return PMIX_SUCCESS;
    }
    /* never forward back to the source! This can happen if the source
     * is a launcher - also, never forward to a peer that is no
     * longer with us */
    if (NULL == req->requestor->info || req->requestor->finalized) {
        return PMIX_SUCCESS;
    }
    if (PMIX_CHECK_NAMES(source, &req->requestor->info->pname)) {
        return PMIX_SUCCESS;
    }
    /* never forward to myself */
    if (PMIX_CHECK_NAMES(&req->requestor->info->pname, &pmix_globals.myid)) {
        return PMIX_SUCCESS;
    }

    /* setup the msg */
    if (NULL == (msg = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    /* provide the source */
    PMIX_BFROPS_PACK(rc, req->requestor, msg, source, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* provide the channel */
    PMIX_BFROPS_PACK(rc, req->requestor, msg, &channels, 1, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* provide their local handler ID so they know which cbfunc to use */
    PMIX_BFROPS_PACK(rc, req->requestor, msg, &req->remote_id, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* pack the number of info's provided */
    PMIX_BFROPS_PACK(rc, req->requestor, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* if some were provided, then pack them too */
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, req->requestor, msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* pack the data */
    PMIX_BFROPS_PACK(rc, req->requestor, msg, bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* send it to the requestor */
    PMIX_PTL_SEND_ONEWAY(rc, req->requestor, msg, PMIX_PTL_TAG_IOF);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
    }
    return PMIX_OPERATION_SUCCEEDED;
}

pmix_byte_object_t* pmix_iof_prep_output(const pmix_proc_t *name,
                                         pmix_iof_flags_t *myflags,
                                         pmix_iof_channel_t stream,
                                         const pmix_byte_object_t *bo)
{
    char starttag[PMIX_IOF_BASE_TAG_MAX], endtag[PMIX_IOF_BASE_TAG_MAX], *suffix;
    char timestamp[PMIX_IOF_BASE_TAG_MAX], outtag[PMIX_IOF_BASE_TAG_MAX];
    char begintag[PMIX_IOF_BASE_TAG_MAX];
    char **segments = NULL;
    pmix_byte_object_t *output;
    size_t offset, j, n, m, bufsize;
    char *buffer, qprint[15], *cptr;
    unsigned char uc;
    const char *usestring;
    bool bufcopy;
    pmix_cb_t cb2;
    pmix_info_t optional;
    pmix_kval_t *kv;
    pid_t pid;
    char *pidstring;
    pmix_status_t rc;

    /* setup output object */
    output = PMIx_Byte_object_create(1);

    /* if 0 bytes, then just pass it so the fd can be closed
     * after it writes everything out
     */
    if (0 == bo->size) {
        // the byte object was initialized to empty
        return output;
    }

    memset(begintag, 0, PMIX_IOF_BASE_TAG_MAX);
    memset(starttag, 0, PMIX_IOF_BASE_TAG_MAX);
    memset(endtag, 0, PMIX_IOF_BASE_TAG_MAX);
    memset(timestamp, 0, PMIX_IOF_BASE_TAG_MAX);
    memset(outtag, 0, PMIX_IOF_BASE_TAG_MAX);
    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);

    if (PMIX_FWD_STDOUT_CHANNEL & stream) {
        /* write the bytes to stdout */
        suffix = "stdout";
    } else if (PMIX_FWD_STDERR_CHANNEL & stream) {
        /* write the bytes to stderr */
        suffix = "stderr";
    } else if (PMIX_FWD_STDDIAG_CHANNEL & stream) {
        /* write the bytes to stderr */
        suffix = "stddiag";
    } else {
        /* error - this should never happen */
        PMIX_ERROR_LOG(PMIX_ERR_VALUE_OUT_OF_BOUNDS);
        pmix_output_verbose(1, pmix_client_globals.iof_output, "%s stream %0x",
                             PMIX_NAME_PRINT(&pmix_globals.myid), stream);
        PMIx_Byte_object_free(output, 1);
        return NULL;
    }


    if (!myflags->set) {
        /* the data is not to be tagged - just copy it
         * and move on to processing
         */
        output->bytes = (char*)malloc(bo->size);
        memcpy(output->bytes, bo->bytes, bo->size);
        output->size = bo->size;
        return output;
    }

    /* if this is to be xml tagged, create a tag with the correct syntax - we do not allow
     * timestamping of xml output
     */
    if (myflags->xml) {
        if (myflags->tag) {
            /* find the '@' delimiter in the nspace */
            cptr = strrchr(name->nspace, '@');
            if (NULL == cptr) {
                usestring = name->nspace;  // just use the whole thing
            } else {
                ++cptr;
                usestring = cptr; // use the jobid portion
            }
            pmix_snprintf(begintag, PMIX_IOF_BASE_TAG_MAX,
                          "<%s %s=\"%s\" rank=\"%s\"", suffix,
                          (usestring == name->nspace) ? "nspace" : "jobid",
                          usestring, PMIX_RANK_PRINT(name->rank));
        } else if (myflags->tag_fullname) {
            pmix_snprintf(begintag, PMIX_IOF_BASE_TAG_MAX,
                          "<%s nspace=\"%s\" rank=\"%s\"", suffix,
                          name->nspace, PMIX_RANK_PRINT(name->rank));
        } else if (myflags->tag_detailed) {
            /* we need the hostname and pid of the source */
            PMIX_CONSTRUCT(&cb2, pmix_cb_t);
            cb2.proc = (pmix_proc_t*)name;
            cb2.key = PMIX_HOSTNAME;
            cb2.info = &optional;
            cb2.ninfo = 1;
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
            if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                /* the union must not be read until the type says which
                 * member is live, and a string member may legitimately be
                 * NULL - "unknown" is what this reports for a hostname it
                 * cannot get, so use it here too rather than strdup(NULL) */
                if (NULL != kv && NULL != kv->value &&
                    PMIX_STRING == kv->value->type &&
                    NULL != kv->value->data.string) {
                    cptr = strdup(kv->value->data.string);
                } else {
                    cptr = strdup("unknown");
                }
                if (NULL != kv) {
                    PMIX_RELEASE(kv);
                }
            } else {
                cptr = strdup("unknown");
            }
            PMIX_DESTRUCT(&cb2);
            /* get the pid */
            PMIX_CONSTRUCT(&cb2, pmix_cb_t);
            cb2.proc = (pmix_proc_t*)name;
            cb2.key = PMIX_PROC_PID;
            cb2.info = &optional;
            cb2.ninfo = 1;
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
            if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                /* PMIx_Value_get_number reads value->type without screening
                 * the pointer, so the kval has to carry a value at all */
                if (NULL != kv && NULL != kv->value &&
                    PMIX_SUCCESS == PMIx_Value_get_number(kv->value, &pid, PMIX_PID)) {
                    pmix_asprintf(&pidstring, "%u", (unsigned int) pid);
                } else {
                    pidstring = strdup("unknown");
                }
                if (NULL != kv) {
                    PMIX_RELEASE(kv);
                }
            } else {
                pidstring = strdup("unknown");
            }
            PMIX_DESTRUCT(&cb2);

            pmix_snprintf(begintag, PMIX_IOF_BASE_TAG_MAX,
                          "<%s nspace=\"%s\" rank=\"%s\"[\"%s\":\"%s\"",
                          suffix, name->nspace,
                          PMIX_RANK_PRINT(name->rank),
                          cptr, pidstring);
            free(cptr);
            free(pidstring);
        } else if (myflags->rank) {
            pmix_snprintf(begintag, PMIX_IOF_BASE_TAG_MAX,
                     "<%s rank=\"%s\"", suffix,
                     PMIX_RANK_PRINT(name->rank));
        } else if (myflags->timestamp) {
            pmix_snprintf(begintag, PMIX_IOF_BASE_TAG_MAX,
                     "<%s rank=\"%s\"", suffix,
                     PMIX_RANK_PRINT(name->rank));
        } else {
            pmix_snprintf(begintag, PMIX_IOF_BASE_TAG_MAX,
                     "<%s rank=\"%s\"", suffix,
                     PMIX_RANK_PRINT(name->rank));
        }
        pmix_snprintf(endtag, PMIX_IOF_BASE_TAG_MAX,
                 "</%s>", suffix);
    } else {
        if (myflags->tag) {
            /* find the '@' delimiter in the nspace */
            cptr = strrchr(name->nspace, '@');
            if (NULL == cptr) {
                usestring = name->nspace;  // just use the whole thing
            } else {
                ++cptr;
                usestring = cptr; // use the jobid portion
            }
            pmix_snprintf(outtag, PMIX_IOF_BASE_TAG_MAX,
                          "[%s,%s]<%s>: ",
                          usestring,
                          PMIX_RANK_PRINT(name->rank),
                          suffix);
        } else if (myflags->tag_detailed) {
            if (myflags->tag_fullname) {
                usestring = name->nspace;
            } else {
                /* find the '@' delimiter in the nspace */
                cptr = strrchr(name->nspace, '@');
                if (NULL == cptr) {
                    usestring = name->nspace;  // just use the whole thing
                } else {
                    ++cptr;
                    usestring = cptr; // use the jobid portion
                }
            }
            /* we need the hostname and pid of the source */
            PMIX_CONSTRUCT(&cb2, pmix_cb_t);
            cb2.proc = (pmix_proc_t*)name;
            cb2.key = PMIX_HOSTNAME;
            cb2.info = &optional;
            cb2.ninfo = 1;
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
            if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                /* the union must not be read until the type says which
                 * member is live, and a string member may legitimately be
                 * NULL - "unknown" is what this reports for a hostname it
                 * cannot get, so use it here too rather than strdup(NULL) */
                if (NULL != kv && NULL != kv->value &&
                    PMIX_STRING == kv->value->type &&
                    NULL != kv->value->data.string) {
                    cptr = strdup(kv->value->data.string);
                } else {
                    cptr = strdup("unknown");
                }
                if (NULL != kv) {
                    PMIX_RELEASE(kv);
                }
            } else {
                cptr = strdup("unknown");
            }
            PMIX_DESTRUCT(&cb2);
            /* get the pid */
            PMIX_CONSTRUCT(&cb2, pmix_cb_t);
            cb2.proc = (pmix_proc_t*)name;
            cb2.key = PMIX_PROC_PID;
            cb2.info = &optional;
            cb2.ninfo = 1;
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
            if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                /* see the note on the xml form of this above */
                if (NULL != kv && NULL != kv->value &&
                    PMIX_SUCCESS == PMIx_Value_get_number(kv->value, &pid, PMIX_PID)) {
                    pmix_asprintf(&pidstring, "%u", (unsigned int) pid);
                } else {
                    pidstring = strdup("unknown");
                }
                if (NULL != kv) {
                    PMIX_RELEASE(kv);
                }
            } else {
                pidstring = strdup("unknown");
            }
            PMIX_DESTRUCT(&cb2);
            pmix_snprintf(outtag, PMIX_IOF_BASE_TAG_MAX,
                          "[%s,%s][%s:%s]<%s>: ",
                          usestring,
                          PMIX_RANK_PRINT(name->rank),
                          cptr, pidstring,
                          suffix);
            free(cptr);
            free(pidstring);
        } else if (myflags->tag_fullname) {
            pmix_snprintf(outtag, PMIX_IOF_BASE_TAG_MAX,
                          "[%s,%s]<%s>: ",
                          name->nspace,
                          PMIX_RANK_PRINT(name->rank),
                          suffix);
        } else if (myflags->rank) {
            pmix_snprintf(outtag, PMIX_IOF_BASE_TAG_MAX,
                     "[%s]<%s>: ",
                     PMIX_RANK_PRINT(name->rank), suffix);
        }
    }

    /* if we are to timestamp output, start the tag with that */
    if (myflags->timestamp) {
        time_t mytime;
        char tod[32];
        size_t tslen;
        /* ctime() renders into a process-wide static buffer, and the
         * newline strip below then writes into it - so this would corrupt
         * the result of any ctime/asctime the application is holding on
         * another thread. ctime_r gives us our own buffer (26 bytes is
         * the defined maximum) and is allowed to decline a time it cannot
         * represent rather than hand back a NULL to index into */
        time(&mytime);
        if (NULL == ctime_r(&mytime, tod)) {
            pmix_snprintf(tod, sizeof(tod), "unknown");
        } else {
            tslen = strlen(tod);
            if (0 < tslen && '\n' == tod[tslen - 1]) {
                tod[tslen - 1] = '\0'; /* remove trailing newline */
            }
        }
        cptr = tod;

        if (myflags->xml && !myflags->tag && !myflags->rank) {
            pmix_snprintf(timestamp, PMIX_IOF_BASE_TAG_MAX,
                     " timestamp=\"%s\"", cptr);
        } else if (myflags->xml && (myflags->tag || myflags->rank)) {
            pmix_snprintf(timestamp, PMIX_IOF_BASE_TAG_MAX,
                     " timestamp=\"%s\"", cptr);
        } else if (myflags->tag || myflags->rank) {
            pmix_snprintf(timestamp, PMIX_IOF_BASE_TAG_MAX, "[%s]", cptr);
        } else {
            pmix_snprintf(timestamp, PMIX_IOF_BASE_TAG_MAX, "[%s]<%s>: ", cptr, suffix);
        }
    }

    /* start with the starttag */
    if (0 < strlen(begintag)) {
        PMIx_Argv_append_nosize(&segments, begintag);
    }
    /* add the timestamp */
    if (0 < strlen(timestamp)) {
        PMIx_Argv_append_nosize(&segments, timestamp);
    }
    /* add the output tag */
    if (0 < strlen(outtag)) {
        PMIx_Argv_append_nosize(&segments, outtag);
    }
    /* if xml, end the starttag with a '>' */
    if (myflags->xml) {
        PMIx_Argv_append_nosize(&segments, ">");
    }

    /* if we are doing XML, then we need to replace key characters */
    if (myflags->xml) {
        bufsize = bo->size;
        for (n = 0; n < bo->size; n++) {
            /* the payload is arbitrary bytes, so it must be read as
             * unsigned: isprint() is undefined for a negative argument
             * other than EOF, and a signed byte would be escaped as a
             * negative character reference that is not valid XML */
            uc = (unsigned char) bo->bytes[n];
            if ('&' == uc) {
                bufsize += 5;
            } else if ('<' == uc || '>' == uc) {
                bufsize += 4;
            } else if (!isprint(uc)) {
                pmix_snprintf(qprint, sizeof(qprint), "&#%03u;", (unsigned int) uc);
                bufsize += strlen(qprint);
            }
        }
        if (bo->size < bufsize) {
            /* we need to increase the size of our buffer to handle the
             * extra characters we need to add to represent these special
             * cases */
            buffer = malloc(bufsize);
            memset(buffer, 0, bufsize);
            bufcopy = true;
            m = 0;
            for (n = 0; n < bo->size; n++) {
                /* read unsigned, exactly as the sizing pass above did */
                uc = (unsigned char) bo->bytes[n];
                if ('&' == uc) {
                    buffer[m++] = '&';
                    buffer[m++] = 'a';
                    buffer[m++] = 'm';
                    buffer[m++] = 'p';
                    buffer[m++] = ';';
                } else if ('<' == uc) {
                    buffer[m++] = '&';
                    buffer[m++] = 'l';
                    buffer[m++] = 't';
                    buffer[m++] = ';';
                } else if ('>' == uc) {
                    buffer[m++] = '&';
                    buffer[m++] = 'g';
                    buffer[m++] = 't';
                    buffer[m++] = ';';
                } else if (!isprint(uc)) {
                    pmix_snprintf(qprint, sizeof(qprint), "&#%03u;", (unsigned int) uc);
                    for (j = 0; j < strlen(qprint); j++) {
                        buffer[m++] = qprint[j];
                    }
                } else {
                    buffer[m++] = (char) uc;
                }
            }
            /* the initial bufsize was a worst-case over-estimate used to
             * size the allocation; the actual number of bytes written is
             * "m". Use that downstream so we don't copy the trailing
             * zero-fill into the output. */
            bufsize = m;
        } else {
            buffer = bo->bytes;
            bufsize = bo->size;
            bufcopy = false;
        }
    } else {
        buffer = bo->bytes;
        bufsize = bo->size;
        bufcopy = false;
    }

    /* assemble the output line */
    if (NULL != segments) {
        for (n=0; NULL != segments[n]; n++) {
            output->size += strlen(segments[n]);
        }
    }
    output->size += bufsize;
    output->size += strlen(endtag);
    if (myflags->xml) {
        // add a spot for a trailing newline
        output->size++;
    }

    output->bytes = (char*)malloc(output->size);
    offset = 0;
    if (NULL != segments) {
        for (n=0; NULL != segments[n]; n++) {
            memcpy(&output->bytes[offset], segments[n], strlen(segments[n]));
            offset += strlen(segments[n]);
        }
    }
    memcpy(&output->bytes[offset], buffer, bufsize);
    offset += bufsize;
    if (0 < strlen(endtag)) {
        memcpy(&output->bytes[offset], endtag, strlen(endtag));
    }
    if (myflags->xml) {
        output->bytes[output->size-1] = '\n';
    }
    if (bufcopy) {
        free(buffer);
    }
    if (NULL != segments) {
        PMIx_Argv_free(segments);
    }
    return output;
}

static pmix_status_t write_output_line(const pmix_proc_t *name,
                                       pmix_iof_write_event_t *channel,
                                       pmix_iof_flags_t *myflags,
                                       pmix_iof_channel_t stream,
                                       bool copystdout, bool copystderr,
                                       const pmix_byte_object_t *bo)
{
    pmix_byte_object_t *wbo;
    pmix_iof_write_output_t *copy;
    pmix_iof_write_output_t *output;


    /* write output data to the corresponding tag */
    if (PMIX_FWD_STDIN_CHANNEL & stream) {
        output = PMIX_NEW(pmix_iof_write_output_t);
        /* copy over the data to be written */
        if (0 < bo->size) {
            /* don't copy 0 bytes - we just need to pass
             * the zero bytes so the fd can be closed
             * after it writes everything out
             */
            output->data = (char*)malloc(bo->size);
            memcpy(output->data, bo->bytes, bo->size);
        }
        output->numbytes = bo->size;
        goto process;
    }

    // process the stream for output formatting
    wbo = pmix_iof_prep_output(name, myflags, stream, bo);
    if (NULL == wbo) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    output = PMIX_NEW(pmix_iof_write_output_t);
    output->data = wbo->bytes;
    output->numbytes = wbo->size;
    // protect returned data region
    wbo->bytes = NULL;
    wbo->size = 0;
    PMIx_Byte_object_free(wbo, 1);

process:
    /* add this data to the write list for this fd */
    pmix_list_append(&channel->outputs, &output->super);

    if (copystdout) {
        copy = PMIX_NEW(pmix_iof_write_output_t);
        copy->data = (char *) malloc(output->numbytes);
        memcpy(copy->data, output->data, output->numbytes);
        copy->numbytes = output->numbytes;
        pmix_list_append(&pmix_client_globals.iof_stdout.wev.outputs, &copy->super);
        if (!pmix_client_globals.iof_stdout.wev.pending) {
            PMIX_IOF_SINK_ACTIVATE(&pmix_client_globals.iof_stdout.wev);
        }
    }
    if (copystderr) {
        copy = PMIX_NEW(pmix_iof_write_output_t);
        copy->data = (char *) malloc(output->numbytes);
        memcpy(copy->data, output->data, output->numbytes);
        copy->numbytes = output->numbytes;
        pmix_list_append(&pmix_client_globals.iof_stderr.wev.outputs, &copy->super);
        if (!pmix_client_globals.iof_stderr.wev.pending) {
            PMIX_IOF_SINK_ACTIVATE(&pmix_client_globals.iof_stderr.wev);
        }
    }

    /* is the write event issued? */
    if (!channel->pending) {
        /* issue it */
        pmix_output_verbose(1, pmix_client_globals.iof_output,
                             "%s write:output adding write event",
                             PMIX_NAME_PRINT(&pmix_globals.myid));
        PMIX_IOF_SINK_ACTIVATE(channel);
    }

    return PMIX_SUCCESS;
}

/* Write out whatever we were holding for this source and stream because
 * it had no newline yet.
 *
 * The end of a stream is the last chance to do that - nothing more is
 * coming, so an unterminated final line has to go out now or not at all.
 * It used to go out "not at all": the zero-byte marker took an early exit
 * that never looked at the residual list, so the bytes sat there until
 * something else swept it. A server sweeps it in PMIx_server_finalize,
 * which at least prints the line, but out of order and at shutdown; a
 * client or tool never sweeps it, so a rank whose last write did not end
 * in a newline simply lost that write. */
static void flush_residual(const pmix_proc_t *name, pmix_iof_channel_t stream)
{
    pmix_iof_residual_t *res, *next;
    pmix_status_t rc;

    PMIX_LIST_FOREACH_SAFE(res, next, &pmix_server_globals.iof_residuals, pmix_iof_residual_t) {
        if (!PMIX_CHECK_PROCID(name, &res->name) || !(stream & res->stream)) {
            continue;
        }
        rc = write_output_line(&res->name, res->channel, &res->flags,
                               res->stream, res->copystdout, res->copystderr,
                               &res->bo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        pmix_list_remove_item(&pmix_server_globals.iof_residuals, &res->super);
        PMIX_RELEASE(res);
    }
}

/* What to format output with when the namespace it came from has nothing to
 * say about it. A tool that has a spawn in flight has already parsed that
 * spawn's directives (see stash_spawn_iof_flags() in pmix_client_spawn.c);
 * output that beat the reply here belongs to that spawn, so those flags are
 * the right answer rather than the process-wide default. */
static pmix_iof_flags_t spawn_or_global_flags(void)
{
    if (pmix_globals.spawn_iof_flags.set) {
        return pmix_globals.spawn_iof_flags;
    }
    return pmix_globals.iof_flags;
}

/* The stand-in above answers two questions, and both of its answers have to
 * be used or the second one is silently the process-wide default instead.
 * HOW to format the output is one; WHETHER to write it at all is the other,
 * and that is the one that was being dropped.
 *
 * A tool that spawned a job with an output-to-file directive is told not to
 * write that job's output locally - PMIX_IOF_FILE_ONLY, which is the default
 * for such a spawn, becomes local_output=false on the namespace when the
 * spawn reply lands. Output that arrives BEFORE that reply finds no namespace
 * record, so it fell through to pmix_globals.iof_flags.local_output - true
 * for a tool - and went to the terminal after all. The result was that a
 * nondeterministic subset of the job's output appeared on a terminal the user
 * had asked to keep clean: whichever ranks' first chunk beat the reply.
 */
static pmix_iof_flags_t stand_in_flags(bool *outputio)
{
    pmix_iof_flags_t flags = spawn_or_global_flags();

    if (flags.local_output_given) {
        *outputio = flags.local_output;
    }
    return flags;
}

pmix_status_t pmix_iof_write_output(const pmix_proc_t *name,
                                    pmix_iof_channel_t stream,
                                    const pmix_byte_object_t *bo)
{
    pmix_status_t rc;
    size_t n, start;
    pmix_byte_object_t bopass;
    pmix_iof_write_event_t *channel;
    pmix_iof_flags_t myflags;
    pmix_namespace_t *nptr, *ns;
    bool outputio;
    bool copystdout = false;
    bool copystderr = false;
    pmix_iof_sink_t *sink;
    pmix_iof_residual_t *res;
    char *inputdata;
    size_t inputsize;
    bool copied;

    /* stdin doesn't come thru here*/
    if (PMIX_FWD_STDIN_CHANNEL & stream) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find the nspace for this source */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t)
    {
        if (0 == strcmp(ns->nspace, name->nspace)) {
            nptr = ns;
            break;
        }
    }

    channel = NULL;
    /* default outputio to our flag */
    outputio = pmix_globals.iof_flags.local_output;

    if (NULL != nptr) {
        if (nptr->iof_flags.set) {
            if (nptr->iof_flags.local_output_given) {
                outputio = nptr->iof_flags.local_output;
            }
            if (!outputio) {
                return PMIX_SUCCESS;
            }
            /* do we need an IOF channel for this source? */
            if (NULL != nptr->iof_flags.directory) {
                /* see if we already have one */
                PMIX_LIST_FOREACH(sink, &nptr->sinks, pmix_iof_sink_t) {
                    if (sink->name.rank == name->rank &&
                        ((stream & sink->tag) || nptr->iof_flags.merge)) {
                        channel = &sink->wev;
                        break;
                    }
                }
                if (NULL == channel) {
                    /* need to set this one up */
                    channel = pmix_iof_setup(nptr, name->rank, stream);
                    if (NULL == channel) {
                        return PMIX_ERR_IOF_FAILURE;
                    }
                }
                if (!nptr->iof_flags.nocopy && pmix_globals.iof_flags.local_output) {
                    if (PMIX_FWD_STDOUT_CHANNEL & stream) {
                        copystdout = true;
                    } else {
                        copystderr = true;
                    }
                }
            } else if (NULL != nptr->iof_flags.file) {
                /* see if we already have one - we reuse the same sink for
                 * all streams */
                PMIX_LIST_FOREACH(sink, &nptr->sinks, pmix_iof_sink_t) {
                    if (sink->name.rank == name->rank &&
                        ((stream & sink->tag) || nptr->iof_flags.merge)) {
                        channel = &sink->wev;
                        break;
                    }
                }
                if (NULL == channel) {
                    /* need to set this one up */
                    channel = pmix_iof_setup(nptr, name->rank, stream);
                    if (NULL == channel) {
                        return PMIX_ERR_IOF_FAILURE;
                    }
                }
                if (!nptr->iof_flags.nocopy && pmix_globals.iof_flags.local_output) {
                    if (PMIX_FWD_STDOUT_CHANNEL & stream) {
                        copystdout = true;
                    } else {
                        copystderr = true;
                    }
                }
            }
            myflags = nptr->iof_flags;
        } else {
            myflags = stand_in_flags(&outputio);
        }
    } else {
        myflags = stand_in_flags(&outputio);
    }

    if (!outputio) {
        return PMIX_SUCCESS;
    }

    if (NULL == channel) {
        if (PMIX_FWD_STDOUT_CHANNEL & stream) {
            channel = &pmix_client_globals.iof_stdout.wev;
        } else {
            if (!myflags.merge) {
                channel = &pmix_client_globals.iof_stderr.wev;
            } else {
                channel = &pmix_client_globals.iof_stdout.wev;
            }
        }
    }

    pmix_output_verbose(1, pmix_client_globals.iof_output,
                         "%s write:output setting up to write %lu bytes to %s for %s on fd %d",
                         PMIX_NAME_PRINT(&pmix_globals.myid), (unsigned long) bo->size,
                         PMIx_IOF_channel_string(stream), PMIX_NAME_PRINT(name),
                         (NULL == channel) ? -1 : channel->fd);

    /* zero bytes can just be passed along - but not before anything we
     * were holding back for the last, still-unterminated line, as this
     * is the end of the stream and there will be no later chunk to
     * complete it */
    if (0 == bo->size) {
        flush_residual(name, stream);
        rc = write_output_line(name, channel, &myflags, stream,
                               false, false, bo);
        return rc;
    }

    if (myflags.raw) {
        rc = write_output_line(name, channel, &myflags, stream,
                               copystdout, copystderr, bo);
        return rc;

    } else {

        /* see if we have some residual for this name/stream */
        inputdata = bo->bytes;
        inputsize = bo->size;
        copied = false;
        PMIX_LIST_FOREACH(res, &pmix_server_globals.iof_residuals, pmix_iof_residual_t) {
            if (PMIX_CHECK_PROCID(name, &res->name) && (stream & res->stream)) {
                /* we need to pre-pend the residual data to the new
                 * data so any lines can be completed */
                inputdata = (char*)malloc(inputsize + res->bo.size);
                memcpy(inputdata, res->bo.bytes, res->bo.size);
                memcpy(&inputdata[res->bo.size], bo->bytes, bo->size);
                inputsize += res->bo.size;
                copied = true;
                pmix_list_remove_item(&pmix_server_globals.iof_residuals, &res->super);
                PMIX_RELEASE(res);
                break;
            }
        }

        /* search the input data stream for '\n' */
        start = 0;
        for (n=0; n < inputsize; n++) {
            if ('\n' == inputdata[n]) {
                bopass.bytes = &inputdata[start];
                bopass.size = n - start + 1;
                rc = write_output_line(name, channel, &myflags, stream,
                                       copystdout, copystderr, &bopass);
                if (PMIX_SUCCESS != rc) {
                    if (copied) {
                        free(inputdata);
                    }
                    return rc;
                }
                start = n + 1;
            }
        }

        if (start < inputsize) {
            /* we have some residual that needs to be cached until
             * the rest of the line is seen */
            res = PMIX_NEW(pmix_iof_residual_t);
            PMIX_XFER_PROCID(&res->name, name);
            res->channel = channel;
            memcpy(&res->flags, &myflags, sizeof(pmix_iof_flags_t));
            res->stream = stream;
            res->copystdout = copystdout;
            res->copystderr = copystderr;
            res->bo.bytes = (char*)malloc(inputsize - start);
            memcpy(res->bo.bytes, &inputdata[start], inputsize - start);
            res->bo.size = inputsize - start;
            pmix_list_append(&pmix_server_globals.iof_residuals, &res->super);
        }
    }
    if (copied) {
        free(inputdata);
    }
    return PMIX_SUCCESS;
}

void pmix_iof_flush_residuals(void)
{
    pmix_status_t rc;
    pmix_iof_residual_t *res, *next;

    /* drain the list as we go, the way flush_sink_residuals does: a
     * residual that has been written must not be written again if this
     * is ever reached twice */
    PMIX_LIST_FOREACH_SAFE(res, next, &pmix_server_globals.iof_residuals, pmix_iof_residual_t) {
        rc = write_output_line(&res->name, res->channel, &res->flags,
                               res->stream, res->copystdout, res->copystderr, &res->bo);
        pmix_list_remove_item(&pmix_server_globals.iof_residuals, &res->super);
        PMIX_RELEASE(res);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return;
        }
    }
}

static void flush_sink_residuals(pmix_iof_sink_t *sink)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_iof_residual_t *res, *next;
    pmix_list_t *residuals = &pmix_server_globals.iof_residuals;

    PMIX_LIST_FOREACH_SAFE(res, next, residuals, pmix_iof_residual_t) {
        if (res->channel != &sink->wev) {
            continue;
        }
        if (PMIX_SUCCESS == rc) {
            rc = write_output_line(&res->name, res->channel, &res->flags,
                                   res->stream, res->copystdout,
                                   res->copystderr, &res->bo);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
        pmix_list_remove_item(residuals, &res->super);
        PMIX_RELEASE(res);
    }
}

void pmix_iof_static_dump_output(pmix_iof_sink_t *sink)
{
    bool dump;
    int num_written;
    pmix_iof_write_event_t *wev = &sink->wev;
    pmix_iof_write_output_t *output;

    if (!pmix_list_is_empty(&wev->outputs)) {
        dump = false;
        /* make one last attempt to write this out */
        while (NULL != (output = (pmix_iof_write_output_t *) pmix_list_remove_first(&wev->outputs))) {
            if (!dump && 0 < output->numbytes) {
                num_written = write(wev->fd, output->data, output->numbytes);
                if (num_written < output->numbytes) {
                    /* don't retry - just cleanout the list and dump it */
                    dump = true;
                }
            }
            PMIX_RELEASE(output);
        }
    }
}

void pmix_iof_write_handler(int sd, short args, void *cbdata)
{
    pmix_iof_sink_t *sink = (pmix_iof_sink_t *) cbdata;
    pmix_iof_write_event_t *wev = &sink->wev;
    pmix_list_item_t *item;
    pmix_iof_write_output_t *output;
    int num_written, total_written = 0;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(sink);

    pmix_output_verbose(1, pmix_client_globals.iof_output,
                         "%s write:handler writing data to %d",
                         PMIX_NAME_PRINT(&pmix_globals.myid), wev->fd);

    while (NULL != (item = pmix_list_remove_first(&wev->outputs))) {
        output = (pmix_iof_write_output_t *) item;
        if (0 == output->numbytes) {
            /* the source that fed this marker has closed its stream */
            PMIX_RELEASE(output);
            if (2 < wev->fd) {
                /* a sink of our own making, fed by exactly that one
                 * source: close the channel and stop. "pending" is
                 * deliberately left set so nothing re-arms the event on a
                 * descriptor that is gone */
                close(wev->fd);
                wev->fd = -1;
                return;
            }
            /* a shared stdout/stderr channel is fed by every source we
             * output for, so one of them closing must not stop it. Skip
             * the marker and keep draining: returning here would leave
             * "pending" set with the event un-armed, and since
             * write_output_line only activates the event when "pending"
             * is clear, every later chunk from every other source would
             * sit in this list unwritten until finalize */
            continue;
        }
        num_written = write(wev->fd, output->data, output->numbytes);
        if (num_written < 0) {
            if (EAGAIN == errno || EINTR == errno) {
                /* push this item back on the front of the list */
                pmix_list_prepend(&wev->outputs, item);
                /* if the list is getting too large, abort */
                if (pmix_globals.output_limit < pmix_list_get_size(&wev->outputs)) {
                    pmix_output(0, "IO Forwarding is running too far behind - something is "
                                   "blocking us from writing");
                    goto ABORT;
                }
                /* leave the write event running so it will call us again
                 * when the fd is ready.
                 */
                wev->numtries++;
                if (PMIX_IOF_MAX_RETRIES < wev->numtries) {
                    /* give up */
                    pmix_output(0, "IO Forwarding is unable to output - something is "
                                "blocking us from writing");
                    goto ABORT;
                }
                goto NEXT_CALL;
            }
            /* otherwise, something bad happened so all we can do is abort
             * this attempt
             */
            PMIX_RELEASE(output);
            goto ABORT;
        } else if (num_written < output->numbytes) {
            /* incomplete write - adjust data to avoid duplicate output */
            memmove(output->data, &output->data[num_written], output->numbytes - num_written);
            /* adjust the number of bytes remaining to be written */
            output->numbytes -= num_written;
            /* push this item back on the front of the list */
            pmix_list_prepend(&wev->outputs, item);
            /* if the list is getting too large, abort */
            if (pmix_globals.output_limit < pmix_list_get_size(&wev->outputs)) {
                pmix_output(0, "IO Forwarding is running too far behind - something is blocking us "
                               "from writing");
                goto ABORT;
            }
            /* leave the write event running so it will call us again
             * when the fd is ready
             */
            wev->numtries = 0;
            goto NEXT_CALL;
        }
        PMIX_RELEASE(output);
        wev->numtries = 0;

        total_written += num_written;
        if (wev->always_writable && (PMIX_IOF_SINK_BLOCKSIZE <= total_written)) {
            /* If this is a regular file it will never tell us it will block
             * Write no more than PMIX_IOF_SINK_BLOCKSIZE at a time to allow
             * other fds to progress
             */
            goto NEXT_CALL;
        }
    }
ABORT:
    wev->pending = false;
    PMIX_POST_OBJECT(wev);
    return;
NEXT_CALL:
    PMIX_IOF_SINK_ACTIVATE(wev);
}

/* return true if we should read stdin from fd, false otherwise */
bool pmix_iof_stdin_check(int fd)
{
#if defined(HAVE_TCGETPGRP)
    if (isatty(fd) && (getpgrp() != tcgetpgrp(fd))) {
        return false;
    }
#endif
    return true;
}

void pmix_iof_stdin_cb(int sd, short args, void *cbdata)
{
    bool should_process;
    pmix_iof_read_event_t *stdinev = (pmix_iof_read_event_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* this is also the SIGCONT handler, which is registered with no
     * cbdata at all - it just means "reconsider the stdin we know
     * about", so resolve it to the one read event there can be */
    if (NULL == stdinev) {
        stdinev = stdinev_global;
        if (NULL == stdinev) {
            return;
        }
    }

    PMIX_ACQUIRE_OBJECT(stdinev);

    /* a suspended stream stays suspended until an XON says otherwise -
     * being told our terminal is back in the foreground does not
     * override the fact that the far end cannot take the data */
    should_process = !stdinev->xoff && pmix_iof_stdin_check(0);

    if (should_process) {
        PMIX_IOF_READ_ACTIVATE(stdinev);
    } else {
        pmix_event_del(&stdinev->ev);
        stdinev->active = false;
        PMIX_POST_OBJECT(stdinev);
    }
}

/* PMIx_Notify_event with a NULL cbfunc is its BLOCKING form: it posts a
 * caddy to the progress thread and then PMIX_WAIT_THREADs on it. The
 * callback below already runs on that thread, so waiting there is
 * waiting for ourselves - the caddy can never fire, and the progress
 * thread is dead for the life of the process. Handing it a do-nothing
 * completion takes the non-blocking path instead, which is what every
 * other in-library notifier does (see pmix_pfexec.c, psensor/file). */
static void iof_ntfy_cbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
}

static void iof_stdin_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                             pmix_buffer_t *buf, void *cbdata)
{
    pmix_iof_read_event_t *stdinev = (pmix_iof_read_event_t *) cbdata;
    int cnt;
    pmix_status_t rc, ret;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    PMIX_ACQUIRE_OBJECT(stdinev);

    /* check the return status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        pmix_event_del(&stdinev->ev);
        stdinev->active = false;
        PMIX_POST_OBJECT(stdinev);
        return;
    }
    /* the far end is backed up: suspend the stream, but leave it intact.
     * This is not a failure - no data was lost and the stream will be
     * resumed by an XON, so the tool is not told anything is wrong */
    if (PMIX_ERR_IOF_XOFF == ret) {
        stdinev->xoff = true;
        pmix_event_del(&stdinev->ev);
        stdinev->active = false;
        PMIX_POST_OBJECT(stdinev);
        pmix_output_verbose(1, pmix_client_globals.iof_output,
                            "%s iof:stdin suspended by server",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
        return;
    }

    /* if the status wasn't success, then terminate the forward */
    if (PMIX_SUCCESS != ret) {
        pmix_event_del(&stdinev->ev);
        stdinev->active = false;
        PMIX_POST_OBJECT(stdinev);
        if (PMIX_ERR_IOF_COMPLETE != ret) {
            /* generate an IOF-failed event so the tool knows */
            PMIx_Notify_event(PMIX_ERR_IOF_FAILURE, &pmix_globals.myid, PMIX_RANGE_PROC_LOCAL, NULL,
                              0, iof_ntfy_cbfunc, NULL);
        }
        return;
    }

    /* an XOFF may have arrived on its own while this chunk was in flight */
    if (stdinev->xoff) {
        return;
    }

    pmix_iof_stdin_cb(0, 0, stdinev);
}

/* carries the pushed data and the read event that produced it into the
 * host's completion callback, so the completion can both free the data
 * and decide whether to ask for the next chunk */
typedef struct {
    pmix_byte_object_t *bo;
    pmix_iof_read_event_t *rev;
} pmix_iof_push_caddy_t;

/* completion of a host push_stdin: the read is deliberately NOT re-armed
 * until we get here, so the host's pace is the stdin producer's pace and
 * a refusal has something to act on. PMIX_ERR_IOF_XOFF means the host
 * took the data and wants the stream suspended - leave the event
 * un-armed, and an XON through PMIx_server_IOF_flow_control will restart
 * it. Any other status is advisory: we have no way to re-deliver the
 * chunk, so keep reading rather than silently stranding the stream. */
static void opcbfn(pmix_status_t status, void *cbdata)
{
    pmix_iof_push_caddy_t *cd = (pmix_iof_push_caddy_t *) cbdata;
    pmix_iof_read_event_t *rev;

    PMIX_ACQUIRE_OBJECT(cd);

    rev = cd->rev;
    PMIX_BYTE_OBJECT_FREE(cd->bo, 1);
    free(cd);

    if (NULL == rev) {
        return;
    }
    if (PMIX_ERR_IOF_XOFF == status) {
        rev->xoff = true;
        pmix_output_verbose(1, pmix_client_globals.iof_output,
                            "%s iof:push_stdin suspended by host",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
        return;
    }
    if (rev->xoff) {
        /* an XOFF arrived while this push was in flight */
        return;
    }
    pmix_iof_stdin_cb(0, 0, rev);
}

/* relay a flow-control request to one peer that has pushed stdin to us */
static void relay_flow_control(pmix_peer_t *peer, const pmix_proc_t *source,
                               pmix_iof_channel_t channel, bool xoff,
                               const pmix_info_t directives[], size_t ndirs)
{
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_proc_t src;

    /* a peer that predates flow control has no recv posted on this tag,
     * and an unmatched message is an error at the far end - so say
     * nothing to it. It simply keeps sending, which is what it did
     * before this existed */
    if (PMIX_PEER_IS_EARLIER(peer, 7, 0, 0)) {
        return;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL == msg) {
        return;
    }
    /* a NULL source goes on the wire as a wildcard so the far end,
     * which may relay onward again, reads the same request we did */
    if (NULL == source) {
        PMIX_LOAD_PROCID(&src, NULL, PMIX_RANK_WILDCARD);
    } else {
        memcpy(&src, source, sizeof(pmix_proc_t));
    }
    PMIX_BFROPS_PACK(rc, peer, msg, &src, 1, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    PMIX_BFROPS_PACK(rc, peer, msg, &channel, 1, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    PMIX_BFROPS_PACK(rc, peer, msg, &xoff, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    PMIX_BFROPS_PACK(rc, peer, msg, &ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    if (0 < ndirs) {
        PMIX_BFROPS_PACK(rc, peer, msg, (pmix_info_t*)directives, ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return;
        }
    }

    PMIX_PTL_SEND_ONEWAY(rc, peer, msg, PMIX_PTL_TAG_IOF_CONTROL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
    }
}

pmix_status_t pmix_iof_flow_control(const pmix_proc_t *source,
                                    pmix_iof_channel_t channel,
                                    bool xoff,
                                    const pmix_info_t directives[], size_t ndirs)
{
    pmix_peer_t *peer;
    int i;

    /* stdin is the only stream anyone can be told to stop producing -
     * output flows the other way, and its producer is a process we do
     * not control */
    if (!(PMIX_FWD_STDIN_CHANNEL & channel)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    pmix_output_verbose(1, pmix_client_globals.iof_output,
                        "%s iof: flow control %s for %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        xoff ? "XOFF" : "XON",
                        (NULL == source) ? "ALL" : PMIX_NAME_PRINT(source));

    /* first, any stdin we are reading on our own behalf - we are its
     * producer, so our own identity is what a source names. A NULL
     * source means everyone; otherwise PMIx_Check_procid honors a
     * wildcard nspace and/or rank */
    if (NULL != stdinev_global &&
        (NULL == source || PMIx_Check_procid(source, &pmix_globals.myid))) {
        if (xoff) {
            stdinev_global->xoff = true;
            if (stdinev_global->active) {
                pmix_event_del(&stdinev_global->ev);
                stdinev_global->active = false;
                PMIX_POST_OBJECT(stdinev_global);
            }
        } else if (stdinev_global->xoff) {
            stdinev_global->xoff = false;
            /* route the restart through the usual check so a backgrounded
             * tty is still not read from */
            pmix_iof_stdin_cb(0, 0, stdinev_global);
        }
    }

    /* now anyone who has been pushing stdin to us. A launcher does both
     * halves, which is what carries the request back down a chain of
     * them to whoever actually holds the input stream */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        for (i = 0; i < pmix_server_globals.clients.size; i++) {
            peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
            if (NULL == peer || peer->finalized || !peer->stdin_producer) {
                continue;
            }
            if (NULL == peer->info) {
                continue;
            }
            if (NULL != source && !PMIX_CHECK_NAMES(source, &peer->info->pname)) {
                continue;
            }
            relay_flow_control(peer, source, channel, xoff, directives, ndirs);
        }
    }

    return PMIX_SUCCESS;
}

/* recv callback for PMIX_PTL_TAG_IOF_CONTROL - our server telling us to
 * suspend or resume the stdin we are feeding it */
void pmix_iof_flow_control_handler(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                                   pmix_buffer_t *buf, void *cbdata)
{
    pmix_proc_t source;
    pmix_iof_channel_t channel;
    bool xoff;
    size_t ndirs = 0;
    pmix_info_t *directives = NULL;
    int32_t cnt;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(hdr, cbdata);

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
    PMIX_BFROPS_UNPACK(rc, peer, buf, &xoff, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < ndirs) {
        PMIX_INFO_CREATE(directives, ndirs);
        if (NULL == directives) {
            return;
        }
        cnt = ndirs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(directives, ndirs);
            return;
        }
    }

    /* "everyone" travels as a wildcard rather than as an absent source,
     * and a wildcard nspace/rank already matches every producer */
    rc = pmix_iof_flow_control(&source, channel, xoff, directives, ndirs);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    if (NULL != directives) {
        PMIX_INFO_FREE(directives, ndirs);
    }
}

/* this is the read handler for stdin */
void pmix_iof_read_local_handler(int sd, short args, void *cbdata)
{
    pmix_iof_read_event_t *rev = (pmix_iof_read_event_t *) cbdata;
    unsigned char data[PMIX_IOF_BASE_MSG_MAX];
    int32_t numbytes;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_IOF_PUSH_CMD;
    pmix_byte_object_t bo, *boptr;
    pmix_iof_push_caddy_t *cd;
    int fd;
    pmix_pfexec_child_t *child = (pmix_pfexec_child_t *) rev->childproc;
    pmix_pfexec_child_t *tgt;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(rev);

    if (0 > rev->fd) {
        fd = fileno(stdin);
    } else {
        fd = rev->fd;
    }
    /* read up to the fragment size */
    memset(data, 0, PMIX_IOF_BASE_MSG_MAX);
    numbytes = read(fd, data, sizeof(data));

    /* The event has fired, so it's no longer active until we
     re-add it */
    rev->active = false;

    if (numbytes < 0) {
        /* either we have a connection error or it was a non-blocking read */

        /* non-blocking, retry */
        if (EAGAIN == errno || EINTR == errno) {
            PMIX_IOF_READ_ACTIVATE(rev);
            return;
        }

        pmix_output_verbose(1, pmix_client_globals.iof_output,
                             "%s iof:read handler Error on %s",
                             PMIX_NAME_PRINT(&pmix_globals.myid),
                             PMIx_IOF_channel_string(rev->channel));
        /* Un-recoverable error */
        bo.bytes = NULL;
        bo.size = 0;
        numbytes = 0;
    } else {
        bo.bytes = (char *) data;
        bo.size = numbytes;
    }

    /* if this is stdout or stderr of a child, then just output it */
    if (NULL != child &&
        (PMIX_FWD_STDOUT_CHANNEL == rev->channel ||
         PMIX_FWD_STDERR_CHANNEL == rev->channel)) {
        if (PMIX_FWD_STDOUT_CHANNEL == rev->channel) {
            rc = pmix_iof_write_output(&child->stdoutev->name, PMIX_FWD_STDOUT_CHANNEL, &bo);
        } else if (PMIX_FWD_STDERR_CHANNEL == rev->channel) {
            rc = pmix_iof_write_output(&child->stderrev->name, PMIX_FWD_STDERR_CHANNEL, &bo);
        } else {
            rc = PMIX_ERR_BAD_PARAM;
        }
        if (0 > rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* if the number of bytes is zero, then we just delete the event - there
         * is no need to pass it upstream as WE are the ones holding the event
         * and associated file descriptor */
        if (0 == numbytes) {
            if (NULL != child && child->completed &&
                (NULL == child->stdoutev || !child->stdoutev->active) &&
                (NULL == child->stderrev || !child->stderrev->active)) {
                PMIX_PFEXEC_CHK_COMPLETE(child);
            }
            return;
        }
        goto reactivate;
    }

    /* if it is stdin that was read, then see if we have a sink
     * for a child of ours that matches this target - this has precedence over
     * anything else */
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        if (rev == stdinev_global && NULL != rev->targets) {
            /* a separate variable: "child" is this read event's own
             * child, and the code above still means it */
            PMIX_LIST_FOREACH(tgt, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
                if (PMIX_CHECK_PROCID(&tgt->proc, &rev->targets[0])) {
                    /* send the input to that target */
                    rc = write_output_line(&tgt->proc, &tgt->stdinsink.wev, NULL,
                                           PMIX_FWD_STDIN_CHANNEL, false, false, &bo);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                    }
                    goto reactivate;
                }
            }
        }
    }

    /* if I am a launcher or a tool and connected to a server, then
     * we want to send things to our server for relay */
    if ((PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) ||
         PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) &&
        pmix_atomic_check_bool(&pmix_globals.connected)) {
        goto forward;
    }

    /* if I am a server, then push this up to my host */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        if (NULL == pmix_host_server.push_stdin) {
            /* nothing we can do with this info - no point in reactivating it */
            return;
        }
        PMIX_BYTE_OBJECT_CREATE(boptr, 1);
        if (0 < bo.size) {
            boptr->bytes = (char*)malloc(bo.size);
            memcpy(boptr->bytes, bo.bytes, bo.size);
            boptr->size = bo.size;
        }
        cd = (pmix_iof_push_caddy_t*)malloc(sizeof(pmix_iof_push_caddy_t));
        if (NULL == cd) {
            PMIX_BYTE_OBJECT_FREE(boptr, 1);
            return;
        }
        cd->bo = boptr;
        /* a zero-byte push is the last one this stream will ever make, so
         * there is nothing for the completion to re-arm */
        cd->rev = (0 == bo.size) ? NULL : rev;
        /* push this to the host even when it is empty - a zero-byte push is
         * how the host learns that our stdin has closed so it can close the
         * targets' stdin in turn. Dropping it leaves every target waiting on
         * an EOF that never arrives
         */
        rc = pmix_host_server.push_stdin(&pmix_globals.myid, rev->targets, rev->ntargets,
                                         rev->directives, rev->ndirs, boptr, opcbfn, (void*)cd);
        if (0 == bo.size) {
            /* our stdin fd has closed - there is nothing left to read, so
             * do not reactivate the event
             */
            if (PMIX_SUCCESS != rc) {
                /* the host will not call back - dispose of the caddy ourselves */
                opcbfn(rc, cd);
            }
            return;
        }
        if (PMIX_SUCCESS != rc) {
            /* by the host up-call convention a non-success return means the
             * host will not call our completion, so it is ours to run - and
             * it is what decides whether we read the next chunk. Note that
             * PMIX_ERR_IOF_XOFF arrives here too, and suspends us. */
            opcbfn(rc, cd);
        }
        /* the completion re-arms the read - do NOT do it here, or the host
         * has no way to pace us */
        return;
    }

forward:
    /* pass the data to our PMIx server so it can relay it
     * to the host RM for distribution */
    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL == msg) {
        /* don't restart the event - just return */
        return;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    /* pack the number of targets */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &rev->ntargets, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    /* and the targets */
    if (0 < rev->ntargets) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, rev->targets, rev->ntargets,
                         PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return;
        }
    }
    /* pack the number of directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &rev->ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }
    /* and the directives */
    if (0 < rev->ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, rev->directives, rev->ndirs,
                         PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return;
        }
    }

    /* pack the data */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &bo, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return;
    }

    /* send it to the server. On success the read is re-armed by
     * iof_stdin_cbfunc when the server acknowledges this chunk, not
     * here - that ack is what lets the far end pace us, and re-arming
     * now would both defeat it and leave the event armed twice */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, iof_stdin_cbfunc, rev);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        /* no ack is coming, so re-arm here or the stream stalls */
        goto reactivate;
    }
    return;

reactivate:
    if (0 < numbytes && !rev->xoff) {
        PMIX_IOF_READ_ACTIVATE(rev);
    }

    /* nothing more to do */
    return;
}

/* class instances */
static void iof_sink_construct(pmix_iof_sink_t *ptr)
{
    PMIX_CONSTRUCT(&ptr->wev, pmix_iof_write_event_t);
    ptr->xoff = false;
    ptr->exclusive = false;
    ptr->closed = false;
}
static void iof_sink_destruct(pmix_iof_sink_t *ptr)
{
    // flush any residuals on this namespace's sinks, and dump their output
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        flush_sink_residuals(ptr);
    }
    pmix_iof_static_dump_output(ptr);

    // close the sink
    if (0 <= ptr->wev.fd) {
        pmix_output_verbose(20, pmix_client_globals.iof_output,
                            "%s iof: closing sink for process %s on fd %d",
                            PMIX_NAME_PRINT(&pmix_globals.myid),
                            PMIX_NAME_PRINT(&ptr->name), ptr->wev.fd);
    }
    /* Always tear down the write event: the write handler sets wev.fd to
     * -1 after writing an EOF buffer, so gating the destruct on a valid
     * fd would leak the malloc'd wev.ev and the queued outputs list. The
     * wev destructor guards its own close() against an invalid fd. */
    PMIX_DESTRUCT(&ptr->wev);
}
PMIX_CLASS_INSTANCE(pmix_iof_sink_t,
                    pmix_list_item_t,
                    iof_sink_construct,
                    iof_sink_destruct);

static void iof_read_event_construct(pmix_iof_read_event_t *rev)
{
    rev->tv.tv_sec = 0;
    rev->tv.tv_usec = 0;
    rev->fd = -1;
    rev->channel = PMIX_FWD_NO_CHANNELS;
    rev->active = false;
    rev->childproc = NULL;
    rev->always_readable = false;
    rev->targets = NULL;
    rev->ntargets = 0;
    rev->directives = NULL;
    rev->ndirs = 0;
    rev->xoff = false;
}
static void iof_read_event_destruct(pmix_iof_read_event_t *rev)
{
    if (rev->active) {
        pmix_event_del(&rev->ev);
    }
    /* Only close a descriptor we opened. The read events this library
     * builds are either on a pipe it created (pfexec's children) or on
     * fileno(stdin), which belongs to the application - taking the
     * caller's stdin away when the library shuts down is not ours to do.
     * The write-event destructor guards itself the same way. */
    if (2 < rev->fd) {
        pmix_output_verbose(20, pmix_client_globals.iof_output, "%s iof: closing fd %d",
                             PMIX_NAME_PRINT(&pmix_globals.myid), rev->fd);
        close(rev->fd);
        rev->fd = -1;
    }
    if (NULL != rev->targets) {
        PMIX_PROC_FREE(rev->targets, rev->ntargets);
    }
    if (NULL != rev->directives) {
        PMIX_INFO_FREE(rev->directives, rev->ndirs);
    }
}
PMIX_CLASS_INSTANCE(pmix_iof_read_event_t, pmix_object_t, iof_read_event_construct,
                    iof_read_event_destruct);

static void iof_write_event_construct(pmix_iof_write_event_t *wev)
{
    wev->pending = false;
    wev->always_writable = false;
    wev->numtries = 0;
    wev->ev = (pmix_event_t*)malloc(sizeof(pmix_event_t));
    wev->fd = -1;
    PMIX_CONSTRUCT(&wev->outputs, pmix_list_t);
    wev->tv.tv_sec = 0;
    wev->tv.tv_usec = 0;
}
static void iof_write_event_destruct(pmix_iof_write_event_t *wev)
{
    if (wev->pending) {
        pmix_event_del(wev->ev);
    }
    free(wev->ev);
    if (2 < wev->fd) {
        pmix_output_verbose(20, pmix_client_globals.iof_output,
                             "%s iof: closing fd %d for write event",
                             PMIX_NAME_PRINT(&pmix_globals.myid), wev->fd);
        close(wev->fd);
    }
    PMIX_LIST_DESTRUCT(&wev->outputs);
}
PMIX_CLASS_INSTANCE(pmix_iof_write_event_t, pmix_list_item_t, iof_write_event_construct,
                    iof_write_event_destruct);

static void wocon(pmix_iof_write_output_t *p)
{
    p->data = NULL;
    p->numbytes = 0;
}
static void wodes(pmix_iof_write_output_t *p)
{
    if (NULL != p->data) {
        free(p->data);
    }
}
PMIX_CLASS_INSTANCE(pmix_iof_write_output_t,
                    pmix_list_item_t,
                    wocon, wodes);

static void iofrescon(pmix_iof_residual_t *p)
{
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->bo);
}
static void iofresdes(pmix_iof_residual_t *p)
{
    if (NULL != p->bo.bytes) {
        free(p->bo.bytes);
    }
}
PMIX_CLASS_INSTANCE(pmix_iof_residual_t,
                    pmix_list_item_t,
                    iofrescon, iofresdes);
