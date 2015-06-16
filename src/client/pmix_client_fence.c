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

#include <private/autogen/config.h>
#include <pmix/rename.h>
#include <private/types.h>
#include <private/pmix_stdint.h>

#include <pmix.h>

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
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/hash.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_client_ops.h"

static int unpack_return(pmix_buffer_t *data);
static int pack_fence(pmix_buffer_t *msg,
                      pmix_cmd_t cmd,
                      pmix_proc_t *procs,
                      size_t nprocs,
                      int collect_data);
static void wait_cbfunc(struct pmix_peer_t *pr,
                        pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata);
static void op_cbfunc(int status, void *cbdata);

int PMIx_Fence(const pmix_proc_t procs[],
               size_t nprocs, int collect_data)
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
    if (PMIX_SUCCESS != (rc = PMIx_Fence_nb(procs, nprocs, collect_data, op_cbfunc, cb))) {
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

int PMIx_Fence_nb(const pmix_proc_t procs[], size_t nprocs,
                  int collect_data,
                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCENB_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_proc_t rg, *rgs;
    size_t nrg;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence_nb called");

    if (pmix_client_globals.init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == procs && 0 != nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* if we are given a NULL proc, then the caller is referencing
     * all procs within our own nspace */
    if (NULL == procs) {
        (void)strncpy(rg.nspace, pmix_globals.nspace, PMIX_MAX_NSLEN);
        rg.rank = PMIX_RANK_WILDCARD;
        rgs = &rg;
        nrg = 1;
    } else {
        rgs = (pmix_proc_t*)procs;
        nrg = nprocs;
    }
    
    msg = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pack_fence(msg, cmd, rgs, nrg, collect_data))) {
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
    pmix_buffer_t *bptr, *bpscope;
    pmix_kval_t *kp;
    char *nspace;
    int rank;
    pmix_nsrec_t *ns, *nptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "client:unpack fence called");

    /* unpack the status code */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "client:unpack fence received status %d", ret);
    if (PMIX_SUCCESS != ret) {
        return ret;
    }
    
    cnt = 1;
    /* if data was returned, unpack and store it */
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(data, &bptr, &cnt, PMIX_BUFFER))) {
        /* unpack the nspace */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &nspace, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "client:unpack fence unpacked blob for npsace %s", nspace);
        /* find the nspace object */
        nptr = NULL;
        PMIX_LIST_FOREACH(ns, &pmix_client_globals.nspaces, pmix_nsrec_t) {
            if (0 == strcmp(nspace, ns->nspace)) {
                nptr = ns;
                break;
            }
        }
        if (NULL == nptr) {
            /* new nspace - setup a record for it */
            nptr = PMIX_NEW(pmix_nsrec_t);
            (void)strncpy(nptr->nspace, nspace, PMIX_MAX_NSLEN);
            pmix_list_append(&pmix_client_globals.nspaces, &nptr->super);
        }
        /* unpack the rank */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &rank, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "client:unpack fence received blob for rank %d", rank);
        /* there may be multiple blobs for this rank, each from a different scope */
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &bpscope, &cnt, PMIX_BUFFER))) {
            /* unpack each value they provided */
            cnt = 1;
            kp = PMIX_NEW(pmix_kval_t);
            while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bpscope, kp, &cnt, PMIX_KVAL))) {
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "client:unpack fence unpacked key %s", kp->key);
                if (PMIX_SUCCESS != (rc = pmix_hash_store(&nptr->modex, rank, kp))) {
                    PMIX_ERROR_LOG(rc);
                }
                PMIX_RELEASE(kp);  // maintain acctg
                cnt = 1;
                kp = PMIX_NEW(pmix_kval_t);
            }
            PMIX_RELEASE(kp);  // maintain acctg
            cnt = 1;
        }  // while bpscope
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_RELEASE(bpscope);
        cnt = 1;
        PMIX_RELEASE(bptr);
        cnt = 1;
    } // while bptr
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }

    return rc;
}

static int pack_fence(pmix_buffer_t *msg,
                      pmix_cmd_t cmd,
                      pmix_proc_t *procs,
                      size_t nprocs,
                      int collect_data)
{
    int rc;
    
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of procs */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &nprocs, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack any provided procs - must always be at least one (our own) */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, procs, nprocs, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack the collect_data flag */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &collect_data, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
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

    if (NULL == cb) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    rc = unpack_return(buf);

    /* if a callback was provided, execute it */
    if (NULL != cb->op_cbfunc) {
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

