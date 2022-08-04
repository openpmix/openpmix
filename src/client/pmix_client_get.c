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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix.h"

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

#include <event.h>

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_hash.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
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
    pmix_cb_t cb;
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get for %s key %s",
                        (NULL == proc) ? "NULL" : PMIX_NAME_PRINT(proc),
                        (NULL == key) ? "NULL" : key);

    /* create a callback object so we can be notified when
     * the non-blocking operation is complete */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    if (PMIX_SUCCESS != (rc = PMIx_Get_nb(proc, key, info, ninfo, _value_cbfunc, &cb))) {
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the data to return */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    if (NULL != val) {
        *val = cb.value;
        cb.value = NULL;
    }
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get completed");

    return rc;
}

static void gcbfn(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cb->cbfunc.valuefn(cb->status, cb->value, cb->cbdata);
    PMIX_RELEASE(cb);
}

static void quicklook(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_kval_t *kv;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* do the lookup */
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
    cb->status = rc;
    if(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        kv = (pmix_kval_t*)pmix_list_remove_first(&cb->kvs);
        cb->value = kv->value;
        kv->value = NULL;
        PMIX_RELEASE(kv);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Get_nb(const pmix_proc_t *proc, const char key[],
                                      const pmix_info_t info[], size_t ninfo,
                                      pmix_value_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb, cb2;
    pmix_status_t rc;
    size_t n, nfo;
    char *hostname = NULL;
    uint32_t sessionid = UINT32_MAX;
    uint32_t nodeid = UINT32_MAX;
    uint32_t appnum = UINT32_MAX;
    uint32_t app;
    pmix_proc_t p;
    pmix_info_t *iptr;
    bool copy = false;
    bool nodedirective = false;
    bool nodeinfo = false;
    bool appdirective = false;
    bool appinfo = false;
    bool sessiondirective = false;
    bool sessioninfo = false;
    bool refreshcache = false;
    pmix_value_t *ival = NULL;
    pmix_info_t optional;

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

    /* see if they just want their own process ID */
    if (NULL == proc && 0 == strncmp(key, PMIX_PROCID, PMIX_MAX_KEYLEN)) {
        PMIX_VALUE_CREATE(ival, 1);
        ival->type = PMIX_PROC;
        ival->data.proc = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
        PMIX_LOAD_PROCID(ival->data.proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        cb = PMIX_NEW(pmix_cb_t);
        cb->status = PMIX_SUCCESS;
        cb->value = ival;
        cb->cbfunc.valuefn = cbfunc;
        cb->cbdata = cbdata;
        PMIX_THREADSHIFT(cb, gcbfn);
        return PMIX_SUCCESS;
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
        PMIX_LOAD_NSPACE(p.nspace, pmix_globals.myid.nspace);
    } else {
        PMIX_LOAD_NSPACE(p.nspace, proc->nspace);
    }

    /* if the proc param is NULL, then we are seeking a key that
     * must be globally unique, so communicate this to the hash
     * functions with the UNDEF rank */
    if (NULL == proc) {
        p.rank = PMIX_RANK_UNDEF;
    } else {
        p.rank = proc->rank;
    }
    iptr = (pmix_info_t*)info;
    nfo = ninfo;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb value for proc %s key %s",
                        PMIX_NAME_PRINT(&p), (NULL == key) ? "NULL" : key);

    if (!PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100)) {
        /* if they passed our nspace and an INVALID rank, and are asking
         * for PMIX_RANK, then they are asking for our process rank */
        if (PMIX_RANK_INVALID == p.rank &&
            PMIX_CHECK_NSPACE(p.nspace, pmix_globals.myid.nspace) &&
            NULL != key && 0 == strncmp(key, PMIX_RANK, PMIX_MAX_KEYLEN)) {
            PMIX_VALUE_CREATE(ival, 1);
            if (NULL == ival) {
                return PMIX_ERR_NOMEM;
            }
            ival->type = PMIX_PROC_RANK;
            ival->data.rank = pmix_globals.myid.rank;
            /* threadshift to return the result - cannot call the
             * cbfunc from within the API */
            cb = PMIX_NEW(pmix_cb_t);
            cb->status = PMIX_SUCCESS;
            cb->value = ival;
            cb->cbfunc.valuefn = cbfunc;
            cb->cbdata = cbdata;
            PMIX_THREADSHIFT(cb, gcbfn);
            return PMIX_SUCCESS;
        }
        /* if the key is NULL, then move along */
        if (NULL == key) {
            goto doget;
        }
        /* see if they are asking about a specific type of info */
        if (pmix_check_node_info(key)) {
            nodeinfo = true;
        } else if (pmix_check_app_info(key)) {
            appinfo = true;
        } else if (pmix_check_session_info(key)) {
            sessioninfo = true;
        }
        for (n=0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_INFO)) {
                /* regardless of the default setting, they want us
                 * to get it from the job realm */
                nodeinfo = false;
                appinfo = false;
                sessioninfo = false;
                /* have to let the loop continue in case there are
                 * other relevant directives - e.g., refresh_cache */
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO)) {
                /* regardless of the default setting, they want us
                 * to get it from the node realm */
                nodedirective = true;
                nodeinfo = true;
                appinfo = false;
                sessioninfo = false;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_APP_INFO)) {
                /* regardless of the default setting, they want us
                 * to get it from the app realm */
                appdirective = true;
                appinfo = true;
                nodeinfo = false;
                sessioninfo = false;
            } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_INFO)) {
                /* regardless of the default setting, they want us
                 * to get it from the session realm */
                sessiondirective = true;
                sessioninfo = true;
                nodeinfo = false;
                appinfo = false;
            } else if (PMIX_CHECK_KEY(info, PMIX_GET_REFRESH_CACHE)) {
                refreshcache = true;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
                hostname = info[n].value.data.string;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID)) {
                PMIX_VALUE_GET_NUMBER(rc, &info[n].value, nodeid, uint32_t);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_APPNUM)) {
                PMIX_VALUE_GET_NUMBER(rc, &info[n].value, appnum, uint32_t);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_ID)) {
                PMIX_VALUE_GET_NUMBER(rc, &info[n].value, sessionid, uint32_t);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
        /* if they want us to refresh our cache, then we have
         * to go ask the server for it */
        if (refreshcache) {
            goto doget;
        }
        PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);

        if (nodeinfo) {
            pmix_output_verbose(2, pmix_client_globals.get_output,
                                "pmix: get_nb value requesting node-level info for proc %s key %s",
                                PMIX_NAME_PRINT(&p), (NULL == key) ? "NULL" : key);
            if (NULL == hostname && UINT32_MAX == nodeid) {
                /* if they didn't specify the target node, then see if they
                 * specified a proc */
                if (PMIX_RANK_IS_VALID(p.rank)) {
                    /* lookup the node where that proc is running - if all we
                     * have is the hash component, then we have to threadshift
                     * to make that request */
                    rc = _getfn_fastpath(&p, PMIX_HOSTNAME, &optional, 1, &ival);
                    if (PMIX_ERR_NOT_SUPPORTED == rc) {
                        /* all we have is hash */
                        PMIX_CONSTRUCT(&cb2, pmix_cb_t);
                        cb2.proc = &p;
                        cb2.key = PMIX_HOSTNAME;
                        cb2.info = &optional;
                        cb2.ninfo = 1;
                        PMIX_THREADSHIFT(&cb2, quicklook);
                        PMIX_WAIT_THREAD(&cb2.lock);
                        rc = cb2.status;
                        if (NULL != cb2.value) {
                            ival = cb2.value;
                            cb2.value = NULL;
                        }
                        PMIX_DESTRUCT(&cb2);
                    }
                    if (PMIX_SUCCESS == rc) {
                        hostname = ival->data.string;
                        /* if they were asking for the hostname, then we are done */
                        if (0 == strcmp(key, PMIX_HOSTNAME)) {
                            cb = PMIX_NEW(pmix_cb_t);
                            cb->status = PMIX_SUCCESS;
                            cb->value = ival;
                            cb->cbfunc.valuefn = cbfunc;
                            cb->cbdata = cbdata;
                            PMIX_THREADSHIFT(cb, gcbfn);
                            return PMIX_SUCCESS;
                        }
                        if (0 == strcmp(hostname, pmix_globals.hostname)) {
                            /* it is us - let dstore handle it */
                            PMIX_VALUE_RELEASE(ival);
                            goto fastpath;
                        }
                        /* mark this as a node info request */
                        if (nodedirective) {
                            /* just need to add the hostname */
                            nfo = ninfo + 2;
                            PMIX_INFO_CREATE(iptr, nfo);
                            for (n=0; n < ninfo; n++) {
                                PMIX_INFO_XFER(&iptr[n], &info[n]);
                            }
                            PMIX_INFO_LOAD(&iptr[ninfo], PMIX_HOSTNAME, hostname, PMIX_STRING);
                            PMIX_INFO_LOAD(&iptr[ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                            copy = true;
                        } else {
                            /* need to add directive and hostname */
                            nfo = ninfo + 3;
                            PMIX_INFO_CREATE(iptr, nfo);
                            for (n=0; n < ninfo; n++) {
                                PMIX_INFO_XFER(&iptr[n], &info[n]);
                            }
                            PMIX_INFO_LOAD(&iptr[ninfo], PMIX_NODE_INFO, NULL, PMIX_BOOL);
                            PMIX_INFO_LOAD(&iptr[ninfo + 1], PMIX_HOSTNAME, hostname, PMIX_STRING);
                            PMIX_INFO_LOAD(&iptr[ninfo+2], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                            copy = true;
                        }
                        PMIX_VALUE_RELEASE(ival);
                        /* proc is on identified host - let hash handle it */
                        p.rank = PMIX_RANK_UNDEF;
                        goto doget;
                    }
                    /* could not find hostname, so we cannot do anything */
                    return PMIX_ERR_NOT_FOUND;
                }
                /* nothing was specified, so assume our node and let dstore handle it */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }
            if (NULL != hostname && 0 == strcmp(hostname, pmix_globals.hostname)) {
                /* the node is us - let dstore handle it */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }
            if (UINT32_MAX != nodeid && nodeid == pmix_globals.nodeid) {
                /* the node is us - let dstore handle it */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }
            /* the node is not us - did they tell us the node? */
            if (NULL == hostname && UINT32_MAX == nodeid) {
                /* we have nothing and cannot find the host for
                 * an invalid rank */
                return PMIX_ERR_NOT_FOUND;
            }
            if (!nodedirective) {
                /* need to add directive - hostname and/or nodeid are already present */
                nfo = ninfo + 2;
                PMIX_INFO_CREATE(iptr, nfo);
                for (n=0; n < ninfo; n++) {
                    PMIX_INFO_XFER(&iptr[n], &info[n]);
                }
                PMIX_INFO_LOAD(&iptr[ninfo], PMIX_NODE_INFO, NULL, PMIX_BOOL);
                PMIX_INFO_LOAD(&iptr[ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                copy = true;
            }
            p.rank = PMIX_RANK_UNDEF;  // ensure invalid rank
            goto doget;
        }

        if (appinfo) {
            if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 2, 100)) {
               /* have to get this from the hash */
                if (!appdirective) {
                    /* didn't provide a directive - add it */
                    nfo = ninfo + 2;
                    PMIX_INFO_CREATE(iptr, nfo);
                    for (n=0; n < ninfo; n++) {
                        PMIX_INFO_XFER(&iptr[n], &info[n]);
                    }
                    PMIX_INFO_LOAD(&iptr[ninfo], PMIX_APP_INFO, NULL, PMIX_BOOL);
                    PMIX_INFO_LOAD(&iptr[ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                    copy = true;
                    goto doget;
                }
                goto doget;
            }
            if (appdirective) {
                if (UINT32_MAX != appnum) {
                    /* they provided an appnum - if it
                     * isn't our appnum, then we need to redirect */
                    rc = _getfn_fastpath(&pmix_globals.myid, PMIX_APPNUM, &optional, 1, &ival);
                    if (PMIX_SUCCESS == rc) {
                        PMIX_VALUE_GET_NUMBER(rc, ival, app, uint32_t);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_VALUE_RELEASE(ival);
                            return rc;
                        }
                        PMIX_VALUE_RELEASE(ival);
                        if (app == appnum) {
                            /* it is our appnum - info should be in dstore */
                            if (PMIX_RANK_UNDEF == p.rank) {
                                p.rank = PMIX_RANK_WILDCARD;
                            }
                            goto fastpath;
                        }
                    }
                    /* go get it from the hash */
                    goto doget;
                }
                /* missing the appnum - assume it is ours */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }

            if (UINT32_MAX != appnum) {
                /* they did not provide the "app-info" attribute but did specify
                 * the appnum - if the ID is other than us, then we just need to
                 * flag it as "app-info" and mark it for the undefined rank so
                 * the GDS will know where to look */
                rc = _getfn_fastpath(&pmix_globals.myid, PMIX_APPNUM, &optional, 1, &ival);
                if (PMIX_SUCCESS == rc) {
                    PMIX_VALUE_GET_NUMBER(rc, ival, app, uint32_t);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_VALUE_RELEASE(ival);
                        return rc;
                    }
                    PMIX_VALUE_RELEASE(ival);
                    if (app == appnum) {
                        /* it is our appnum - info is in dstore */
                        if (PMIX_RANK_UNDEF == p.rank) {
                            p.rank = PMIX_RANK_WILDCARD;
                        }
                        goto fastpath;
                    }
                }
                nfo = ninfo + 2;
                PMIX_INFO_CREATE(iptr, nfo);
                for (n=0; n < ninfo; n++) {
                    PMIX_INFO_XFER(&iptr[n], &info[n]);
                }
                PMIX_INFO_LOAD(&iptr[ninfo], PMIX_APP_INFO, NULL, PMIX_BOOL);
                PMIX_INFO_LOAD(&iptr[ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                copy = true;
                goto doget;
            } else {
                /* missing both - all we can do is assume they want our info */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }
        }

        if (sessioninfo) {
            if (sessiondirective) {
                if (UINT32_MAX != sessionid) {
                    /* they provided a session ID - if it
                     * isn't our session ID, then we need to redirect */
                    if (sessionid != pmix_globals.sessionid) {
                        /* go get it from the hash */
                        nfo = ninfo + 1;
                        PMIX_INFO_CREATE(iptr, nfo);
                        for (n=0; n < ninfo; n++) {
                            PMIX_INFO_XFER(&iptr[n], &info[n]);
                        }
                        PMIX_INFO_LOAD(&iptr[ninfo], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                        copy = true;
                        goto doget;
                    }
                }
                /* missing the session ID - assume it is ours */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }

            if (UINT32_MAX != sessionid) {
                /* they did not provide the "session-info" attribute but did specify
                 * the session ID - if the ID is other than us, then we just need to
                 * flag it as "session-info" and mark it for the undefined rank so
                 * the GDS will know where to look */
                if (sessionid != pmix_globals.sessionid) {
                    /* go get it from the hash */
                    nfo = ninfo + 2;
                    PMIX_INFO_CREATE(iptr, nfo);
                    for (n=0; n < ninfo; n++) {
                        PMIX_INFO_XFER(&iptr[n], &info[n]);
                    }
                    PMIX_INFO_LOAD(&iptr[ninfo], PMIX_SESSION_INFO, NULL, PMIX_BOOL);
                    PMIX_INFO_LOAD(&iptr[ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
                    copy = true;
                    goto doget;
                }
            } else {
                /* missing both - all we can do is assume they want our info */
                if (PMIX_RANK_UNDEF == p.rank) {
                    p.rank = PMIX_RANK_WILDCARD;
                }
                goto fastpath;
            }
        }
    }

    /* don't consider the fastpath option
     * for undefined rank or NULL keys */
    if (PMIX_RANK_UNDEF == p.rank || NULL == key) {
        goto doget;
    }

  fastpath:
    /* try to get data directly, without threadshift */
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb value trying fastpath for proc %s key %s",
                        PMIX_NAME_PRINT(&p), (NULL == key) ? "NULL" : key);
    if (PMIX_SUCCESS == (rc = _getfn_fastpath(&p, key, iptr, nfo, &ival))) {
        /* threadshift to return the result - cannot execute the cbfunc
         * from within the API */
        cb = PMIX_NEW(pmix_cb_t);
        cb->status = rc;
        cb->value = ival;
        cb->cbfunc.valuefn = cbfunc;
        cb->cbdata = cbdata;
        PMIX_THREADSHIFT(cb, gcbfn);
        return PMIX_SUCCESS;
    }

  doget:
    /* threadshift this request so we can access global structures */
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb value trying slowpath for proc %s key %s",
                        PMIX_NAME_PRINT(&p), (NULL == key) ? "NULL" : key);
    cb = PMIX_NEW(pmix_cb_t);
    cb->pname.nspace = strdup(p.nspace);
    cb->pname.rank = p.rank;
    cb->key = (char*)key;
    cb->info = iptr;
    cb->ninfo = nfo;
    cb->infocopy = copy;
    cb->cbfunc.valuefn = cbfunc;
    cb->cbdata = cbdata;
    PMIX_THREADSHIFT(cb, _getnbfn);

    return PMIX_SUCCESS;
}

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    if (PMIX_SUCCESS == status) {
        cb->value = kv;
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
    pmix_status_t rc, ret = PMIX_ERR_NOT_FOUND;
    pmix_value_t *val = NULL;
    int32_t cnt;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    bool diffnspace;

    PMIX_ACQUIRE_OBJECT(cb);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

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
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb server lost connection");
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
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb server returned %s for key %s from proc %s:%d",
                            PMIx_Error_string(ret), cb->key, cb->pname.nspace, cb->pname.rank);
        goto done;
    }
    PMIX_GDS_ACCEPT_KVS_RESP(rc, pmix_globals.mypeer, buf);

  done:
    /* now search any pending requests (including the one this was in
     * response to) to see if they can be met. Note that this function
     * will only be called if the user requested a specific key - we
     * don't support calls to "get" for a NULL key */
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb looking for requested key");
    PMIX_LIST_FOREACH_SAFE(cb, cb2, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (PMIX_CHECK_NSPACE(proc.nspace, cb->pname.nspace) && cb->pname.rank == proc.rank) {
            pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
            if (PMIX_SUCCESS != ret) {
                cb->cbfunc.valuefn(ret, NULL, cb->cbdata);
                PMIX_RELEASE(cb);
                continue;
            }
            /* we have the data for this proc - see if we can find the key */
            cb->proc = &proc;
            cb->scope = PMIX_SCOPE_UNDEF;
            /* fetch the data from server peer module - since it is passing
             * it back to the user, we need a copy of it */
            cb->copy = true;
            if (PMIX_RANK_UNDEF == proc.rank || diffnspace) {
                if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100)) {
                    /* everything is under rank=wildcard */
                    proc.rank = PMIX_RANK_WILDCARD;
                }
            }
            pmix_output_verbose(2, pmix_client_globals.get_output,
                                "pmix: get_nb searching for key %s for proc %s",
                                cb->key, PMIX_NAME_PRINT(&proc));
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
            if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
               /* if we are both using the "hash" component, then the server's peer
                * will simply be pointing at the same hash tables as my peer - no
                * no point in checking there again */
                if (0 != strcmp(pmix_globals.mypeer->nptr->compat.gds->name, pmix_client_globals.myserver->nptr->compat.gds->name)) {
                    pmix_output_verbose(2, pmix_client_globals.get_output,
                                        "pmix: get_nb searching for key %s for proc %s, - %s",
                                        cb->key, PMIX_NAME_PRINT(&proc), pmix_client_globals.myserver->nptr->compat.gds->name);
                    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
                }
            }
            if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                if (1 != pmix_list_get_size(&cb->kvs)) {
                    rc = PMIX_ERR_INVALID_VAL;
                    val = NULL;
                } else {
                    kv = (pmix_kval_t*)pmix_list_remove_first(&cb->kvs);
                    val = kv->value;
                    kv->value = NULL; // protect the value
                    PMIX_RELEASE(kv);
                }
                rc = PMIX_SUCCESS;
            }
            cb->cbfunc.valuefn(rc, val, cb->cbdata);
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
        kv = (pmix_kval_t*)pmix_list_get_first(kvs);
        *v = kv->value;
        kv->value = NULL;  // protect the value
        return PMIX_SUCCESS;
    }
    /* we will return the data as an array of pmix_info_t
     * in the kvs pmix_value_t */
    PMIX_VALUE_CREATE(val, 1);
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
        PMIx_Value_xfer(&info[n].value, kv->value);
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
    PMIX_HIDE_UNUSED_PARAMS(fd, flags);

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
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb looking for job info");
        cb->proc = &proc;
        /* fetch the data from my server's module - since we are passing
         * it back to the user, we need a copy of it */
        cb->copy = true;
        /* if the peer and server GDS component are the same, then no
         * point in trying it again */
        if (0 != strcmp(pmix_globals.mypeer->nptr->compat.gds->name, pmix_client_globals.myserver->nptr->compat.gds->name)) {
            PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
        } else {
            rc = PMIX_ERR_NOT_FOUND;
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
                /* => cb->key starts with pmix
                 * if the server is pre-v3.2, then we have to go up and ask
                 * for the info */
                if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100)) {
                    pmix_output_verbose(5, pmix_client_globals.get_output,
                                        "pmix:client old server - fetching data");
                    proc.rank = PMIX_RANK_WILDCARD;
                    goto request;
                } else if (PMIX_CHECK_KEY(cb, PMIX_GROUP_CONTEXT_ID)) {
                    goto request;
                }
                /* we should have had this info, so respond with the error - if
                 * they want us to check with the server, they should ask us to
                 * refresh the cache */
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
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb UNDEF rank");
        goto request;
    } else {
        /* if the peer and server GDS component are the same, then no
         * point in trying it again */
        if (0 == strcmp(pmix_globals.mypeer->nptr->compat.gds->name, pmix_client_globals.myserver->nptr->compat.gds->name)) {
            val = NULL;
            goto request;
        }
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb checking myserver");
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
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb responding with answer %s", PMIx_Error_string(rc));
    /* if a callback was provided, execute it */
    if (NULL != cb->cbfunc.valuefn) {
        if (NULL != val)  {
            PMIX_VALUE_COMPRESSED_STRING_UNPACK(val);
        }
        cb->cbfunc.valuefn(rc, val, cb->cbdata);
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
