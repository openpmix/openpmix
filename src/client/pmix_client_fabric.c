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
    if (0 < ninfo) {
        PMIX_INFO_CREATE(cb->info, ninfo);
        cb->ninfo = ninfo;
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cb->info[n], &info[n]);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_WAKEUP_THREAD(&cb->lock);
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
        /* release the caller */
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
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
                        "pmix:fabric recv from server releasing");
    /* release the caller */
    PMIX_WAKEUP_THREAD(&cb->lock);
}


PMIX_EXPORT pmix_status_t PMIx_Fabric_register(pmix_fabric_t *fabric,
                                               const pmix_info_t directives[],
                                               size_t ndirs)
{
    pmix_cb_t cb;
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
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_REQUEST_INFO,
                                     directives, ndirs,
                                     fcb, &cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        if (0 < cb.ninfo) {
            fabric->info = cb.info;
            fabric->ninfo = cb.ninfo;
            cb.info = NULL;
            cb.ninfo = 0;
        }
        PMIX_DESTRUCT(&cb);
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
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, frecv, (void*)&cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_DESTRUCT(&cb);
    }
    /* wait for the response */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (0 < cb.ninfo) {
        fabric->info = cb.info;
        fabric->ninfo = cb.ninfo;
        cb.info = NULL;
        cb.ninfo = 0;
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_update(pmix_fabric_t *fabric)
{
    pmix_cb_t cb;
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
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_INFO_LOAD(&info, PMIX_FABRIC_INDEX, &fabric->index, PMIX_SIZE);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_UPDATE_INFO,
                                     &info, 1, fcb, &cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_INFO_DESTRUCT(&info);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        PMIX_WAIT_THREAD(&cb.lock);
        PMIX_INFO_DESTRUCT(&info);
        rc = cb.status;
        if (0 < cb.ninfo) {
            if (NULL != fabric->info) {
                PMIX_INFO_FREE(fabric->info, fabric->ninfo);
            }
            fabric->info = cb.info;
            fabric->ninfo = cb.ninfo;
            cb.info = NULL;
            cb.ninfo = 0;
        }
        PMIX_DESTRUCT(&cb);
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
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, frecv, (void*)&cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_DESTRUCT(&cb);
    }
    /* wait for the response */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (0 < cb.ninfo) {
        if (NULL != fabric->info) {
            PMIX_INFO_FREE(fabric->info, fabric->ninfo);
        }
        fabric->info = cb.info;
        fabric->ninfo = cb.ninfo;
        cb.info = NULL;
        cb.ninfo = 0;
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}


PMIX_EXPORT pmix_status_t PMIx_Fabric_deregister(pmix_fabric_t *fabric)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.deregister_fabric(fabric);
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return rc;
    }

    /* otherwise, just remove any storage in it */
    if (NULL != fabric->info) {
        PMIX_INFO_FREE(fabric->info, fabric->ninfo);
    }

    return PMIX_SUCCESS;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_get_vertex_info(pmix_fabric_t *fabric,
                                                      uint32_t i,
                                                      pmix_info_t **info, size_t *ninfo)
{
    pmix_cb_t cb;
    pmix_status_t rc;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_GET_VERTEX_INFO_CMD;
    pmix_info_t pinfo;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* set default */
    *info = NULL;
    *ninfo = 0;

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.get_vertex_info(fabric, i, info, ninfo);
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
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_INFO_LOAD(&pinfo, PMIX_FABRIC_INDEX, &fabric->index, PMIX_SIZE);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_GET_VERTEX_INFO,
                                     &pinfo, 1, fcb, &cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            PMIX_INFO_DESTRUCT(&pinfo);
            return rc;
        }
        PMIX_WAIT_THREAD(&cb.lock);
        PMIX_INFO_DESTRUCT(&pinfo);
        rc = cb.status;
        if (0 < cb.ninfo) {
            fabric->info = cb.info;
            fabric->ninfo = cb.ninfo;
            cb.info = NULL;
            cb.ninfo = 0;
        }
        PMIX_DESTRUCT(&cb);
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

    /* pack the index */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &i, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, frecv, (void*)&cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_DESTRUCT(&cb);
    }
    /* wait for the response */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (0 < cb.ninfo) {
        *info = cb.info;
        *ninfo = cb.ninfo;
        cb.info = NULL;
        cb.ninfo = 0;
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Fabric_get_device_index(pmix_fabric_t *fabric,
                                                       const pmix_info_t vertex[], size_t ninfo,
                                                       uint32_t *i)
{
    pmix_cb_t cb;
    pmix_status_t rc;
    size_t n;
    uint32_t index;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FABRIC_GET_DEVICE_INDEX_CMD;
    bool found;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* if I am a scheduler server, then I should be able
     * to support this myself */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        rc = pmix_pnet.get_device_index(fabric, vertex, ninfo, i);
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
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        rc = pmix_host_server.fabric(&pmix_globals.myid, PMIX_FABRIC_GET_DEVICE_INDEX,
                                     vertex, ninfo, fcb, &cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        if (PMIX_SUCCESS == rc && 0 < cb.ninfo) {
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
                     msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (NULL != vertex && 0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                         msg, vertex, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver,
                       msg, frecv, (void*)&cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_DESTRUCT(&cb);
    }
    /* wait for the response */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (PMIX_SUCCESS == rc && 0 < cb.ninfo) {
        found = false;
        for (n=0; n < cb.ninfo; n++) {
            if (PMIX_CHECK_KEY(&cb.info[n], PMIX_FABRIC_DEVICE_INDEX)) {
                PMIX_VALUE_GET_NUMBER(rc, &cb.info[n].value, index, uint32_t);
                if (PMIX_SUCCESS == rc) {
                    found = true;
                    *i = index;
                }
                break;
            }
        }
        if (!found) {
            rc = PMIX_ERR_BAD_PARAM;
        }
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}
