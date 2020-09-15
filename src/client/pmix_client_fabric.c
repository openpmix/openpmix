/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2015 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include "src/include/types.h"
#include "src/include/pmix_stdint.h"
#include "src/include/pmix_socket_errno.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include PMIX_EVENT_HEADER

#include "src/class/pmix_list.h"
#include "src/mca/ploc/ploc.h"
#include "src/mca/pnet/base/base.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/client/pmix_client_ops.h"

static void fcb(pmix_status_t status,
                pmix_info_t *info, size_t ninfo,
                void *cbdata,
                pmix_release_cbfunc_t release_fn,
                void *release_cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    size_t n;

    cb->status = status;
    if (PMIX_SUCCESS == status && 0 < ninfo) {
        PMIX_INFO_CREATE(cb->fabric->info, ninfo);
        cb->fabric->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cb->fabric->info[n], &info[n]);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    /* release the caller */
    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(status, cb->cbdata);
        PMIX_RELEASE(cb);
    } else {
        PMIX_WAKEUP_THREAD(&cb->lock);
    }
}

static void frecv(struct pmix_peer_t *peer,
                  pmix_ptl_hdr_t *hdr,
                  pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_status_t rc;
    int cnt;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric recv from server with %d bytes",
                        (int)buf->bytes_used);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        rc = PMIX_ERR_UNREACH;
        goto complete;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (PMIX_SUCCESS != cb->status) {
        goto complete;
    }

    /* unpack any returned data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->fabric->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < cb->fabric->ninfo) {
        PMIX_INFO_CREATE(cb->fabric->info, cb->fabric->ninfo);
        cnt = cb->fabric->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cb->fabric->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
    }

  complete:
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric recv from server releasing");
    /* release the caller */
    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(rc, cb->cbdata);
        PMIX_RELEASE(cb);
    } else {
        PMIX_WAKEUP_THREAD(&cb->lock);
    }
}


PMIX_EXPORT pmix_status_t PMIx_Fabric_register(pmix_fabric_t *fabric,
                                               const pmix_info_t directives[],
                                               size_t ndirs)
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
                        "pmix:fabric register");

    /* create a callback object so we can be notified when
     * the non-blocking operation is complete */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.fabric = fabric;
    if (PMIX_SUCCESS != (rc = PMIx_Fabric_register_nb(fabric, directives, ndirs, NULL, &cb))) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the data to return */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    /* the fabric info was directly filled into the fabric object */
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric register completed");

    return rc;
}


PMIX_EXPORT pmix_status_t PMIx_Fabric_register_nb(pmix_fabric_t *fabric,
                                                  const pmix_info_t directives[],
                                                  size_t ndirs,
                                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_REGISTER_CMD;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.register_fabric(fabric, directives, ndirs);
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return rc;
    }

    /* otherwise, if we are a server, then see if we can pass
     * it up to our host so they can send it to the scheduler */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (NULL == pmix_host_server.fabric) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        if (NULL != cbfunc) {
            cb = PMIX_NEW(pmix_cb_t);
            cb->fabric = fabric;
            cb->cbfunc.opfn = cbfunc;
            cb->cbdata = cbdata;
        } else {
            cb = (pmix_cb_t*)cbdata;
        }
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_REQUEST_INFO,
                                     directives, ndirs,
                                     fcb, (void*)cb);
        if (PMIX_SUCCESS != rc && NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* finally, if I am a tool or client, then I need to send it to
     * a daemon for processing */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (NULL != directives && 0 < ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                         msg, directives, ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    if (NULL != cbfunc) {
        cb = PMIX_NEW(pmix_cb_t);
        cb->fabric = fabric;
        cb->cbfunc.opfn = cbfunc;
        cb->cbdata = cbdata;
    } else {
        cb = (pmix_cb_t*)cbdata;
    }

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, frecv, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
    }
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_update(pmix_fabric_t *fabric)
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
                        "pmix:fabric update");

    /* create a callback object so we can be notified when
     * the non-blocking operation is complete */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.fabric = fabric;
    if (PMIX_SUCCESS != (rc = PMIx_Fabric_update_nb(fabric, NULL, &cb))) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the data to return */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    /* the fabric info was directly filled into the fabric object */
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric update completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_update_nb(pmix_fabric_t *fabric,
                                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_UPDATE_CMD;
    pmix_info_t info;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.update_fabric(fabric);
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return rc;
    }

    /* otherwise, if we are a server, then see if we can pass
     * it up to our host so they can send it to the scheduler */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (NULL == pmix_host_server.fabric) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        if (NULL != cbfunc) {
            cb = PMIX_NEW(pmix_cb_t);
            cb->fabric = fabric;
            cb->cbfunc.opfn = cbfunc;
            cb->cbdata = cbdata;
        } else {
            cb = (pmix_cb_t*)cbdata;
        }
        cb->infocopy = true;
        PMIX_INFO_CREATE(cb->info, 1);
        cb->ninfo = 1;
        PMIX_INFO_LOAD(&cb->info[0], PMIX_FABRIC_INDEX, &fabric->index, PMIX_SIZE);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_UPDATE_INFO,
                                     cb->info, 1, fcb, (void*)cb);
        if (PMIX_SUCCESS != rc && NULL != cbfunc) {
            PMIX_INFO_DESTRUCT(&info);
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* finally, if I am a tool or client, then I need to send it to
     * a daemon for processing */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* pack the fabric index */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &fabric->index, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    if (NULL != cbfunc) {
        cb = PMIX_NEW(pmix_cb_t);
        cb->fabric = fabric;
        cb->cbfunc.opfn = cbfunc;
        cb->cbdata = cbdata;
    } else {
        cb = (pmix_cb_t*)cbdata;
    }

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, frecv, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
    }
    return rc;
}


PMIX_EXPORT pmix_status_t PMIx_Fabric_deregister(pmix_fabric_t *fabric)
{
    pmix_status_t rc;

    rc = PMIx_Fabric_deregister_nb(fabric, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_deregister_nb(pmix_fabric_t *fabric,
                                                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.deregister_fabric(fabric);
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* otherwise, just remove any storage in it */
    if (NULL != fabric->info) {
        PMIX_INFO_FREE(fabric->info, fabric->ninfo);
    }

    return PMIX_OPERATION_SUCCEEDED;
}


PMIX_EXPORT pmix_status_t PMIx_Fabric_get_device_info(pmix_fabric_t *fabric,
                                                      uint32_t index,
                                                      pmix_info_t **info, size_t *ninfo)
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
                        "pmix:fabric get_vertex_info");

    /* create a callback object so we can be notified when
     * the non-blocking operation is complete */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.fabric = fabric;
    rc = PMIx_Fabric_get_device_info_nb(fabric, index, NULL, &cb);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    if (PMIX_OPERATION_SUCCEEDED != rc) {
        /* wait for the data to return */
        PMIX_WAIT_THREAD(&cb.lock);
    }
    rc = cb.status;
    /* xfer the results */
    if (NULL != cb.info) {
        *info = cb.info;
        *ninfo = cb.ninfo;
        cb.info = NULL;
        cb.ninfo = 0;
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric get_vertex_info completed");

    return rc;
}

static void icbrelfn(void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    PMIX_RELEASE(cb);
}

static void icbfunc(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    if (NULL != cb->cbfunc.infofn) {
        cb->cbfunc.infofn(cb->status, cb->info, cb->ninfo, cb->cbdata,
                          icbrelfn, cb);
        return;
    }

    PMIX_RELEASE(cb);
}

static void ifcb(pmix_status_t status,
                 pmix_info_t *info, size_t ninfo,
                 void *cbdata,
                 pmix_release_cbfunc_t release_fn,
                 void *release_cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    size_t n;

    if (NULL != cb->cbfunc.infofn) {
        cb->cbfunc.infofn(status, info, ninfo, cb->cbdata, icbrelfn, (void*)cb);
        return;
    }

    cb->status = status;
    if (NULL != cb->info) {
        PMIX_INFO_FREE(cb->info, cb->ninfo);
    }

    if (PMIX_SUCCESS == status && 0 < ninfo) {
        PMIX_INFO_CREATE(cb->info, ninfo);
        cb->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cb->info[n], &info[n]);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    /* release the caller */
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static void ifrecv(struct pmix_peer_t *peer,
                   pmix_ptl_hdr_t *hdr,
                   pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_status_t rc;
    int cnt;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric ifrecv from server with %d bytes",
                        (int)buf->bytes_used);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        rc = PMIX_ERR_UNREACH;
        goto complete;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (PMIX_SUCCESS != cb->status) {
        rc = cb->status;
        goto complete;
    }

    /* unpack any returned data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < cb->ninfo) {
        PMIX_INFO_CREATE(cb->info, cb->ninfo);
        cnt = cb->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cb->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
    }

  complete:
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric ifrecv from server releasing");
    /* release the caller */
    if (NULL != cb->cbfunc.infofn) {
        cb->cbfunc.infofn(rc, cb->info, cb->ninfo, cb->cbdata, icbrelfn, (void*)cb);
    } else {
        PMIX_WAKEUP_THREAD(&cb->lock);
    }
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_get_device_info_nb(pmix_fabric_t *fabric, uint32_t index,
                                                         pmix_info_cbfunc_t cbfunc, void *cbdata)

{
    pmix_cb_t *cb;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_GET_VERTEX_INFO_CMD;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (NULL != cbfunc) {
        cb = PMIX_NEW(pmix_cb_t);
        cb->fabric = fabric;
        cb->cbfunc.infofn = cbfunc;
        cb->cbdata = cbdata;
    } else {
        cb = (pmix_cb_t*)cbdata;
    }

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        cb->status = pmix_pnet.get_vertex_info(fabric, index, &cb->info, &cb->ninfo);
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        /* if they gave us a callback function, then we have
         * to threadshift before calling it. Otherwise, we can
         * just let the cb object return the results for us */
        if (NULL != cbfunc) {
            /* threadshift to return the result */
            PMIX_THREADSHIFT(cb, icbfunc);
        } else {
            if (PMIX_SUCCESS == cb->status) {
                cb->status = PMIX_OPERATION_SUCCEEDED;
            }
        }
        return cb->status;
    }

    /* otherwise, if we are a server, then see if we can pass
     * it up to our host so they can send it to the scheduler */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (NULL == pmix_host_server.fabric) {
            if (NULL != cbfunc) {
                PMIX_RELEASE(cb);
            }
            return PMIX_ERR_NOT_SUPPORTED;
        }
        PMIX_INFO_CREATE(cb->info, 1);
        PMIX_INFO_LOAD(&cb->info[0], PMIX_FABRIC_INDEX, &fabric->index, PMIX_SIZE);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_GET_DEVICE_INFO,
                                     cb->info, 1, ifcb, (void*)cb);
        if (PMIX_SUCCESS != rc && NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* finally, if I am a tool or client, then I need to send it to
     * a daemon for processing */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* pack the index */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &index, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, ifrecv, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
    }
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_get_device_index(pmix_fabric_t *fabric,
                                                       const pmix_info_t vertex[], size_t ninfo,
                                                       uint32_t *i)
{
    pmix_cb_t cb;
    pmix_status_t rc;
    uint32_t index;
    size_t n;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric get_device_index");

    /* set the default answer */
    *i = UINT32_MAX;

    /* create a callback object so we can be notified when
     * the non-blocking operation is complete */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.fabric = fabric;
    rc = PMIx_Fabric_get_device_index_nb(fabric, vertex, ninfo, NULL, &cb);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    if (PMIX_OPERATION_SUCCEEDED != rc) {
        /* wait for the data to return */
        PMIX_WAIT_THREAD(&cb.lock);
    }
    rc = cb.status;
    /* search the results for the PMIX_FABRIC_DEVICE_INDEX attribute */
    if (NULL != cb.info) {
        for (n=0; n < cb.ninfo; n++) {
            if (PMIX_CHECK_KEY(&cb.info[n], PMIX_FABRIC_DEVICE_INDEX)) {
                PMIX_VALUE_GET_NUMBER(rc, &cb.info[n].value, index, uint32_t);
                if (PMIX_SUCCESS == rc) {
                    *i = index;
                }
                break;
            }
        }
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric get_device_index completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_get_device_index_nb(pmix_fabric_t *fabric,
                                                          const pmix_info_t vertex[], size_t ninfo,
                                                          pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    uint32_t index;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_GET_DEVICE_INDEX_CMD;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    cb = PMIX_NEW(pmix_cb_t);
    cb->fabric = fabric;
    cb->cbfunc.infofn = cbfunc;
    cb->cbdata = cbdata;

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.get_device_index(fabric, vertex, ninfo, &index);
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
            /* must return the result in the info array of the cb object */
            PMIX_INFO_CREATE(cb->info, 1);
            cb->ninfo = 1;
            PMIX_INFO_LOAD(&cb->info[0], PMIX_FABRIC_DEVICE_INDEX, &index, PMIX_UINT32);
            cb->status = rc;
            PMIX_THREADSHIFT(cb, icbfunc);
            return PMIX_SUCCESS;
        }
        /* if we didn't get it, then return the error */
        PMIX_RELEASE(cb);
        return rc;
    }

    /* otherwise, if we are a server, then see if we can pass
     * it up to our host so they can send it to the scheduler */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (NULL == pmix_host_server.fabric) {
            PMIX_RELEASE(cb);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_GET_DEVICE_INDEX,
                                     vertex, ninfo, ifcb, cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* finally, if I am a tool or client, then I need to send it to
     * a daemon for processing */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        PMIX_RELEASE(cb);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
        return rc;
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
        return rc;
    }
    if (NULL != vertex && 0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                         msg, vertex, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_RELEASE(cb);
            return rc;
        }
    }

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, ifrecv, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }
    return rc;
}

pmix_status_t PMIx_Fabric_update_distances(pmix_device_distance_t *distances[],
                                           size_t *ndist)
{
    pmix_cb_t cb;
    pmix_status_t rc;
    size_t n;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric update_distances");

    *distances = NULL;
    *ndist = 0;

    /* create a callback object so we can be notified when
     * the non-blocking operation is complete */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    rc = PMIx_Fabric_update_distances_nb(NULL, &cb);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    if (PMIX_OPERATION_SUCCEEDED != rc) {
        /* wait for the data to return */
        PMIX_WAIT_THREAD(&cb.lock);
    }
    rc = cb.status;
    /* xfer the results */
    if (NULL != cb.info) {
        for (n=0; n < cb.ninfo; n++) {
            if (PMIX_CHECK_KEY(&cb.info[n], PMIX_FABRIC_DEVICE_DIST)) {
                *distances = (pmix_device_distance_t*)cb.info[n].value.data.darray->array;
                *ndist = cb.info[n].value.data.darray->size;
                break;
            }
        }
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:fabric get_vertex_info completed");

    return rc;
}

pmix_status_t PMIx_Fabric_update_distances_nb(pmix_info_cbfunc_t cbfunc,
                                              void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_UPDATE_DISTANCES_CMD;
    pmix_cpuset_t mycpuset;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (NULL != cbfunc) {
        cb = PMIX_NEW(pmix_cb_t);
        cb->cbfunc.infofn = cbfunc;
        cb->cbdata = cbdata;
    } else {
        cb = (pmix_cb_t*)cbdata;
    }

    /* see if I can support this myself */
    cb->status = pmix_ploc.update_distances(ifcb, (void*)cb);
    if (PMIX_SUCCESS == cb->status) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        /* if they gave us a callback function, then we have
         * to threadshift before calling it. Otherwise, we can
         * just let the cb object return the results for us */
        if (NULL != cbfunc) {
            /* threadshift to return the result */
            PMIX_THREADSHIFT(cb, icbfunc);
        } else {
            if (PMIX_SUCCESS == cb->status) {
                cb->status = PMIX_OPERATION_SUCCEEDED;
            }
        }
        return cb->status;
    }
    /* if I am a server, there is nothing more I can do */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) || !pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if we are a tool or client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* get our current location */
    PMIX_CPUSET_CONSTRUCT(&mycpuset);
    rc = pmix_ploc.get_location(&mycpuset);

    /* pack the cpuset */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &mycpuset, 1, PMIX_PROC_CPUSET);
    pmix_ploc.release(&mycpuset, 1);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, ifrecv, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        if (NULL != cbfunc) {
            PMIX_RELEASE(cb);
        }
    }
    return rc;
}
