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

static pmix_buffer_t* pack_get(const char nspace[], int rank,
                               const char key[], pmix_cmd_t cmd);
static int unpack_get_return(pmix_buffer_t *data, const char *key,
                             pmix_value_t **val);
static void getnb_cbfunc(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata);
static void getnb_shortcut(int fd, short flags, void *cbdata);
static void value_cbfunc(int status, pmix_value_t *kv, void *cbdata);

int PMIx_Get(const char nspace[], int rank,
             const char key[], pmix_value_t **val)
{
    pmix_cb_t *cb;
    int rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: getting value for proc %s:%d key %s",
                        (NULL == nspace) ? "NULL" : nspace, rank,
                        (NULL == key) ? "NULL" : key);

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->active = true;

    if (PMIX_SUCCESS != (rc = PMIx_Get_nb(nspace, rank, key, value_cbfunc, cb))) {
        PMIX_RELEASE(cb);
        return rc;
    }
    
    /* wait for the data to return */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    rc = cb->status;
    *val = cb->value;
    PMIX_RELEASE(cb);
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client get completed");

    return rc;
}

int PMIx_Get_nb(const char *nspace, int rank,
                const char *key,
                pmix_value_cbfunc_t cbfunc, void *cbdata)
{
    pmix_value_t *val;
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    int rc;
    char *nm;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: get_nb value for proc %s:%d key %s",
                        (NULL == nspace) ? "NULL" : nspace, rank,
                        (NULL == key) ? "NULL" : key);
    if (pmix_client_globals.init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* protect against bozo input */
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* if the nspace is NULL, then the caller is referencing
     * our own nspace */
    if (NULL == nspace) {
        nm = pmix_globals.nspace;
    } else {
        nm = (char*)nspace;
    }
    
    /* first see if we already have the info in our dstore */
    if (PMIX_SUCCESS == pmix_client_hash_fetch(nm, rank, key, &val)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: value retrieved from dstore");
        /* need to push this into the event library to ensure
         * the callback occurs within an event */
        cb = PMIX_NEW(pmix_cb_t);
        cb->nspace = strdup(nm);
        cb->rank = rank;
        cb->key = strdup(key);
        cb->value_cbfunc = cbfunc;
        cb->cbdata = cbdata;
        /* pack the return data so the unpack routine can get it */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&cb->data, val, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
        /* cleanup */
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
        /* activate the event */
        event_assign(&(cb->ev), pmix_globals.evbase, -1,
                     EV_WRITE, getnb_shortcut, cb);
        event_active(&(cb->ev), EV_WRITE, 1);
        return PMIX_SUCCESS;
    }
    
    if (NULL == (msg = pack_get(nm, rank, key, PMIX_GETNB_CMD))) {
        return PMIX_ERROR;
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->nspace = strdup(nm);
    cb->rank = rank;
    cb->key = strdup(key);
    cb->value_cbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, msg, getnb_cbfunc, cb);

    return PMIX_SUCCESS;
}

static void value_cbfunc(int status, pmix_value_t *kv, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    cb->status = status;
    pmix_bfrop.copy((void**)&cb->value, kv, PMIX_VALUE);
    cb->active = false;
}

static pmix_buffer_t* pack_get(const char nspace[], int rank,
                               const char key[], pmix_cmd_t cmd)
{
    pmix_buffer_t *msg;
    int rc;
    
    /* nope - see if we can get it */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the get cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &rank, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    return msg;
}

static int unpack_get_return(pmix_buffer_t *data, const char *key,
                             pmix_value_t **val)
{
    int rc, ret;
    int32_t cnt;
    pmix_buffer_t buf;
    size_t np, i;
    pmix_kval_t *kp;
    pmix_modex_data_t *mdx;
    
    /* init the return */
    *val = NULL;

    /* unpack the status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (PMIX_SUCCESS != ret) {
        return ret;
    }
    
    /* we have received the entire data blob for this process - unpack
     * and cache all values, keeping the one we requested to return
     * to the caller */
    
    /* get the number of blobs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &np, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: unpacking %d blobs for key %s",
                        (int)np, (NULL == key) ? "NULL" : key);

    /* if data was returned, unpack and store it */
    if (0 < np) {
        mdx = (pmix_modex_data_t*)malloc(np * sizeof(pmix_modex_data_t));
        cnt = np;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, mdx, &cnt, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        for (i=0; i < np; i++) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "pmix: unpacked blob %d of size %d",
                                (int)i, (int)mdx[i].size);
            if (NULL == mdx[i].blob) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return PMIX_ERROR;
            }
            /* now unpack and store the values - everything goes into our internal store */
            PMIX_CONSTRUCT(&buf, pmix_buffer_t);
            PMIX_LOAD_BUFFER(&buf, mdx[i].blob, mdx[i].size);
            cnt = 1;
            kp = PMIX_NEW(pmix_kval_t);
            while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&buf, kp, &cnt, PMIX_KVAL))) {
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "pmix: unpacked key %s", kp->key);
                if (PMIX_SUCCESS != (rc = pmix_client_hash_store(mdx[i].nspace, mdx[i].rank, kp))) {
                    PMIX_ERROR_LOG(rc);
                }
                if (NULL != key && 0 == strcmp(key, kp->key)) {
                    pmix_output_verbose(2, pmix_globals.debug_output,
                                        "pmix: found requested value");
                    if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)val, kp->value, PMIX_VALUE))) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(kp);
                        return rc;
                    }
                }
                PMIX_RELEASE(kp); // maintain acctg - hash_store does a retain
                cnt = 1;
                kp = PMIX_NEW(pmix_kval_t);
            }
            PMIX_RELEASE(kp);
            PMIX_DESTRUCT(&buf);  // free's the data region
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
        free(mdx);
    }
    rc = PMIX_SUCCESS;

    return rc;
}

static void getnb_cbfunc(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc;
    pmix_value_t *val;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: get_nb callback recvd");

    rc = unpack_get_return(buf, cb->key, &val);

    /* if a callback was provided, execute it */
    if (NULL != cb && NULL != cb->value_cbfunc) {
        if( !rc && ( NULL == val ) ){
            rc = PMIX_ERR_NOT_FOUND;
        }
        cb->value_cbfunc(rc, val, cb->cbdata);
    }
    if( NULL != val ){
        PMIX_VALUE_RELEASE(val);
    }
    PMIX_RELEASE(cb);
}

static void getnb_shortcut(int fd, short flags, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_value_t val;
    int rc;
    int32_t m;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "getnb_shortcut called with %s cbfunc",
                        (NULL == cb->value_cbfunc) ? "NULL" : "NON-NULL");

    PMIX_VALUE_CONSTRUCT(&val);
    if (NULL != cb->value_cbfunc) {
        m=1;
        rc = pmix_bfrop.unpack(&cb->data, &val, &m, PMIX_VALUE);
        cb->value_cbfunc(rc, &val, cb->cbdata);
    }
    PMIX_VALUE_DESTRUCT(&val);
    PMIX_RELEASE(cb);
}
