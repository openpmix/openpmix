/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#include "src/common/pmix_monitor.h"

static void query_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata);
static void acb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                pmix_release_cbfunc_t release_fn, void *release_cbdata);

static void relcbfunc(void *cbdata)
{
    pmix_cb_t *cd = (pmix_cb_t *) cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:monitor release callback");

    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_Process_monitor(const pmix_info_t *monitor, pmix_status_t error,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_t **results, size_t *nresults)
{
    pmix_cb_t cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "%s pmix:monitor",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    // init return values
    *results = NULL;
    *nresults = 0;

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    rc = PMIx_Process_monitor_nb(monitor, error, directives, ndirs, acb, &cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the operation to complete */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (0 < cb.ninfo) {
        *results = cb.info;
        *nresults = cb.ninfo;
        cb.info = NULL;
        cb.ninfo = 0;
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:monitor completed");

    return rc;
}

void pmix_monitor_processing(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_status_t rc;
    pmix_proc_t *procs, proc;
    pmix_info_t *results;
    size_t sz, m, n;
    int k;
    pmix_peer_t *ptr;
    pmix_node_pid_t *ppid;
    char **hostnames;
    uint32_t *nids;
    bool local, remote;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // the monitor argument is in cb->info
    // directives are in cb->directives

    local = false;
    remote = false;

    // see if the requested targets involve our node, or processes on
    // our local node
    for (n=0; n < cb->ndirs; n++) {
        if (PMIx_Check_key(cb->directives[n].key, PMIX_MONITOR_TARGET_PROCS)) {
            // if there are no procs specified, then it is all procs everywhere
            if (NULL == cb->directives[n].value.data.darray ||
                NULL == cb->directives[n].value.data.darray->array) {
                local = true;
                remote = true;
                break;
            }
            // are any of the procs local?
            procs = (pmix_proc_t*)cb->directives[n].value.data.darray->array;
            sz = cb->directives[n].value.data.darray->size;
            for (m=0; !local && !remote && m < sz; m++) {
                for (k=0; (!local || !remote) && k < pmix_server_globals.clients.size; k++) {
                    ptr = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
                    if (NULL == ptr) {
                        continue;
                    }
                    PMIx_Load_procid(&proc, ptr->info->pname.nspace, ptr->info->pname.rank);
                    if (PMIx_Check_procid(&procs[m], &proc)) {
                        local = true;
                    } else {
                        remote = true;
                    }
                }
            }
            if (local && remote) {
                // no need to process further
                break;
            }
            continue;
        }

        if (PMIx_Check_key(cb->directives[n].key, PMIX_MONITOR_TARGET_NODES)) {
            // if there are no hosts specified, then it is all hosts everywhere
            if (NULL == cb->directives[n].value.data.darray ||
                NULL == cb->directives[n].value.data.darray->array) {
                local = true;
                remote = true;
                break;
            }
            // is our node included?
            hostnames = (char**)cb->directives[n].value.data.darray->array;
            sz = cb->directives[n].value.data.darray->size;
            for (m=0; (!local || !remote) && m < sz; m++) {
                // see if this node is us
                if (0 == strcmp(hostnames[m], pmix_globals.hostname)) {
                    local = true;
                } else {
                    remote = true;
                }
            }
            if (local && remote) {
                // no need to process further
                break;
            }
            continue;
        }

        if (PMIx_Check_key(cb->directives[n].key, PMIX_MONITOR_TARGET_NODEIDS)) {
            // if there are no nodeIDs specified, then it is all hosts everywhere
            if (NULL == cb->directives[n].value.data.darray ||
                NULL == cb->directives[n].value.data.darray->array) {
                local = true;
                remote = true;
                break;
            }
            // is our node included?
            nids = (uint32_t*)cb->directives[n].value.data.darray->array;
            sz = cb->directives[n].value.data.darray->size;
            for (m=0; (!local || !remote) && m < sz; m++) {
                // see if this node is us
                if (nids[m] == pmix_globals.nodeid) {
                    local = true;
                } else {
                    remote = true;
                }
            }
            if (local && remote) {
                // no need to process further
                break;
            }
            continue;
        }

        if (PMIx_Check_key(cb->directives[n].key, PMIX_MONITOR_TARGET_PIDS)) {
            // if there are no host/pids specified, then it is all host/pids everywhere
            if (NULL == cb->directives[n].value.data.darray ||
                NULL == cb->directives[n].value.data.darray->array) {
                local = true;
                remote = true;
                break;
            }
            // is our node included?
            ppid = (pmix_node_pid_t*)cb->directives[n].value.data.darray->array;
            sz = cb->directives[n].value.data.darray->size;
            for (m=0; (!local || !remote) && m < sz; m++) {
                // see if this node is us
                if (ppid[m].nodeid == pmix_globals.nodeid ||
                    0 == strcmp(ppid[m].hostname, pmix_globals.hostname)) {
                    local = true;
                } else {
                    remote = true;
                }
            }
            if (local && remote) {
                // no need to process further
                break;
            }
        }

    }

    // if it involves other nodes or processes on other nodes,
    // then we must pass it to the host - check if we can do so
    if (remote && NULL == pmix_host_server.monitor) {
        /* nothing we can do */
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto complete;
    }

    if (!remote) {
        // we have strictly local participation, so pass it down for processing
        // and return the results directly to the caller
        rc = pmix_pstat.query(cb->info, cb->status, cb->directives, cb->ndirs,
                              &results, &sz);
        if (PMIX_SUCCESS != rc) {
            if (cb->infocopy) {
                PMIX_INFO_FREE(cb->info, cb->ninfo);
                cb->info = NULL;
                cb->ninfo = 0;
                cb->infocopy = false;
            }
            goto complete;
        }
        // replace the monitor struct with the returned results
        if (cb->infocopy) {
            PMIX_INFO_FREE(cb->info, cb->ninfo);
        }
        cb->infocopy = true;
        cb->info = results;
        cb->ninfo = sz;
        if (NULL != cb->cbfunc.infofn) {
            cb->cbfunc.infofn(PMIX_SUCCESS, cb->info, cb->ninfo, cb,
                              relcbfunc, (void*)cb);
        } else {
            PMIX_WAKEUP_THREAD(&cb->lock);
        }

    } else if (!local) {
        // strictly remote participation, so pass it up - we already checked
        // for host-level support
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:monitor handed to RM");
        rc = pmix_host_server.monitor(&pmix_globals.myid, cb->info, cb->status,
                                      cb->directives, cb->ndirs, cb->cbfunc.infofn,
                                      cb->cbdata);
        if (PMIX_SUCCESS != rc) {
            goto complete;
        }

    } else {
        // we have both local and remote participation, so handle the local
        // contribution first
        rc = pmix_pstat.query(cb->info, cb->status, cb->directives, cb->ndirs,
                              &results, &sz);
        if (PMIX_SUCCESS != rc) {
            goto complete;
        }

    }

    // wait for callback
    return;

complete:
    if (NULL != cb->cbfunc.infofn) {
        cb->cbfunc.infofn(rc, cb->info, cb->ninfo, cb, relcbfunc, (void*)cb);
    } else {
        PMIX_WAKEUP_THREAD(&cb->lock);
    }
}

pmix_status_t PMIx_Process_monitor_nb(const pmix_info_t *monitor, pmix_status_t error,
                                       const pmix_info_t directives[], size_t ndirs,
                                       pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_MONITOR_CMD;
    pmix_status_t rc;
    pmix_query_caddy_t *qcd;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: monitor called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* sanity check */
    if (NULL == monitor) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we are the server, then we locally handle this */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {

        // makes mo sense for server to send heartbeat
        if (PMIx_Check_key(monitor->key, PMIX_SEND_HEARTBEAT)) {
            return PMIX_ERR_BAD_PARAM;
        }
        // threadshift for internal access
        cb = PMIX_NEW(pmix_cb_t);
        cb->infocopy = false;
        cb->info = (pmix_info_t*)monitor;
        cb->ninfo = 1;
        cb->status = error;
        cb->directives = (pmix_info_t*)directives;
        cb->ndirs = ndirs;
        if (NULL != cbfunc) {
            cb->cbfunc.infofn = cbfunc;
            cb->cbdata = cbdata;
        } else {
            cb->cbfunc.infofn = acb;
            cb->cbdata = cb;
        }
        PMIX_THREADSHIFT(cb, pmix_monitor_processing);
        return PMIX_SUCCESS;
    }

    /* non-servers cannot perform the monitoring - we need to send
     * the request to our server, so check for connection */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    /* if the monitor is PMIX_SEND_HEARTBEAT, then send it */
    if (PMIX_CHECK_KEY(monitor, PMIX_SEND_HEARTBEAT)) {
        PMIx_Heartbeat();
        return PMIX_SUCCESS;
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

    /* pack the monitor */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, monitor, 1, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the error */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &error, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (0 < ndirs) {
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

    /* threadshift to push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, query_cbfunc, (void *) qcd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(qcd);
    }

    return rc;
}

void PMIx_Heartbeat(void)
{
    pmix_buffer_t *msg;
    pmix_status_t rc;

    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL == msg) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    // threadshift to push msg into our event base for transmission
    PMIX_PTL_SEND_ONEWAY(rc, pmix_client_globals.myserver, msg, PMIX_PTL_TAG_HEARTBEAT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
    }
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
                        "pmix:monitor cback from server with %d bytes",
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
                        "pmix:monitor cback from server releasing");
    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(results->status, results->info, results->ninfo,
                   cd->cbdata, relcbfunc, results);
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

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:monitor intermediate cback");

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
    } else {
        cb->info = NULL;
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

