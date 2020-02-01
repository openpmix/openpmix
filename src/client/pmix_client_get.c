/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <src/include/pmix_stdint.h>

#include <pmix.h>
#include <pmix_rename.h>

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
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/threads.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/hash.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/mca/gds/gds.h"
#include "src/mca/ptl/base/base.h"

#include "pmix_client_ops.h"

static pmix_buffer_t* _pack_get(char *nspace, pmix_rank_t rank, char *key,
                               const pmix_info_t info[], size_t ninfo,
                               pmix_cmd_t cmd);

static void _getnbfn(int sd, short args, void *cbdata);

static void _getnb_cbfunc(struct pmix_peer_t *pr,
                          pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata);

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata);

static pmix_status_t _getfn_fastpath(const pmix_proc_t *proc, const pmix_key_t key,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_value_t **val);

static pmix_status_t process_values(pmix_value_t **v, pmix_cb_t *cb);


PMIX_EXPORT pmix_status_t PMIx_Get(const pmix_proc_t *proc,
                                   const pmix_key_t key,
                                   const pmix_info_t info[], size_t ninfo,
                                   pmix_value_t **val)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    size_t n, nfo;
    bool wantinfo = false;
    bool haveid = false;
    pmix_proc_t p;
    pmix_info_t lclinfo, *iptr;


    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    memcpy(&p, proc, sizeof(pmix_proc_t));
    iptr = (pmix_info_t*)info;
    nfo = ninfo;

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get for %s key %s",
                        (NULL == proc) ? "NULL" : PMIX_NAME_PRINT(proc),
                        (NULL == key) ? "NULL" : key);

    if (!PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 5)) {
        if (PMIX_RANK_UNDEF == proc->rank || NULL == key) {
            goto doget;
        }
        /* if they are asking about a node-level piece of info,
         * then the rank must be UNDEF */
        if (pmix_check_node_info(key)) {
            p.rank = PMIX_RANK_UNDEF;
            /* the key is node-related - see if the target node is in the info array */
            if (NULL != info) {
                for (n=0; n < ninfo; n++) {
                    if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO)) {
                        wantinfo = true;
                    } else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME) &&
                               0 != strcmp(info[n].value.data.string, pmix_globals.hostname)) {
                        haveid = true;
                    } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID) &&
                               info[n].value.data.uint32 != pmix_globals.nodeid) {
                        haveid = true;
                    }
                }
            }
            if (wantinfo && haveid) {
                goto doget;
            } else {
                /* guess not - better do it */
                PMIX_INFO_LOAD(&lclinfo, PMIX_NODE_INFO, NULL, PMIX_BOOL);
                iptr = &lclinfo;
                nfo = 1;
                goto doget;
            }
        }

        /* see if they are asking about an app-level piece of info */
        wantinfo = false;
        haveid = false;
        if (pmix_check_app_info(key)) {
            p.rank = PMIX_RANK_UNDEF;
            /* the key is app-related - see if the target app number is in the info array */
            if (NULL != info) {
                for (n=0; n < ninfo; n++) {
                    if (PMIX_CHECK_KEY(&info[n], PMIX_APP_INFO)) {
                        wantinfo = true;
                    } else if (PMIX_CHECK_KEY(&info[n], PMIX_APPNUM) &&
                               0 != info[n].value.data.uint32) {
                        haveid = true;
                    }
                }
            }
            if (wantinfo && haveid) {
                goto doget;
            } else {
                /* guess not - better do it */
                PMIX_INFO_LOAD(&lclinfo, PMIX_NODE_INFO, NULL, PMIX_BOOL);
                iptr = &lclinfo;
                nfo = 1;
                goto doget;
            }
        }

        /* see if they are requesting session info or requesting cache refresh */
        for (n=0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(info, PMIX_SESSION_INFO) ||
                PMIX_CHECK_KEY(info, PMIX_GET_REFRESH_CACHE)) {
                goto doget;
            }
        }
    }

    /* try to get data directly, without threadshift */
    if (PMIX_SUCCESS == (rc = _getfn_fastpath(&p, key, iptr, nfo, val))) {
        goto done;
    }

  doget:
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_SUCCESS != (rc = PMIx_Get_nb(&p, key, iptr, nfo, _value_cbfunc, cb))) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the data to return */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    if (NULL != val) {
        *val = cb->value;
        cb->value = NULL;
    }
    PMIX_RELEASE(cb);

  done:
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Get_nb(const pmix_proc_t *proc, const pmix_key_t key,
                                      const pmix_info_t info[], size_t ninfo,
                                      pmix_value_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb;
    int rank;
    char *nm;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if the proc is NULL, then the caller is assuming
     * that the key is universally unique within the caller's
     * own nspace. This most likely indicates that the code
     * was originally written for a legacy version of PMI.
     *
     * If the key is NULL, then the caller wants all
     * data from the specified proc. Again, this likely
     * indicates use of a legacy version of PMI.
     *
     * Either case is supported. However, we don't currently
     * support the case where -both- values are NULL */
    if (NULL == proc && NULL == key) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb value error - both proc and key are NULL");
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the key is NULL, the rank cannot be WILDCARD as
     * we cannot return all info from every rank */
    if (NULL != proc && PMIX_RANK_WILDCARD == proc->rank && NULL == key) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb value error - WILDCARD rank and key is NULL");
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the given proc param is NULL, or the nspace is
     * empty, then the caller is referencing our own nspace */
    if (NULL == proc || 0 == strlen(proc->nspace)) {
        nm = pmix_globals.myid.nspace;
    } else {
        nm = (char*)proc->nspace;
    }

    /* if the proc param is NULL, then we are seeking a key that
     * must be globally unique, so communicate this to the hash
     * functions with the UNDEF rank */
    if (NULL == proc) {
        rank = PMIX_RANK_UNDEF;
    } else {
        rank = proc->rank;
    }

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb value for proc %s:%u key %s",
                        nm, rank, (NULL == key) ? "NULL" : key);

    /* threadshift this request so we can access global structures */
    cb = PMIX_NEW(pmix_cb_t);
    cb->pname.nspace = strdup(nm);
    cb->pname.rank = rank;
    cb->key = (char*)key;
    cb->info = (pmix_info_t*)info;
    cb->ninfo = ninfo;
    cb->cbfunc.valuefn = cbfunc;
    cb->cbdata = cbdata;
    PMIX_THREADSHIFT(cb, _getnbfn);

    return PMIX_SUCCESS;
}

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    if (PMIX_SUCCESS == status) {
        PMIX_BFROPS_COPY(rc, pmix_client_globals.myserver,
                         (void**)&cb->value, kv, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static pmix_buffer_t* _pack_get(char *nspace, pmix_rank_t rank, char *key,
                               const pmix_info_t info[], size_t ninfo,
                               pmix_cmd_t cmd)
{
    pmix_buffer_t *msg;
    pmix_status_t rc;

    /* nope - see if we can get it */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the get cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &nspace, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the number of info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                     msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                         msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return NULL;
        }
    }
    if (NULL != key) {
        /* pack the key */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver,
                         msg, &key, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return NULL;
        }
    }

    return msg;
}


/* this callback is coming from the ptl recv, and thus
 * is occurring inside of our progress thread - hence, no
 * need to thread shift */
static void _getnb_cbfunc(struct pmix_peer_t *pr,
                          pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_cb_t *cb2;
    pmix_status_t rc, ret;
    pmix_value_t *val = NULL;
    int32_t cnt;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    bool diffnspace;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb callback recvd");

    if (NULL == cb) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }

    /* cache the proc id */
    pmix_strncpy(proc.nspace, cb->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cb->pname.rank;

    /* check for a different nspace */
    diffnspace = !PMIX_CHECK_NSPACE(pmix_globals.myid.nspace, proc.nspace);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        ret = PMIX_ERR_UNREACH;
        goto done;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver,
                       buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
        PMIX_RELEASE(cb);
        return;
    }

    if (PMIX_SUCCESS != ret) {
        goto done;
    }
    if (PMIX_RANK_UNDEF == proc.rank || diffnspace) {
        PMIX_GDS_ACCEPT_KVS_RESP(rc, pmix_globals.mypeer, buf);
    } else {
        PMIX_GDS_ACCEPT_KVS_RESP(rc, pmix_client_globals.myserver, buf);
    }
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

  done:
    /* now search any pending requests (including the one this was in
     * response to) to see if they can be met. Note that this function
     * will only be called if the user requested a specific key - we
     * don't support calls to "get" for a NULL key */
    PMIX_LIST_FOREACH_SAFE(cb, cb2, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (0 == strncmp(proc.nspace, cb->pname.nspace, PMIX_MAX_NSLEN) &&
            cb->pname.rank == proc.rank) {
           /* we have the data for this proc - see if we can find the key */
            cb->proc = &proc;
            cb->scope = PMIX_SCOPE_UNDEF;
            /* fetch the data from server peer module - since it is passing
             * it back to the user, we need a copy of it */
            cb->copy = true;
            if (PMIX_RANK_UNDEF == proc.rank || diffnspace) {
                if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 5)) {
                    /* everything is under rank=wildcard */
                    proc.rank = PMIX_RANK_WILDCARD;
                }
                PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
            } else {
                if (PMIX_RANK_UNDEF == proc.rank &&
                    PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 5)) {
                    /* everything is under rank=wildcard */
                    proc.rank = PMIX_RANK_WILDCARD;
                }
                PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
            }
            if (PMIX_SUCCESS == rc) {
                if (1 != pmix_list_get_size(&cb->kvs)) {
                    rc = PMIX_ERR_INVALID_VAL;
                    val = NULL;
                } else {
                    kv = (pmix_kval_t*)pmix_list_remove_first(&cb->kvs);
                    val = kv->value;
                    kv->value = NULL; // protect the value
                    PMIX_RELEASE(kv);
                }
            }
            cb->cbfunc.valuefn(rc, val, cb->cbdata);
            pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
            PMIX_RELEASE(cb);
        }
    }
}

static void timeout(int fd, short flags, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    /* let them know that we timed out */
    cb->cbfunc.valuefn(PMIX_ERR_TIMEOUT, NULL, cb->cbdata);
    cb->timer_running = false;

    /* remove this request */
    pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
    PMIX_RELEASE(cb);
}

static pmix_status_t process_values(pmix_value_t **v, pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    pmix_value_t *val;
    pmix_info_t *info;
    size_t ninfo, n;

    if (NULL != cb->key && 1 == pmix_list_get_size(kvs)) {
        kv = (pmix_kval_t*)pmix_list_get_first(kvs);
        *v = kv->value;
        kv->value = NULL;  // protect the value
        return PMIX_SUCCESS;
    }
    /* we will return the data as an array of pmix_info_t
     * in the kvs pmix_value_t */
    val = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == val) {
        return PMIX_ERR_NOMEM;
    }
    val->type = PMIX_DATA_ARRAY;
    val->data.darray = (pmix_data_array_t*)malloc(sizeof(pmix_data_array_t));
    if (NULL == val->data.darray) {
        PMIX_VALUE_RELEASE(val);
        return PMIX_ERR_NOMEM;
    }
    val->data.darray->type = PMIX_INFO;
    val->data.darray->size = 0;
    val->data.darray->array = NULL;
    ninfo = pmix_list_get_size(kvs);
    PMIX_INFO_CREATE(info, ninfo);
    if (NULL == info) {
        PMIX_VALUE_RELEASE(val);
        return PMIX_ERR_NOMEM;
    }
    /* copy the list elements */
    n=0;
    PMIX_LIST_FOREACH(kv, kvs, pmix_kval_t) {
        pmix_strncpy(info[n].key, kv->key, PMIX_MAX_KEYLEN);
        pmix_value_xfer(&info[n].value, kv->value);
        ++n;
    }
    val->data.darray->size = ninfo;
    val->data.darray->array = info;
    *v = val;
    return PMIX_SUCCESS;
}

static pmix_status_t _getfn_fastpath(const pmix_proc_t *proc, const pmix_key_t key,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_value_t **val)
{
    pmix_cb_t cb;
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t*)proc;
    cb.copy = true;
    cb.key = (char*)key;
    cb.info = (pmix_info_t*)info;
    cb.ninfo = ninfo;

    PMIX_GDS_FETCH_IS_TSAFE(rc, pmix_client_globals.myserver);
    if (PMIX_SUCCESS == rc) {
        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
        if (PMIX_SUCCESS == rc) {
            goto done;
        }
    }
    PMIX_GDS_FETCH_IS_TSAFE(rc, pmix_globals.mypeer);
    if (PMIX_SUCCESS == rc) {
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS == rc) {
            goto done;
        }
    }
    PMIX_DESTRUCT(&cb);
    return rc;

  done:
    rc = process_values(val, &cb);
    if (NULL != *val) {
        PMIX_VALUE_COMPRESSED_STRING_UNPACK(*val);
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}

static void _getnbfn(int fd, short flags, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_cb_t *cbret;
    pmix_buffer_t *msg;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    size_t n;
    pmix_proc_t proc;
    bool optional = false;
    bool internal_only = false;

    /* cb was passed to us from another thread - acquire it */
    PMIX_ACQUIRE_OBJECT(cb);

    /* set the proc object identifier */
    PMIX_LOAD_PROCID(&proc, cb->pname.nspace, cb->pname.rank);
    cb->proc = &proc;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: getnbfn value for proc %s key %s",
                        PMIX_NAME_PRINT(&proc),
                        (NULL == cb->key) ? "NULL" : cb->key);

    /* scan the incoming directives */
    if (NULL != cb->info) {
        for (n=0; n < cb->ninfo; n++) {
            if (PMIX_CHECK_KEY(&cb->info[n], PMIX_OPTIONAL)) {
                optional = PMIX_INFO_TRUE(&cb->info[n]);
            } else if (PMIX_CHECK_KEY(&cb->info[n], PMIX_DATA_SCOPE)) {
                cb->scope = cb->info[n].value.data.scope;
            } else if (PMIX_CHECK_KEY(&cb->info[n], PMIX_NODE_INFO) ||
                       PMIX_CHECK_KEY(&cb->info[n], PMIX_APP_INFO) ||
                       PMIX_CHECK_KEY(&cb->info[n], PMIX_SESSION_INFO)) {
                internal_only = true;
            } else if (PMIX_CHECK_KEY(&cb->info[n], PMIX_GET_REFRESH_CACHE)) {
                /* immediately query the server */
                goto request;
            }
        }
    }

    /* check the internal storage first */
    cb->proc = &proc;
    cb->copy = true;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
    if (PMIX_SUCCESS == rc) {
        pmix_output_verbose(5, pmix_client_globals.get_output,
                            "pmix:client data found in internal storage");
        rc = process_values(&val, cb);
        goto respond;
    }
    pmix_output_verbose(5, pmix_client_globals.get_output,
                        "pmix:client data NOT found in internal storage");

    /* if the key is NULL or starts with "pmix", then they are looking
     * for data that was provided by the server at startup */
    if (!internal_only && (NULL == cb->key || 0 == strncmp(cb->key, "pmix", 4))) {
        cb->proc = &proc;
        /* fetch the data from my server's module - since we are passing
         * it back to the user, we need a copy of it */
        cb->copy = true;
        /* if the peer and server GDS component are the same, then no
         * point in trying it again */
        if (0 != strcmp(pmix_globals.mypeer->nptr->compat.gds->name, pmix_client_globals.myserver->nptr->compat.gds->name)) {
            PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
        } else {
            rc = PMIX_ERR_TAKE_NEXT_OPTION;
        }
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(5, pmix_client_globals.get_output,
                                "pmix:client job-level data NOT found");
            if (!PMIX_CHECK_NSPACE(cb->pname.nspace, pmix_globals.myid.nspace)) {
                /* we are asking about the job-level info from another
                 * namespace. It seems that we don't have it - go and
                 * ask server and indicate we only need job-level info
                 * by setting the rank to WILDCARD
                 */
                proc.rank = PMIX_RANK_WILDCARD;
                goto request;
            } else if (NULL != cb->key) {
                /* if immediate was given, then we are being directed to
                 * check with the server even though the caller is looking for
                 * job-level info. In some cases, a server may elect not
                 * to provide info at init to save memory */
                if (!optional) {
                    pmix_output_verbose(5, pmix_client_globals.get_output,
                                        "pmix:client not optional - requesting data");
                    goto request;
                }
                /* we should have had this info, so respond with the error */
                pmix_output_verbose(5, pmix_client_globals.get_output,
                                    "pmix:client returning NOT FOUND error");
                goto respond;
            } else {
                pmix_output_verbose(5, pmix_client_globals.get_output,
                                    "pmix:client NULL KEY - returning error");
                goto respond;
            }
        }
        pmix_output_verbose(5, pmix_client_globals.get_output,
                            "pmix:client job-level data found");
        rc = process_values(&val, cb);
        goto respond;
    } else if (PMIX_RANK_UNDEF == proc.rank) {
        /* the data would have to be stored on our own peer, so
         * we need to go request it */
        goto request;
    } else {
        /* if the peer and server GDS component are the same, then no
         * point in trying it again */
        if (0 == strcmp(pmix_globals.mypeer->nptr->compat.gds->name, pmix_client_globals.myserver->nptr->compat.gds->name)) {
            val = NULL;
            goto request;
        }
        cb->proc = &proc;
        cb->copy = true;
        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
        if (PMIX_SUCCESS != rc) {
            val = NULL;
            goto request;
        }
        /* return whatever we found */
        rc = process_values(&val, cb);
        if (PMIX_SUCCESS != rc) {
            goto request;
        }
    }

  respond:
    /* if a callback was provided, execute it */
    if (NULL != cb->cbfunc.valuefn) {
        if (NULL != val)  {
            PMIX_VALUE_COMPRESSED_STRING_UNPACK(val);
        }
        cb->cbfunc.valuefn(rc, val, cb->cbdata);
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    PMIX_RELEASE(cb);
    return;

  request:
    /* if we got here, then we don't have the data for this proc. If we
     * are a server, or we are a client and not connected, then there is
     * nothing more we can do */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
        (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) && !pmix_globals.connected)) {
        rc = PMIX_ERR_NOT_FOUND;
        goto respond;
    }

    /* we also have to check the user's directives to see if they do not want
     * us to attempt to retrieve it from the server */
    if (optional) {
        /* they don't want us to try and retrieve it */
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "PMIx_Get key=%s for rank = %u, namespace = %s was not found - request was optional",
                            cb->key, cb->pname.rank, cb->pname.nspace);
        rc = PMIX_ERR_NOT_FOUND;
        goto respond;
    }

    /* see if we already have a request in place with the server for data from
     * this nspace:rank. If we do, then no need to ask again as the
     * request will return _all_ data from that proc */
    PMIX_LIST_FOREACH(cbret, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (PMIX_CHECK_PROCID(&cbret->pname, &cb->pname)) {
            /* we do have a pending request, but we still need to track this
             * outstanding request so we can satisfy it once the data is returned */
            pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
            return;
        }
    }

    /* we don't have a pending request, so let's create one */
    msg = _pack_get(cb->proc->nspace, proc.rank, cb->key, cb->info, cb->ninfo, PMIX_GETNB_CMD);
    if (NULL == msg) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        goto respond;
    }

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "%s REQUESTING DATA FROM SERVER FOR %s:%s KEY %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        cb->proc->nspace, PMIX_RANK_PRINT(proc.rank), cb->key);

    /* track the callback object */
    pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
    /* send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, _getnb_cbfunc, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
        rc = PMIX_ERROR;
        goto respond;
    }
    /* we made a lot of changes to cb, so ensure they get
     * written out before we return */
    PMIX_POST_OBJECT(cb);
    return;
}
