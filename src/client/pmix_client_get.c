/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2021 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#include <event.h>

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_client_ops.h"

static pmix_buffer_t *_pack_get(pmix_cb_t *cb,
                                pmix_rank_t rank,
                                pmix_cmd_t cmd);

static void get_data(int sd, short args, void *cbdata);

static void _getnb_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata);

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata);

static pmix_status_t process_values(pmix_cb_t *cb);

static pmix_status_t refresh_cache(void);

static pmix_status_t process_request(const pmix_proc_t *proc, const char key[],
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_get_logic_t *lg, pmix_value_t **val)
{
    pmix_value_t *ival;
    size_t n;

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
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_IMMEDIATE)) {
            lg->immediate = PMIX_INFO_TRUE(&info[n]);
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

    /* if we are to refresh the cache, go do that */
    if (lg->refresh_cache) {
        rc = refresh_cache();
        if (PMIX_SUCCESS != rc) {
            // couldn't refresh for some reason
            PMIX_RELEASE(lg);
            return rc;
        }
    }

    /* the request is good - let's go get the data */
    cb = PMIX_NEW(pmix_cb_t);
    cb->lg = lg;
    cb->key = (char*)key;
    cb->info = (pmix_info_t*)info;
    cb->ninfo = ninfo;
    cb->cbfunc.valuefn = _value_cbfunc;
    PMIX_RETAIN(cb);
    cb->cbdata = cb;

    /* MUST threadshift here to avoid touching global
     * data while in the user's thread */
    PMIX_THREADSHIFT(cb, get_data);

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

    /* if we are to refresh the cache, go do that */
    if (lg->refresh_cache) {
        rc = refresh_cache();
        if (PMIX_SUCCESS != rc) {
            // couldn't refresh for some reason
            PMIX_RELEASE(lg);
            return rc;
        }
    }

    /* the request is good - let's go get the data */
    cb = PMIX_NEW(pmix_cb_t);
    cb->lg = lg;
    cb->key = (char*)key;
    cb->info = (pmix_info_t*)info;
    cb->ninfo = ninfo;
    cb->scope = lg->scope;
    cb->cbfunc.valuefn = cbfunc;
    cb->cbdata = cbdata;
    // flag that we need to use an intermediate return point
    cb->checked = true;

    /* MUST threadshift here to avoid touching global
     * data while in the user's thread */
    PMIX_THREADSHIFT(cb, get_data);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get_nb in progress");

    return rc;
}

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cb);
    cb = (pmix_cb_t *) cbdata;
    cb->status = status;
    if (PMIX_SUCCESS == status) {
        PMIX_BFROPS_COPY(rc, pmix_client_globals.myserver, (void **)&cb->value, kv, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static pmix_buffer_t *_pack_get(pmix_cb_t *cb,
                                pmix_rank_t rank,
                                pmix_cmd_t cmd)
{
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_info_t *immediate;
    size_t n, nimm;
    char *nspace = cb->proc->nspace;

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
    if (cb->lg->add_immediate) {
        nimm = cb->ninfo + 1;
        PMIX_INFO_CREATE(immediate, nimm);
        for (n=0; n < cb->ninfo; n++) {
            PMIX_INFO_XFER(&immediate[n], &cb->info[n]);
        }
        PMIX_INFO_LOAD(&immediate[n], PMIX_IMMEDIATE, NULL, PMIX_BOOL);
        cb->info = immediate;
        cb->ninfo = nimm;
        cb->infocopy = true;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cb->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    if (0 < cb->ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cb->info, cb->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return NULL;
        }
    }
    if (NULL != cb->key) {
        /* pack the key */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cb->key, 1, PMIX_STRING);
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
static void _getnb_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_cb_t *cb2;
    pmix_status_t rc, ret = PMIX_ERR_NOT_FOUND;
    pmix_value_t *val = NULL;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_get_logic_t *lg;

    PMIX_ACQUIRE_OBJECT(cb);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb callback recvd");

    if (NULL == cb || NULL == cb->lg) {
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
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb server returned %s",
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
            pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
            if (PMIX_SUCCESS != ret) {
                cb->cbfunc.valuefn(ret, NULL, cb->cbdata);
                PMIX_RELEASE(cb);
                continue;
            }
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
            else if (PMIX_SUCCESS != rc) {
               /* if we are both using the "hash" component, then the server's peer
                * will simply be pointing at the same hash tables as my peer - no
                * no point in checking there again */
               if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
                    pmix_output_verbose(2, pmix_client_globals.get_output,
                                        "pmix: get_nb searching for key %s for proc %s, - %s",
                                        cb->key, PMIX_NAME_PRINT(cb->proc), pmix_client_globals.myserver->nptr->compat.gds->name);
                    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
                    if (PMIX_OPERATION_SUCCEEDED == rc) {
                        rc = PMIX_SUCCESS;
                    }
               }
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
            PMIX_RELEASE(cb);
        }
    }
}

static pmix_status_t process_values(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    pmix_value_t *val;
    pmix_info_t *info;
    size_t ninfo, n;

    if (NULL != cb->key && 1 == pmix_list_get_size(kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(kvs);
        cb->value = kv->value;
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
        PMIx_Value_xfer(&info[n].value, kv->value);
        ++n;
    }
    val->data.darray->size = ninfo;
    val->data.darray->array = info;
    cb->value = val;
    return PMIX_SUCCESS;
}

static void get_data(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_cb_t *cbret;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_get_logic_t *lg;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cb);
    cb = (pmix_cb_t*)cbdata;
    lg = cb->lg;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client:get_data value for proc %s key %s",
                        PMIX_NAME_PRINT(&lg->p), (NULL == cb->key) ? "NULL" : cb->key);

    /* check the data provided to us by the server first */
    cb->proc = &lg->p;
    cb->scope = lg->scope;

    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
    if (PMIX_SUCCESS == rc) {
        pmix_output_verbose(5, pmix_client_globals.get_output,
                            "pmix:client data found in server-provided data");
        cb->status = process_values(cb);
        goto done;
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
            cb->status = process_values(cb);
            goto done;
        }
    }
    pmix_output_verbose(5, pmix_client_globals.get_output,
                        "pmix:client job-level data NOT found");

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
        if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100) ||
            !PMIX_CHECK_NSPACE(lg->p.nspace, pmix_globals.myid.nspace)) {
            /* flag that we want all of the job-level info */
            proc.rank = PMIX_RANK_WILDCARD;
        } else if (NULL != cb->key) {
            /* this is a reserved key - we should have had this info, but
             * it is possible that some system don't provide it, thereby
             * causing the app to manually distribute it. We will therefore
             * request it from the server, but do so under the "immediate"
             * use-case, adding that flag if they didn't already include it
             */
            pmix_output_verbose(5, pmix_client_globals.get_output,
                                "pmix:client reserved key not locally found");
            if (!lg->immediate) {
                lg->add_immediate = true;
            }
        }
    }

    /* if we got here, then we don't have the data for this proc. If we
     * are a server, or we are a client and not connected, then there is
     * nothing more we can do */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
        (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) && !pmix_globals.connected)) {
        cb->status = PMIX_ERR_NOT_FOUND;
        goto done;
    }

    /* since we are looking for a non-reserved key, check to see if we already
     * have the data for this proc - if we do, then no point in asking for
     * it again */
    if (PMIX_ERR_EXISTS_OUTSIDE_SCOPE == rc) {
        cb->status = rc;
        goto done;
    }

    /* we also have to check the user's directives to see if they do not want
     * us to attempt to retrieve it from the server */
    if (lg->optional) {
        /* they don't want us to try and retrieve it */
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "PMIx_Get key=%s for rank = %u, namespace = %s was not found - request was optional",
                            cb->key, cb->pname.rank, cb->pname.nspace);
        cb->status = PMIX_ERR_NOT_FOUND;
        goto done;
    }

    /* see if we already have a request in place with the server for data from
     * this nspace:rank. If we do, then no need to ask again as the
     * request will return _all_ data from that proc */
    PMIX_LIST_FOREACH (cbret, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (PMIX_CHECK_PROCID(&cbret->pname, &proc)) {
            /* we do have a pending request, but we still need to track this
             * outstanding request so we can satisfy it once the data is returned */
            pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
            cb->status = PMIX_SUCCESS; // indicate waiting for response
            goto done;
        }
    }

    /* we don't have a pending request, so let's create one */
    msg = _pack_get(cb, proc.rank, PMIX_GETNB_CMD);
    if (NULL == msg) {
        cb->status = PMIX_ERROR;
        PMIX_ERROR_LOG(cb->status);
        goto done;
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
        cb->status = PMIX_ERROR;
        goto done;
    }
    return;

done:
    /* we made a lot of changes to cb, so ensure they get
     * written out before we return */
    PMIX_POST_OBJECT(cb);
    if (cb->checked) {
        gcbfn(0, 0, cb);
    } else {
        cb->cbfunc.valuefn(cb->status, cb->value, cb->cbdata);
    }
    return;
}

static void refcb(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                  pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    int32_t cnt;
    pmix_status_t rc, ret;

    PMIX_ACQUIRE_OBJECT(cb);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    if (NULL == cb) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: refcb server lost connection");
        ret = PMIX_ERR_LOST_CONNECTION;
        goto done;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

done:
    cb->status = ret;
    /* release the lock */
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
    return;
}

static pmix_status_t refresh_cache(void)
{
    pmix_cb_t cb;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_cmd_t cmd = PMIX_REFRESH_CACHE;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "%s REQUESTING CACHE REFRESH BY SERVER",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* pack a quick message to the server asking it
     * to refresh our cache */
    msg = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    /* send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, refcb, (void *)&cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    PMIX_DESTRUCT(&cb);
    return rc;
}
