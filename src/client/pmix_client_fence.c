/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdint.h"
#include "src/include/pmix_socket_errno.h"

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
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_client_hash.h"
#include "pmix_client_ops.h"

static int unpack_return(pmix_buffer_t *data);
static int pack_fence(pmix_buffer_t *msg,
                      pmix_cmd_t cmd,
                      pmix_range_t *ranges,
                      size_t nranges,
                      int collect_data,
                      int barrier);
static void wait_cbfunc(struct pmix_peer_t *pr,
                        pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata);
static void op_cbfunc(int status, void *cbdata);

int PMIx_Fence(const pmix_range_t ranges[],
               size_t nranges, int collect_data)
{
    pmix_cb_t *cb;
    int rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: executing fence");

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    if (PMIX_SUCCESS != (rc = PMIx_Fence_nb(ranges, nranges, true, collect_data, op_cbfunc, cb))) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the fence to complete */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    rc = cb->status;
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence released");

    return rc;
}

int PMIx_Fence_nb(const pmix_range_t ranges[], size_t nranges,
                  int barrier, int collect_data,
                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCENB_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_range_t rg, *rgs;
    size_t nrg;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence_nb called");

    if (pmix_client_globals.init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == ranges && 0 != nranges) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 != collect_data && 0 == barrier) {
        /* we cannot return all the data if we don't
         * do a barrier */
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* if we are given a NULL range, then the caller is referencing
     * all procs within our own nspace */
    if (NULL == ranges) {
        (void)strncpy(rg.nspace, pmix_globals.nspace, PMIX_MAX_NSLEN);
        rg.ranks = NULL;
        rg.nranks = 0;
        rgs = &rg;
        nrg = 1;
    } else {
        rgs = (pmix_range_t*)ranges;
        nrg = nranges;
    }
    
    msg = PMIX_NEW(pmix_buffer_t);
    /* if the barrier flag is true, then the server must delay calling
     * us back until all participating procs have called fence_nb. If false,
     * then the server should call us back right away */
    if (PMIX_SUCCESS != (rc = pack_fence(msg, cmd, rgs, nrg, collect_data, barrier))) {
        PMIX_RELEASE(msg);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->op_cbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, msg, wait_cbfunc, cb);

    return PMIX_SUCCESS;
}

static int unpack_return(pmix_buffer_t *data)
{
    int rc, ret;
    int32_t cnt;
    pmix_buffer_t buf;
    size_t np, i;
    pmix_kval_t *kp;
    pmix_modex_data_t *mdx;

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (PMIX_SUCCESS != ret) {
        return ret;
    }
    
    /* get the number of blobs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &np, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if data was returned, unpack and store it */
    if (0 < np) {
        mdx = (pmix_modex_data_t*)malloc(np * sizeof(pmix_modex_data_t));
        cnt = np;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, mdx, &cnt, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* now unpack and store the values - everything goes into our internal store */
        for (i=0; i < np; i++) {
            PMIX_CONSTRUCT(&buf, pmix_buffer_t);
            PMIX_LOAD_BUFFER(&buf, mdx[i].blob, mdx[i].size);
            cnt = 1;
            kp = PMIX_NEW(pmix_kval_t);
            while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&buf, kp, &cnt, PMIX_KVAL))) {
                if (PMIX_SUCCESS != (rc = pmix_client_hash_store(mdx[i].nspace, mdx[i].rank, kp))) {
                    PMIX_ERROR_LOG(rc);
                }
                PMIX_RELEASE(kp);  // maintain acctg
                cnt = 1;
                kp = PMIX_NEW(pmix_kval_t);
            }
            PMIX_RELEASE(kp);  // maintain acctg
            PMIX_DESTRUCT(&buf);  // free's the data region
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
        if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        } else {
            rc = PMIX_SUCCESS;
        }
    }

    return rc;
}

static int pack_fence(pmix_buffer_t *msg,
                      pmix_cmd_t cmd,
                      pmix_range_t *ranges,
                      size_t nranges,
                      int collect_data,
                      int barrier)
{
    int rc;
    pmix_scope_t scope;
    
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of ranges */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &nranges, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack any provided ranges */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, ranges, nranges, PMIX_RANGE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack the collect_data flag */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &collect_data, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack the barrier flag */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &barrier, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* if we haven't already done it, ensure we have committed our values */
    if (NULL != pmix_client_globals.cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PMIX_RELEASE(pmix_client_globals.cache_local);
    }
    if (NULL != pmix_client_globals.cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PMIX_RELEASE(pmix_client_globals.cache_remote);
    }
    if (NULL != pmix_client_globals.cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PMIX_RELEASE(pmix_client_globals.cache_global);
    }
    return PMIX_SUCCESS;
}

static void wait_cbfunc(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence_nb callback recvd");

    rc = unpack_return(buf);

    /* if a callback was provided, execute it */
    if (NULL != cb && NULL != cb->op_cbfunc) {
        cb->op_cbfunc(rc, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

static void op_cbfunc(int status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    cb->status = status;
    cb->active = false;
}

