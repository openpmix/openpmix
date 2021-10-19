/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2021 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include PMIX_EVENT_HEADER

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/threads.h"
#include "src/util/argv.h"
#include "src/util/construct_config.h"
#include "src/util/error.h"
#include "src/util/hash.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"

#include "pmix_client_ops.h"

static pmix_buffer_t *_pack_get(char *nspace, pmix_rank_t rank, char *key, const pmix_info_t info[],
                                size_t ninfo, pmix_cmd_t cmd);

static pmix_status_t get_data(const char *key, const pmix_info_t info[], size_t ninfo,
                              pmix_value_t **val, pmix_cb_t *cb);

static void _getnb_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                          void *cbdata);

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata);

static pmix_status_t process_values(pmix_value_t **v, pmix_cb_t *cb);

static pmix_status_t process_request(const pmix_proc_t *proc, const char key[],
                                     const pmix_info_t info[], size_t ninfo, pmix_get_logic_t *lg,
                                     pmix_value_t **val)
{
    pmix_value_t *ival;
    size_t n;
    pmix_status_t rc;

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

    /* see if they want a static response (i.e., they provided the storage),
     * a pointer to the answer, or an allocated storage object */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GET_POINTER_VALUES)) {
            /* they want the pointer */
            if (NULL == val) {
                return PMIX_ERR_BAD_PARAM;
            }
            lg->pntrval = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GET_STATIC_VALUES)) {
            /* they provided the storage */
            if (NULL == val || NULL == *val) {
                return PMIX_ERR_BAD_PARAM;
            }
            lg->stval = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_OPTIONAL)) {
            lg->optional = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_DATA_SCOPE)) {
            lg->scope = info[n].value.data.scope;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GET_REFRESH_CACHE)) {
            /* immediately query the server */
            lg->refresh_cache = PMIX_INFO_TRUE(&info[n]);
        }
    }

    /* see if they just want their own process ID */
    if (NULL == proc && 0 == strncmp(key, PMIX_PROCID, PMIX_MAX_KEYLEN)) {
        if (lg->stval) {
            ival = *val;
            ival->type = PMIX_PROC;
            ival->data.proc = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            PMIX_LOAD_PROCID(ival->data.proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        } else if (lg->pntrval) {
            (*val) = &pmix_globals.myidval;
        } else {
            PMIX_VALUE_CREATE(ival, 1);
            if (NULL == ival) {
                return PMIX_ERR_NOMEM;
            }
            ival->type = PMIX_PROC;
            ival->data.proc = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            PMIX_LOAD_PROCID(ival->data.proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
            *val = ival;
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    /* if they want the library's config, pass it back */
    if (NULL != key && 0 == strncmp(key, PMIX_CONFIGURATION, PMIX_MAX_KEYLEN)) {
        rc = pmix_util_construct_config(val, lg);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    /* if the given proc param is NULL, or the nspace is
     * empty, then the caller is referencing our own nspace */
    if (NULL == proc || 0 == strlen(proc->nspace)) {
        PMIX_LOAD_NSPACE(lg->p.nspace, pmix_globals.myid.nspace);
    } else {
        PMIX_LOAD_NSPACE(lg->p.nspace, proc->nspace);
    }
    /* if the proc param is NULL, then we are seeking a key that
     * must be globally unique, so communicate this to the hash
     * functions with the UNDEF rank */
    if (NULL == proc) {
        lg->p.rank = PMIX_RANK_UNDEF;
    } else {
        lg->p.rank = proc->rank;
    }

    /* if they passed our nspace and an INVALID rank, and are asking
     * for PMIX_RANK, then they are asking for our process rank */
    if (PMIX_RANK_INVALID == lg->p.rank && PMIX_CHECK_NSPACE(lg->p.nspace, pmix_globals.myid.nspace)
        && NULL != key && 0 == strncmp(key, PMIX_RANK, PMIX_MAX_KEYLEN)) {
        if (lg->stval) {
            ival = *val;
            ival->type = PMIX_PROC_RANK;
            ival->data.rank = pmix_globals.myid.rank;
        } else if (lg->pntrval) {
            (*val) = &pmix_globals.myrankval;
        } else {
            PMIX_VALUE_CREATE(ival, 1);
            if (NULL == ival) {
                return PMIX_ERR_NOMEM;
            }
            ival->type = PMIX_PROC_RANK;
            ival->data.rank = pmix_globals.myid.rank;
            *val = ival;
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    /* indicate that everything was okay */
    return PMIX_SUCCESS;
}

PMIX_EXPORT pmix_status_t PMIx_Get(const pmix_proc_t *proc, const char key[],
                                   const pmix_info_t info[], size_t ninfo, pmix_value_t **val)
{
    pmix_cb_t *cb;
    pmix_get_logic_t *lg;
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_client_globals.get_output, "pmix:client get for %s key %s",
                        (NULL == proc) ? "NULL" : PMIX_NAME_PRINT(proc),
                        (NULL == key) ? "NULL" : key);

    if (NULL != key && PMIX_MAX_KEYLEN < pmix_keylen(key)) {
        return PMIX_ERR_BAD_PARAM;
    }

    lg = PMIX_NEW(pmix_get_logic_t);
    rc = process_request(proc, key, info, ninfo, lg, val);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* the value has already been prepped */
        PMIX_RELEASE(lg);
        return PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != rc) {
        *val = NULL;
        PMIX_RELEASE(lg);
        return rc;
    }

    /* the request is good - let's go get the data */
    cb = PMIX_NEW(pmix_cb_t);
    cb->lg = lg;
    cb->cbfunc.valuefn = _value_cbfunc;
    PMIX_RETAIN(cb);
    cb->cbdata = cb;

    rc = get_data(key, info, ninfo, val, cb);

    if (PMIX_OPERATION_SUCCEEDED == rc) {
        PMIX_RELEASE(lg);
        PMIX_RELEASE(cb);
        return PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != rc) {
        *val = NULL;
        PMIX_RELEASE(lg);
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the data to be obtained */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS == rc && NULL != cb->value) {
        *val = cb->value;
        cb->value = NULL;
    } else {
        *val = NULL;
    }
    /* lg was released in the callback function */
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get completed with status %s", PMIx_Error_string(rc));

    return rc;
}

static void gcbfn(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cb->cbfunc.valuefn(cb->status, cb->value, cb->cbdata);
    PMIX_RELEASE(cb);
}

PMIX_EXPORT pmix_status_t PMIx_Get_nb(const pmix_proc_t *proc, const char key[],
                                      const pmix_info_t info[], size_t ninfo,
                                      pmix_value_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    pmix_get_logic_t *lg;
    pmix_value_t *val;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    if (NULL == cbfunc) {
        /* no way to return the result! */
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL != key && PMIX_MAX_KEYLEN < pmix_keylen(key)) {
        return PMIX_ERR_BAD_PARAM;
    }

    lg = PMIX_NEW(pmix_get_logic_t);
    rc = process_request(proc, key, info, ninfo, lg, &val);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* the value has already been prepped - threadshift to return result */
        cb = PMIX_NEW(pmix_cb_t);
        cb->status = PMIX_SUCCESS;
        cb->value = val;
        cb->cbfunc.valuefn = cbfunc;
        cb->cbdata = cbdata;
        PMIX_RELEASE(lg);
        PMIX_THREADSHIFT(cb, gcbfn);
        return PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != rc) {
        /* it's a true error */
        PMIX_RELEASE(lg);
        return rc;
    }

    /* the request is good - let's go get the data */
    cb = PMIX_NEW(pmix_cb_t);
    cb->lg = lg;
    cb->cbfunc.valuefn = cbfunc;
    cb->cbdata = cbdata;
    rc = get_data(key, info, ninfo, &val, cb);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* we were able to obtain the data atomically, but
         * we must threadshift to return it */
        cb->status = PMIX_SUCCESS;
        cb->value = val;
        cb->cbfunc.valuefn = cbfunc;
        cb->cbdata = cbdata;
        PMIX_RELEASE(lg);
        PMIX_THREADSHIFT(cb, gcbfn);
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(lg);
        PMIX_RELEASE(cb);
    }

    pmix_output_verbose(2, pmix_client_globals.get_output, "pmix:client get completed with %s",
                        PMIx_Error_string(rc));

    return rc;
}

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    if (PMIX_SUCCESS == status) {
        PMIX_BFROPS_COPY(rc, pmix_client_globals.myserver, (void **) &cb->value, kv, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static pmix_buffer_t *_pack_get(char *nspace, pmix_rank_t rank, char *key, const pmix_info_t info[],
                                size_t ninfo, pmix_cmd_t cmd)
{
    pmix_buffer_t *msg;
    pmix_status_t rc;

    /* nope - see if we can get it */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the get cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nspace, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the number of info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return NULL;
        }
    }
    if (NULL != key) {
        /* pack the key */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &key, 1, PMIX_STRING);
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
static void _getnb_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                          void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_cb_t *cb2;
    pmix_status_t rc, ret;
    pmix_value_t *val = NULL;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_get_logic_t *lg;

    PMIX_ACQUIRE_OBJECT(cb);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    pmix_output_verbose(2, pmix_client_globals.get_output, "pmix: get_nb callback recvd");

    if (NULL == cb) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    lg = cb->lg;

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb server lost connection");
        goto done;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
        PMIX_RELEASE(cb);
        return;
    }

    if (PMIX_SUCCESS != ret) {
        pmix_output_verbose(2, pmix_client_globals.get_output, "pmix: get_nb server returned %s",
                            PMIx_Error_string(ret));
        goto done;
    }
    /* store this into our GDS component associated
     * with the server - if it is the hash component,
     * the buffer will include a copy of the data. If
     * it is the shmem component, it will contain just
     * the memory address info */
    PMIX_GDS_ACCEPT_KVS_RESP(rc, pmix_globals.mypeer, buf);

done:
    /* now search any pending requests (including the one this was in
     * response to) to see if they can be met. Note that this function
     * will only be called if the user requested a specific key - we
     * don't support calls to "get" for a NULL key */
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb looking for requested key");
    PMIX_LIST_FOREACH_SAFE (cb, cb2, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (PMIX_CHECK_NSPACE(lg->p.nspace, cb->pname.nspace) && cb->pname.rank == lg->p.rank) {
            /* we have the data for this proc - see if we can find the key */
            cb->proc = &lg->p;
            cb->scope = PMIX_SCOPE_UNDEF;
            pmix_output_verbose(2, pmix_client_globals.get_output,
                                "pmix: get_nb searching for key %s for rank %s", cb->key,
                                PMIX_RANK_PRINT(cb->proc->rank));
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
            if (PMIX_OPERATION_SUCCEEDED == rc) {
                rc = PMIX_SUCCESS;
            }
            if (PMIX_SUCCESS == rc) {
                if (1 != pmix_list_get_size(&cb->kvs)) {
                    rc = PMIX_ERR_INVALID_VAL;
                    val = NULL;
                } else {
                    kv = (pmix_kval_t *) pmix_list_remove_first(&cb->kvs);
                    val = kv->value;
                    kv->value = NULL; // protect the value
                    PMIX_RELEASE(kv);
                }
            }
            cb->cbfunc.valuefn(rc, val, cb->cbdata);
            pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
            PMIX_RELEASE(lg);
            PMIX_RELEASE(cb);
        }
    }
}

static pmix_status_t process_values(pmix_value_t **v, pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    pmix_value_t *val;
    pmix_info_t *info;
    size_t ninfo, n;

    if (NULL != cb->key && 1 == pmix_list_get_size(kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(kvs);
        *v = kv->value;
        kv->value = NULL; // protect the value
        return PMIX_SUCCESS;
    }
    /* we will return the data as an array of pmix_info_t
     * in the kvs pmix_value_t */
    PMIX_VALUE_CREATE(val, 1);
    if (NULL == val) {
        return PMIX_ERR_NOMEM;
    }
    val->type = PMIX_DATA_ARRAY;
    val->data.darray = (pmix_data_array_t *) malloc(sizeof(pmix_data_array_t));
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
    n = 0;
    PMIX_LIST_FOREACH (kv, kvs, pmix_kval_t) {
        pmix_strncpy(info[n].key, kv->key, PMIX_MAX_KEYLEN);
        pmix_value_xfer(&info[n].value, kv->value);
        ++n;
    }
    val->data.darray->size = ninfo;
    val->data.darray->array = info;
    *v = val;
    return PMIX_SUCCESS;
}

static pmix_status_t get_data(const char *key, const pmix_info_t info[], size_t ninfo,
                              pmix_value_t **val, pmix_cb_t *cb)
{
    pmix_cb_t *cbret;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_get_logic_t *lg = cb->lg;

    pmix_output_verbose(2, pmix_client_globals.get_output, "pmix: getnbfn value for proc %s key %s",
                        PMIX_NAME_PRINT(&lg->p), (NULL == key) ? "NULL" : key);

    /* check the data provided to us by the server first */
    if (NULL != key) {
        cb->key = strdup(key);
    }
    cb->proc = &lg->p;
    cb->scope = lg->scope;
    cb->info = (pmix_info_t*)info;
    cb->ninfo = ninfo;

    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
    if (PMIX_SUCCESS == rc) {
        pmix_output_verbose(5, pmix_client_globals.get_output,
                            "pmix:client data found in server-provided data");
        rc = process_values(val, cb);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }
    pmix_output_verbose(5, pmix_client_globals.get_output,
                        "pmix:client data NOT found in server-provided data");

    /* if we are both using the "hash" component, then the server's peer
     * will simply be pointing at the same hash tables as my peer - no
     * no point in checking there again */
    if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
        /* check the data in my hash module */
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
        if (PMIX_SUCCESS == rc) {
            pmix_output_verbose(5, pmix_client_globals.get_output,
                                "pmix:client data found in internal hash data");
            rc = process_values(val, cb);
            if (PMIX_SUCCESS == rc) {
                rc = PMIX_OPERATION_SUCCEEDED;
            }
            return rc;
        }
    }
    pmix_output_verbose(5, pmix_client_globals.get_output, "pmix:client job-level data NOT found");

    /* we may wind up requesting the data using a different rank as an
     * indicator of the breadth of data we want, but we will need to
     * get the specific data someone requested later. So setup a tmp
     * process ID */
    memcpy(&proc, &lg->p, sizeof(pmix_proc_t));
    cb->pname.nspace = strdup(lg->p.nspace);
    cb->pname.rank = lg->p.rank;

    /* we didn't find the data in either the server or the internal hash
     * components. If this is a NULL or reserved key, then we do NOT go
     * up to the server unless special circumstances require it */
    if (NULL == cb->key || PMIX_CHECK_RESERVED_KEY(cb->key)) {
        /* if the server is pre-v3.2, or we are asking about the
         * job-level info from another namespace, then we have to
         * request the data */
        if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100)
            || !PMIX_CHECK_NSPACE(lg->p.nspace, pmix_globals.myid.nspace)) {
            /* flag that we want all of the job-level info */
            proc.rank = PMIX_RANK_WILDCARD;
        } else if (NULL != cb->key && !PMIX_CHECK_KEY(cb, PMIX_GROUP_CONTEXT_ID)) {
            /* this is a reserved key - we should have had this info, so
             * respond with the error - if they want us to check with the
             * server, they should ask us to refresh the cache */
            pmix_output_verbose(5, pmix_client_globals.get_output,
                                "pmix:client returning NOT FOUND error");
            return PMIX_ERR_NOT_FOUND;
        }
    }

    /* if we got here, then we don't have the data for this proc. If we
     * are a server, or we are a client and not connected, then there is
     * nothing more we can do */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)
        || (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) && !pmix_globals.connected)) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* we also have to check the user's directives to see if they do not want
     * us to attempt to retrieve it from the server */
    if (lg->optional) {
        /* they don't want us to try and retrieve it */
        pmix_output_verbose(
            2, pmix_client_globals.get_output,
            "PMIx_Get key=%s for rank = %u, namespace = %s was not found - request was optional",
            cb->key, cb->pname.rank, cb->pname.nspace);
        return PMIX_ERR_NOT_FOUND;
    }

    /* see if we already have a request in place with the server for data from
     * this nspace:rank. If we do, then no need to ask again as the
     * request will return _all_ data from that proc */
    PMIX_LIST_FOREACH (cbret, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (PMIX_CHECK_PROCID(&cbret->pname, &proc)) {
            /* we do have a pending request, but we still need to track this
             * outstanding request so we can satisfy it once the data is returned */
            pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
            return PMIX_SUCCESS; // indicate waiting for response
        }
    }

    /* we don't have a pending request, so let's create one */
    msg = _pack_get(cb->proc->nspace, proc.rank, cb->key, info, ninfo, PMIX_GETNB_CMD);
    if (NULL == msg) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "%s REQUESTING DATA FROM SERVER FOR %s:%s KEY %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), cb->proc->nspace,
                        PMIX_RANK_PRINT(proc.rank), cb->key);

    /* track the callback object */
    pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
    /* send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, _getnb_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
        return PMIX_ERROR;
    }
    /* we made a lot of changes to cb, so ensure they get
     * written out before we return */
    PMIX_POST_OBJECT(cb);
    return PMIX_SUCCESS;
}
