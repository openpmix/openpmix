/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix.h"

#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/ptl/ptl.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_client_ops.h"

/* callback for wait completion */
static void wait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                        void *cbdata);
static void op_cbfunc(pmix_status_t status, void *cbdata);

PMIX_EXPORT pmix_status_t PMIx_Connect(const pmix_proc_t procs[], size_t nprocs,
                                       const pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_client_globals.connect_output, "pmix: connect called");

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);

    /* push the message into our event base to send to the server */
    if (PMIX_SUCCESS != (rc = PMIx_Connect_nb(procs, nprocs, info, ninfo, op_cbfunc, cb))) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the connect to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix: connect completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Connect_nb(const pmix_proc_t procs[], size_t nprocs,
                                          const pmix_info_t info[], size_t ninfo,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_CONNECTNB_CMD;
    pmix_status_t rc;
    pmix_cb_t *cb, cb2;
    pmix_byte_object_t bo;
    pmix_buffer_t pbkt;
    pmix_info_t xfer;
    pmix_kval_t *kv;
    void *ilist;
    pmix_data_array_t darray;
    bool found;
    size_t n;
    pmix_nspace_t nspace;
    pmix_rank_t minrank;
    pmix_proc_t proc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_client_globals.connect_output, "pmix:connect_nb called");

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* check for bozo input */
    if (NULL == procs || 0 >= nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, procs, nprocs, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the info structs */
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

    /* get our endpt info, if some was posted. We use
     * "remote" scope as all local procs have access
     * to info posted by all other local procs, regardless
     * of their namespace */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
    cb2.proc = &pmix_globals.myid;
    cb2.scope = PMIX_REMOTE;
    cb2.copy = true;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
    if (PMIX_SUCCESS == rc) {
        ilist = PMIx_Info_list_start();
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        // start with our procID
        PMIx_Info_list_add(ilist, PMIX_PROCID, &pmix_globals.myid, PMIX_PROC);
        // now add the kvals
        found = false;
        PMIX_LIST_FOREACH (kv, &cb2.kvs, pmix_kval_t) {
            if (PMIx_Check_reserved_key(kv->key)) {
                continue;
            }
            PMIx_Info_list_add_value_unique(ilist, kv->key, kv->value, true);
            found = true;
        }
        if (found) {
            // convert to array
            rc = PMIx_Info_list_convert(ilist, &darray);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIx_Info_list_release(ilist);
                return rc;
            }
            // insert into a pmix_info_t for packing
            PMIX_INFO_LOAD(&xfer, PMIX_PROC_DATA, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            // pack the result
            PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &xfer, 1, PMIX_INFO);
            PMIX_INFO_DESTRUCT(&xfer);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIx_Info_list_release(ilist);
                return rc;
            }
        }
        PMIx_Info_list_release(ilist);
    }
    PMIX_DESTRUCT(&cb2);

    /* if this operation involves multiple namespaces, then we need to
     * share job-level info between the participants. We only need to
     * add it once per namespace, so have the lowest participating rank
     * in each namespace add the info */
    PMIX_LOAD_NSPACE(nspace, procs[0].nspace);
    found = false;
    for (n=1; n < nprocs; n++) {
        if (!PMIX_CHECK_NSPACE(nspace, procs[n].nspace)) {
            found = true;
            break;
        }
    }
    if (found) {
        // see if I am the lowest participating rank from my namespace
        minrank = UINT32_MAX;
        for (n=0; n < nprocs; n++) {
            if (PMIX_CHECK_NSPACE(pmix_globals.myid.nspace, procs[n].nspace)) {
                // this is my nspace - check the rank
                if (PMIX_RANK_WILDCARD == procs[n].rank) {
                    // all ranks included, so see if I am rank 0
                    if (0 == pmix_globals.myid.rank) {
                        minrank = 0;
                        break;
                    }
                } else {
                    // see if I am the lowest
                    if (procs[n].rank < minrank) {
                        minrank = procs[n].rank;
                    }
                }
            }
        }
        if (minrank == pmix_globals.myid.rank) {
            // we will provide the job-level info for our nspace
            PMIX_CONSTRUCT(&cb2, pmix_cb_t);
            PMIX_LOAD_PROCID(&proc, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
            cb2.proc = &proc;
            cb2.scope = PMIX_SCOPE_UNDEF;
            cb2.copy = false;
            PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb2);
            if (PMIX_SUCCESS != rc) {
                if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
                    /* check the data in my hash module */
                    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&cb2);
                        goto moveon;
                    }
                } else {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&cb2);
                    goto moveon;
                }
            }
            if (0 < pmix_list_get_size(&cb2.kvs)) {
                // pack to send it along
                ilist = PMIx_Info_list_start();
                // start with our namespace
                PMIx_Info_list_add(ilist, PMIX_NSPACE, pmix_globals.myid.nspace, PMIX_PROC_NSPACE);
                // now add the kvals
                PMIX_LIST_FOREACH (kv, &cb2.kvs, pmix_kval_t) {
                    PMIx_Info_list_add_value_unique(ilist, kv->key, kv->value, true);
                    found = true;
                }
                // convert to array
                rc = PMIx_Info_list_convert(ilist, &darray);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(msg);
                    PMIx_Info_list_release(ilist);
                    return rc;
                }
                // insert into a pmix_info_t for packing
                PMIX_INFO_LOAD(&xfer, PMIX_JOB_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
                // pack the result
                PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &xfer, 1, PMIX_INFO);
                PMIX_DATA_ARRAY_DESTRUCT(&darray);
                PMIX_INFO_DESTRUCT(&xfer);
                PMIx_Info_list_release(ilist);
            }
        }
    }

moveon:
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->cbfunc.opfn = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, wait_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Disconnect(const pmix_proc_t procs[], size_t nprocs,
                                          const pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);
    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);

    if (PMIX_SUCCESS != (rc = PMIx_Disconnect_nb(procs, nprocs, info, ninfo, op_cbfunc, cb))) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the connect to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix: disconnect completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Disconnect_nb(const pmix_proc_t procs[], size_t nprocs,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_DISCONNECTNB_CMD;
    pmix_status_t rc;
    pmix_cb_t *cb;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix: disconnect called");

    size_t cnt;
    for (cnt = 0; cnt < nprocs; cnt++) {
        if (0 != strcmp(pmix_globals.myid.nspace, procs[cnt].nspace)) {
            PMIX_GDS_DEL_NSPACE(rc, procs[cnt].nspace);
        }
    }

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* check for bozo input */
    if (NULL == procs || 0 >= nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, procs, nprocs, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the info structs */
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
    cb = PMIX_NEW(pmix_cb_t);
    cb->cbfunc.opfn = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, wait_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix: disconnect completed");

    return rc;
}

static void wait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                        void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t rc;
    pmix_status_t ret;
    int32_t cnt;
    char *nspace;
    pmix_buffer_t bkt;
    pmix_byte_object_t bo;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    if (NULL == buf) {
        ret = PMIX_ERR_BAD_PARAM;
        goto report;
    }

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        ret = PMIX_ERR_UNREACH;
        goto report;
    }

    /* unpack the returned status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    /* connect has to also pass back data from all nspace's involved in
     * the operation, including our own. Each will come as a byte object */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    while (PMIX_SUCCESS == rc) {
        /* load it for unpacking */
        PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &bkt, bo.bytes, bo.size);

        /* unpack the nspace for this blob */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &bkt, &nspace, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            continue;
        }
        /* extract and process any proc-related info for this nspace */
        PMIX_GDS_STORE_JOB_INFO(rc, pmix_globals.mypeer, nspace, &bkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        free(nspace);
        PMIX_DESTRUCT(&bkt);
        /* get the next one */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

report:
    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(ret, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

static void op_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;

    cb->status = status;
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}
