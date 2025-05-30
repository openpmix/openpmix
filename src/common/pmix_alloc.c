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

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_alloc_directive_t directive;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *results;
    size_t nresults;
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} pmix_alloc_caddy_t;
static void acon(pmix_alloc_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->results = NULL;
    p->nresults = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void ades(pmix_alloc_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
}
static PMIX_CLASS_INSTANCE(pmix_alloc_caddy_t,
                           pmix_object_t,
                           acon, ades);


static void acb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    size_t n;

    cb->status = status;
    if (NULL != info) {
        PMIX_INFO_CREATE(cb->info, ninfo);
        if (NULL == cb->info) {
            cb->status = PMIX_ERR_NOMEM;
            goto done;
        }
        cb->ninfo = ninfo;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cb->info[n], &info[n]);
        }
    }

done:
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static void relcbfunc(void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                "pmix:alloc release callback");

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

static void alloc_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_status_t rc;
    pmix_shift_caddy_t *results;
    int cnt;
    size_t n;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:alloc cback from server");

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return;
    }

    results = PMIX_NEW(pmix_shift_caddy_t);

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &results->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        results->status = rc;
        goto complete;
    }
    if (PMIX_SUCCESS != results->status) {
        goto complete;
    }

    /* unpack any returned data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &results->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        results->status = rc;
        goto complete;
    }
    if (0 < results->ninfo) {
        PMIX_INFO_CREATE(results->info, results->ninfo);
        cnt = results->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, results->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            results->status = rc;
            goto complete;
        }
        /* locally cache the results */
        for (n = 0; n < results->ninfo; n++) {
            kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(results->info[n].key);
            PMIX_VALUE_CREATE(kv->value, 1);
            PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kv->value, &results->info[n].value);

            PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kv);
            PMIX_RELEASE(kv); // maintain accounting
        }
    }

complete:
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:alloc cback from server releasing with status %s",
                        PMIx_Error_string(results->status));
    /* release the caller */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(results->status, results->info, results->ninfo, cd->cbdata, relcbfunc, results);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT pmix_status_t PMIx_Allocation_request(pmix_alloc_directive_t directive,
                                                  pmix_info_t *info, size_t ninfo,
                                                  pmix_info_t **results, size_t *nresults)
{
    pmix_cb_t cb;
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_globals.debug_output, "%s pmix:allocate",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* set the default response */
    *results = NULL;
    *nresults = 0;

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    if (PMIX_SUCCESS != (rc = PMIx_Allocation_request_nb(directive, info, ninfo, acb, &cb))) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the operation to complete */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (NULL != cb.info) {
        *results = cb.info;
        *nresults = cb.ninfo;
        /* protect the data */
        cb.info = NULL;
        cb.ninfo = 0;
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:allocate completed");

    return rc;
}

static void myrel(void *cbdata)
{
    pmix_alloc_caddy_t *acd = (pmix_alloc_caddy_t*)cbdata;
    PMIX_RELEASE(acd);
}

static void mycbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                     pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_alloc_caddy_t *acd = (pmix_alloc_caddy_t*)cbdata;

    if (NULL != acd->cbfunc) {
        acd->cbfunc(status, info, ninfo, acd->cbdata, myrel, acd);
    } else {
        PMIX_RELEASE(acd);
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
}

static void _allocreq(int sd, short args, void *cbdata)
{
    pmix_alloc_caddy_t *acd = (pmix_alloc_caddy_t*)cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    rc = pmix_host_server.allocate(&pmix_globals.myid, acd->directive,
                                   acd->info, acd->ninfo, mycbfunc, acd);
    if (PMIX_SUCCESS != rc) {
        // must not be supported - cannot return OPERATION_SUCCEEDED as there
        // is no way to return the results
        if (NULL != acd->cbfunc) {
            acd->cbfunc(rc, NULL, 0, acd->cbdata, myrel, acd);
        } else {
            PMIX_RELEASE(acd);
        }
    }
}

PMIX_EXPORT pmix_status_t PMIx_Allocation_request_nb(pmix_alloc_directive_t directive,
                                                     pmix_info_t *info, size_t ninfo,
                                                     pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_ALLOC_CMD;
    pmix_status_t rc;
    pmix_query_caddy_t *cb;
    pmix_alloc_caddy_t *acd;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: allocate called");

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    /* if we are hosted by the scheduler, then this makes no sense */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if our server is the scheduler, then send it */
    if (PMIX_PEER_IS_SCHEDULER(pmix_client_globals.myserver)) {
        goto sendit;
    }

    /* if we are the system controller, then nothing we can do
     * since the scheduler is not attached */
    if (PMIX_PEER_IS_SYS_CTRLR(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if we are a server and our host provides an allocate
     * entry, then pass it up - they might need to forward
     * it to their system controller or via some outside
     * path to the scheduler */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        NULL != pmix_host_server.allocate) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:allocate handed to host");
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        // need to be in our thread context to perform the upcall
        acd = PMIX_NEW(pmix_alloc_caddy_t);
        acd->directive = directive;
        acd->info = info;
        acd->ninfo = ninfo;
        acd->cbfunc = cbfunc;
        acd->cbdata = cbdata;
        PMIX_THREADSHIFT(acd, _allocreq);
        return PMIX_SUCCESS;
    }

sendit:
    /* for all other cases, we need to send this to someone
     * if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* all other cases, relay this request to our server, which
     * might (for a tool) actually be the scheduler itself */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the directive */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &directive, 1, PMIX_ALLOC_DIRECTIVE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the info */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_query_caddy_t);
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* threadshift to push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, alloc_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    return rc;
}


typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_resource_block_directive_t directive;
    char *block;
    const pmix_resource_unit_t *units;
    size_t nunits;
    const pmix_info_t *info;
    size_t ninfo;
    pmix_op_cbfunc_t cbfunc;
    void *cbdata;
} pmix_rb_caddy_t;
static void rbcon(pmix_rb_caddy_t *p)
{
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static PMIX_CLASS_INSTANCE(pmix_rb_caddy_t,
                           pmix_object_t,
                           rbcon, NULL);

static void blkcbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                      pmix_buffer_t *buf, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_status_t rc, status;
    int cnt;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:resource block cback from server");

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return;
    }

    /* unpack the returned status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        status = rc;
    }

    /* if we have a cbfunc, execute it */
    if (NULL != cd->cbfunc.opcbfn) {
        cd->cbfunc.opcbfn(status, cd->cbdata);
    }

    /* cleanup */
    PMIX_RELEASE(cd);
}

static void opcb(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    cb->status = status;
    PMIX_RELEASE_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Resource_block(pmix_resource_block_directive_t directive,
                                              char *block,
                                              const pmix_resource_unit_t *units, size_t nunits,
                                              const pmix_info_t *info, size_t ninfo)
{
    pmix_cb_t cb;
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "%s pmix:resource block op",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    rc = PMIx_Resource_block_nb(directive, block, units, nunits, info, ninfo, opcb, &cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the operation to complete */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:resource block operation completed");

    return rc;
}

static void _rbreq(int sd, short args, void *cbdata)
{
    pmix_rb_caddy_t *cd = (pmix_rb_caddy_t*)cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    rc = pmix_host_server.resource_block(&pmix_globals.myid, cd->directive, cd->block,
                                         cd->units, cd->nunits, cd->info, cd->ninfo,
                                         cd->cbfunc, cd->cbdata);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT pmix_status_t PMIx_Resource_block_nb(pmix_resource_block_directive_t directive,
                                                 char *block,
                                                 const pmix_resource_unit_t *units, size_t nunits,
                                                 const pmix_info_t *info, size_t ninfo,
                                                 pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_RESBLK_CMD;
    pmix_status_t rc;
    pmix_shift_caddy_t *cb;
    pmix_rb_caddy_t *cd;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: allocate called");

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    /* if we are hosted by the scheduler, then this makes no sense */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if our server is the scheduler, then send it */
    if (PMIX_PEER_IS_SCHEDULER(pmix_client_globals.myserver)) {
        goto sendit;
    }

    /* if we are the system controller, then nothing we can do
     * since the scheduler is not attached */
    if (PMIX_PEER_IS_SYS_CTRLR(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if we are a server and our host provides a resource block
     * entry, then pass it up - they might need to forward
     * it to their system controller or via some outside
     * path to the scheduler */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        NULL != pmix_host_server.resource_block) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:resource_block handed to host");
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        cd = PMIX_NEW(pmix_rb_caddy_t);
        cd->directive = directive;
        cd->block = block;
        cd->units = units;
        cd->nunits = nunits;
        cd->info = info;
        cd->ninfo = ninfo;
        cd->cbfunc = cbfunc;
        cd->cbdata = cbdata;
        PMIX_THREADSHIFT(cd, _rbreq);
        return PMIX_SUCCESS;
    }

sendit:
    /* for all other cases, we need to send this to someone
     *  if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* all other cases, relay this request to our server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the directive */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &directive, 1, PMIX_RESBLOCK_DIRECTIVE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the block name */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &block, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the units */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nunits, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (0 < nunits) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, units, nunits, PMIX_RESOURCE_UNIT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* pack the info */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_shift_caddy_t);
    cb->cbfunc.opcbfn = cbfunc;
    cb->cbdata = cbdata;

    /* threadshift to push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, blkcbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    return rc;
}
