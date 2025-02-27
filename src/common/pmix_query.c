/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix.h"
#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/common/pmix_attributes.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

static void relcbfunc(void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                "pmix:query release callback");

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

static void query_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_status_t rc, status;
    int cnt;
    pmix_info_t *info;
    size_t n, ninfo;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:query cback from server");

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (PMIX_SUCCESS != status &&
        PMIX_ERR_PARTIAL_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    /* unpack any returned data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        status = rc;
        goto complete;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            status = rc;
            goto complete;
        }
        /* add the results to our cache */
        for (n = 0; n < ninfo; n++) {
            PMIX_KVAL_NEW(kv, info[n].key);
            PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kv->value, &info[n].value);
            pmix_list_append(&cd->results, &kv->super);
        }
        PMIX_INFO_FREE(info, ninfo);
    }

complete:
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:query cback from server releasing with status %s",
                        PMIx_Error_string(status));
    /* release the caller */
    if (NULL != cd->cbfunc) {
        if (0 < pmix_list_get_size(&cd->results)) {
            cd->ninfo = pmix_list_get_size(&cd->results);
            PMIX_INFO_CREATE(cd->info, cd->ninfo);
            n = 0;
            PMIX_LIST_FOREACH(kv, &cd->results, pmix_kval_t) {
                PMIX_LOAD_KEY(cd->info[n].key, kv->key);
                PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, &cd->info[n].value, kv->value);
                if (PMIX_SUCCESS != rc) {
                    status = rc;
                }
                ++n;
            }
        }
        cd->cbfunc(status, cd->info, cd->ninfo, cd->cbdata, relcbfunc, cd);
    } else {
        PMIX_RELEASE(cd);
    }
}

static void qinfocb(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    size_t n;

    // transfer the results
    cd->status = status;
    if (0 < ninfo) {
        cd->ninfo = ninfo;
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cd->info[n], &info[n]);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    PMIX_WAKEUP_THREAD(&cd->lock);
}

static pmix_status_t send_for_help(pmix_query_caddy_t *cd)
{
    pmix_cmd_t cmd = PMIX_QUERY_CMD;
    pmix_buffer_t *msg;
    pmix_status_t rc;

    msg = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cd);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cd->nqueries, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cd->queries, cd->nqueries, PMIX_QUERY);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:query sending to server");
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, query_cbfunc, (void *) cd);
    return rc;
}

static void finalstep(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                      pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t*)cbdata;
    pmix_status_t rc, lstat = status;
    size_t n;
    pmix_kval_t *kv;

    // transfer any returned answers
    for (n = 0; n < ninfo; n++) {
        PMIX_KVAL_NEW(kv, info[n].key);
        PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kv->value, &info[n].value);
        pmix_list_append(&cd->results, &kv->super);
    }

    /* construct the final response */
    if (NULL != cd->cbfunc) {
        if (0 < pmix_list_get_size(&cd->results)) {
            cd->ninfo = pmix_list_get_size(&cd->results);
            PMIX_INFO_CREATE(cd->info, cd->ninfo);
            n = 0;
            PMIX_LIST_FOREACH(kv, &cd->results, pmix_kval_t) {
                PMIX_LOAD_KEY(cd->info[n].key, kv->key);
                PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, &cd->info[n].value, kv->value);
                if (PMIX_SUCCESS != rc) {
                    lstat = rc;
                }
                ++n;
            }
        }
        cd->cbfunc(lstat, cd->info, cd->ninfo, cd->cbdata, relcbfunc, cd);
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    return;
}

static pmix_status_t request_help(pmix_query_caddy_t *cd)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    /* if our host has support, then we just issue the query and
     * return the response - but don't pass it back to the host
     * is the host is a server as that would be a loopback */
    if (!cd->host_called && NULL != pmix_host_server.query) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:query handed to RM");
        cd->host_called = true;
        rc = pmix_host_server.query(&pmix_globals.myid,
                                    cd->queries, cd->nqueries,
                                    finalstep, (void*)cd);
        return rc;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_globals.connected) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_UNREACH;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    rc = send_for_help(cd);

    return rc;
}

static void _local_relcb(void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

void pmix_parse_localquery(int sd, short args, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_query_t *queries = cd->queries;
    size_t nqueries = cd->nqueries;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_cb_t cb;
    size_t n, p, m;
    pmix_list_t unresolved;
    pmix_kval_t *kv, *kvnxt;
    pmix_proc_t proc;
    bool rank_given;
    pmix_querylist_t *qry;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // setup the list of unresolved queries
    PMIX_CONSTRUCT(&unresolved, pmix_list_t);
    for (n = 0; n < nqueries; n++) {
        rank_given = false;
        PMIX_LOAD_PROCID(&proc, NULL, PMIX_RANK_INVALID);
        for (p = 0; p < queries[n].nqual; p++) {
            if (PMIX_CHECK_KEY(&queries[n].qualifiers[p], PMIX_PROCID)) {
                PMIX_LOAD_NSPACE(proc.nspace, queries[n].qualifiers[p].value.data.proc->nspace);
                proc.rank = queries[n].qualifiers[p].value.data.proc->rank;
                rank_given = true;
            } else if (PMIX_CHECK_KEY(&queries[n].qualifiers[p], PMIX_NSPACE)) {
                PMIX_LOAD_NSPACE(proc.nspace, queries[n].qualifiers[p].value.data.string);
            } else if (PMIX_CHECK_KEY(&queries[n].qualifiers[p], PMIX_RANK)) {
                proc.rank = queries[n].qualifiers[p].value.data.rank;
                rank_given = true;
            }
        }

        /* setup to try a local "get" on the data to see if we already have it */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.copy = false;
        /* if they are querying about node or app values not directly
         * associated with a proc (i.e., they didn't specify the proc),
         * then we obtain those by leaving the proc info as undefined */
        if (!rank_given) {
            proc.rank = PMIX_RANK_UNDEF;
            cb.proc = &proc;
        } else {
            /* set the proc */
            if (PMIX_RANK_INVALID == proc.rank && 0 == strlen(proc.nspace)) {
                /* use our id */
                cb.proc = &pmix_globals.myid;
            } else {
                if (0 == strlen(proc.nspace)) {
                    /* use our nspace */
                    PMIX_LOAD_NSPACE(cb.proc->nspace, pmix_globals.myid.nspace);
                }
                if (PMIX_RANK_INVALID == proc.rank) {
                    /* user the wildcard rank */
                    proc.rank = PMIX_RANK_WILDCARD;
                }
                cb.proc = &proc;
            }
        }

        /* see if we already have this info */
        for (p = 0; NULL != queries[n].keys[p]; p++) {
            cb.key = queries[n].keys[p];
            // Locally resolvable keys
            if (0 == strcmp(queries[n].keys[p], PMIX_QUERY_STABLE_ABI_VERSION)) {
                PMIX_KVAL_NEW(kv, cb.key);
                PMIx_Value_load(kv->value, PMIX_STD_ABI_STABLE_VERSION, PMIX_STRING);
                pmix_list_append(&cd->results, &kv->super);

            } else if (0 == strcmp(queries[n].keys[p], PMIX_QUERY_PROVISIONAL_ABI_VERSION)) {
                PMIX_KVAL_NEW(kv, cb.key);
                PMIx_Value_load(kv->value, PMIX_STD_ABI_PROVISIONAL_VERSION, PMIX_STRING);
                pmix_list_append(&cd->results, &kv->super);

            } else if (0 == strcmp(queries[n].keys[p], PMIX_QUERY_ATTRIBUTE_SUPPORT)) {
                // supported attrs will be appended to cd->results
                // any that must be sent for help will be appended to unresolved
                pmix_attrs_query_support(cd, &queries[n], &unresolved);

            /* check for request to scan the local node for available
             * servers the caller could connect to */
            } else if (0 == strcmp(queries[n].keys[p], PMIX_QUERY_AVAIL_SERVERS)) {
                PMIX_THREADSHIFT(cd, pmix_ptl_base_query_servers);
                return;

            } else {
                PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
                if (PMIX_SUCCESS == rc) {
                    /* need to retain this result */
                    PMIX_LIST_FOREACH_SAFE (kv, kvnxt, &cb.kvs, pmix_kval_t) {
                        pmix_list_remove_item(&cb.kvs, &kv->super);
                        pmix_list_append(&cd->results, &kv->super);
                    }

                } else {
                    // cache this as unresolved
                    qry = PMIX_NEW(pmix_querylist_t);
                    PMIX_ARGV_APPEND(rc, qry->query.keys, queries[n].keys[p]);
                    if (0 < queries[n].nqual) {
                        qry->query.nqual = queries[n].nqual;
                        PMIX_INFO_CREATE(qry->query.qualifiers, qry->query.nqual);
                        for (m=0; m < queries[n].nqual; m++) {
                            PMIX_INFO_XFER(&qry->query.qualifiers[m], &queries[n].qualifiers[m]);
                        }
                    }
                    pmix_list_append(&unresolved, &qry->super);
                }
            }
            cb.key = NULL;  // protect the key
        }
        PMIX_DESTRUCT(&cb);
    }

    if (0 == pmix_list_get_size(&unresolved)) {
        /* if we get here, then all queries were locally
         * resolved, so construct the results for return */
        cd->status = PMIX_SUCCESS;
        cd->ninfo = pmix_list_get_size(&cd->results);
        if (0 < cd->ninfo) {
            PMIX_INFO_CREATE(cd->info, cd->ninfo);
            n = 0;
            PMIX_LIST_FOREACH(kv, &cd->results, pmix_kval_t) {
                PMIX_LOAD_KEY(cd->info[n].key, kv->key);
                rc = PMIx_Value_xfer(&cd->info[n].value, kv->value);
                if (PMIX_SUCCESS != rc) {
                    cd->status = rc;
                    PMIX_INFO_FREE(cd->info, cd->ninfo);
                    break;
                }
                ++n;
            }
        }
        /* done with the list of unresolved queries */
        PMIX_LIST_DESTRUCT(&unresolved);

        if (NULL != cd->cbfunc) {
            cd->cbfunc(cd->status, cd->info, cd->ninfo, cd->cbdata, _local_relcb, cd);
        }
        return;
    }

    // move the unresolved queries into the queries cache
    PMIX_QUERY_FREE(cd->queries, cd->nqueries);
    cd->nqueries = pmix_list_get_size(&unresolved);
    PMIX_QUERY_CREATE(cd->queries, cd->nqueries);
    n = 0;
    PMIX_LIST_FOREACH(qry, &unresolved, pmix_querylist_t) {
        cd->queries[n].keys = PMIx_Argv_copy(qry->query.keys);
        if (0 < qry->query.nqual) {
            cd->queries[n].nqual = qry->query.nqual;
            PMIX_INFO_CREATE(cd->queries[n].qualifiers, cd->queries[n].nqual);
            for (m=0; m < qry->query.nqual; m++) {
                PMIX_INFO_XFER(&cd->queries[n].qualifiers[m], &qry->query.qualifiers[m]);
            }
        }
        ++n;
    }
    /* ask for help */
    rc = request_help(cd);
    if (PMIX_SUCCESS != rc) {
        /* if there are some results, then we return them
         * as "partial success" */
        cd->ninfo = pmix_list_get_size(&cd->results);
        if (0 < cd->ninfo) {
            rc = PMIX_ERR_PARTIAL_SUCCESS;
            PMIX_INFO_CREATE(cd->info, cd->ninfo);
            n = 0;
            PMIX_LIST_FOREACH(kv, &cd->results, pmix_kval_t) {
                PMIX_LOAD_KEY(cd->info[n].key, kv->key);
                rc = PMIx_Value_xfer(&cd->info[n].value, kv->value);
                if (PMIX_SUCCESS != rc) {
                    cd->status = rc;
                    PMIX_INFO_FREE(cd->info, cd->ninfo);
                    break;
                }
                ++n;
            }
            if (NULL != cd->cbfunc) {
                cd->cbfunc(rc, cd->info, cd->ninfo, cd->cbdata, _local_relcb, cd);
            }
        } else {
            /* we have to return the error to the caller */
            if (NULL != cd->cbfunc) {
                cd->cbfunc(rc, NULL, 0, cd->cbdata, _local_relcb, cd);
            }
        }
    }
    return;

    /* get here if the request returned PMIX_SUCCESS, which means
     * that the query is being processed and will call the cbfunc
     * when complete */
}

PMIX_EXPORT pmix_status_t PMIx_Query_info(pmix_query_t queries[], size_t nqueries,
                                          pmix_info_t **results, size_t *nresults)
{
    pmix_query_caddy_t *cd;
    pmix_status_t rc;

    // setup default response
    *results = NULL;
    *nresults = 0;

    /* pass this to the non-blocking version for processing */
    cd = PMIX_NEW(pmix_query_caddy_t);
    rc = PMIx_Query_info_nb(queries, nqueries, qinfocb, (void*)cd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }

    /* wait for the operation to complete */
    PMIX_WAIT_THREAD(&cd->lock);
    rc = cd->status;
    if (NULL != cd->info) {
        *results = cd->info;
        *nresults = cd->ninfo;
        cd->info = NULL;
        cd->ninfo = 0;
    }
    PMIX_RELEASE(cd);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:query completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Query_info_nb(pmix_query_t queries[], size_t nqueries,
                                             pmix_info_cbfunc_t cbfunc, void *cbdata)

{
    pmix_query_caddy_t *cd;
    size_t n, p;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:query non-blocking");

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    if (0 == nqueries || NULL == queries) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* do a quick check of the qualifiers arrays to ensure
     * the nqual field has been set */
    for (n = 0; n < nqueries; n++) {
        if (NULL != queries[n].qualifiers && 0 == queries[n].nqual) {
            /* look for the info marked as "end" */
            p = 0;
            while (!(PMIX_INFO_IS_END(&queries[n].qualifiers[p])) && p < SIZE_MAX) {
                ++p;
            }
            if (SIZE_MAX == p) {
                /* nothing we can do */
                return PMIX_ERR_BAD_PARAM;
            }
            queries[n].nqual = p;
        }
    }

    /* we get here if a refresh isn't required - need to
     * threadshift this to access our internal data */
    cd = PMIX_NEW(pmix_query_caddy_t);
    cd->host_called = false;
    // copy the queries as we will modify them
    cd->nqueries = nqueries;
    PMIX_QUERY_CREATE(cd->queries, cd->nqueries);
    for (n=0; n < nqueries; n++) {
        cd->queries[n].keys = PMIx_Argv_copy(queries[n].keys);
        if (0 < queries[n].nqual) {
            cd->queries[n].nqual = queries[n].nqual;
            PMIX_INFO_CREATE(cd->queries[n].qualifiers, cd->queries[n].nqual);
            for (p=0; p < queries[n].nqual; p++) {
                PMIX_INFO_XFER(&cd->queries[n].qualifiers[p], &queries[n].qualifiers[p]);
            }
        }
    }
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    PMIX_THREADSHIFT(cd, pmix_parse_localquery);
    /* regardless of the result of the query, we return
     * PMIX_SUCCESS here to indicate that the operation
     * was accepted for processing */

    return PMIX_SUCCESS;
}
