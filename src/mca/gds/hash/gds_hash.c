/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <time.h>

#include "include/pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/pmdl/pmdl.h"
#include "src/mca/preg/preg.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/hash.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"

#include "gds_hash.h"
#include "src/mca/gds/base/base.h"

static pmix_status_t hash_init(pmix_info_t info[], size_t ninfo);
static void hash_finalize(void);

static pmix_status_t hash_assign_module(pmix_info_t *info, size_t ninfo, int *priority);

static pmix_status_t hash_cache_job_info(struct pmix_namespace_t *ns, pmix_info_t info[],
                                         size_t ninfo);

static pmix_status_t hash_register_job_info(struct pmix_peer_t *pr, pmix_buffer_t *reply);

static pmix_status_t hash_store_job_info(const char *nspace, pmix_buffer_t *buf);

static pmix_status_t hash_store_modex(struct pmix_namespace_t *ns, pmix_buffer_t *buff,
                                      void *cbdata);

static pmix_status_t _hash_store_modex(pmix_gds_base_ctx_t ctx, pmix_proc_t *proc,
                                       pmix_gds_modex_key_fmt_t key_fmt, char **kmap,
                                       pmix_buffer_t *pbkt);

static pmix_status_t setup_fork(const pmix_proc_t *peer, char ***env);

static pmix_status_t nspace_add(const char *nspace, uint32_t nlocalprocs, pmix_info_t info[],
                                size_t ninfo);

static pmix_status_t nspace_del(const char *nspace);

static pmix_status_t assemb_kvs_req(const pmix_proc_t *proc, pmix_list_t *kvs, pmix_buffer_t *bo,
                                    void *cbdata);

static pmix_status_t accept_kvs_resp(pmix_buffer_t *buf);

pmix_gds_base_module_t pmix_hash_module = {.name = "hash",
                                           .is_tsafe = false,
                                           .init = hash_init,
                                           .finalize = hash_finalize,
                                           .assign_module = hash_assign_module,
                                           .cache_job_info = hash_cache_job_info,
                                           .register_job_info = hash_register_job_info,
                                           .store_job_info = hash_store_job_info,
                                           .store = pmix_gds_hash_store,
                                           .store_modex = hash_store_modex,
                                           .fetch = pmix_gds_hash_fetch,
                                           .setup_fork = setup_fork,
                                           .add_nspace = nspace_add,
                                           .del_nspace = nspace_del,
                                           .assemb_kvs_req = assemb_kvs_req,
                                           .accept_kvs_resp = accept_kvs_resp};

static pmix_status_t hash_init(pmix_info_t info[], size_t ninfo)
{

    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    return PMIX_SUCCESS;
}

static void hash_finalize(void)
{
    return;
}

static pmix_status_t hash_assign_module(pmix_info_t *info, size_t ninfo, int *priority)
{
    size_t n, m;
    char **options;

    *priority = 10;
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_GDS_MODULE, PMIX_MAX_KEYLEN)) {
                options = pmix_argv_split(info[n].value.data.string, ',');
                for (m = 0; NULL != options[m]; m++) {
                    if (0 == strcmp(options[m], "hash")) {
                        /* they specifically asked for us */
                        *priority = 100;
                        break;
                    }
                }
                pmix_argv_free(options);
                break;
            }
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t hash_cache_job_info(struct pmix_namespace_t *ns, pmix_info_t info[],
                                         size_t ninfo)
{
    pmix_namespace_t *nptr = (pmix_namespace_t *) ns;
    pmix_job_t *trk;
    pmix_session_t *s = NULL, *sptr;
    pmix_hash_table_t *ht;
    pmix_kval_t *kp2, *kvptr;
    pmix_info_t *iptr;
    char **nodes = NULL, **procs = NULL;
    uint8_t *tmp;
    uint32_t sid = UINT32_MAX;
    pmix_rank_t rank;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n, j, size, len;
    uint32_t flags = 0;
    pmix_nodeinfo_t *nd;
    pmix_apptrkr_t *apptr;
    bool found;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:cache_job_info for nspace %s with %lu info",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, nptr->nspace, ninfo);

    trk = pmix_gds_hash_get_tracker(nptr->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* if there isn't any data, then be content with just
     * creating the tracker */
    if (NULL == info || 0 == ninfo) {
        return PMIX_SUCCESS;
    }

    /* cache the job info on the internal hash table for this nspace */
    ht = &trk->internal;
    for (n = 0; n < ninfo; n++) {
        pmix_output_verbose(12, pmix_gds_base_framework.framework_output,
                            "%s gds:hash:cache_job_info for key %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), info[n].key);
        if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, sid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
            /* see if we have this session */
            s = NULL;
            PMIX_LIST_FOREACH (sptr, &mca_gds_hash_component.mysessions, pmix_session_t) {
                if (sptr->session == sid) {
                    s = sptr;
                    break;
                }
            }
            if (NULL == s) {
                s = PMIX_NEW(pmix_session_t);
                s->session = sid;
                pmix_list_append(&mca_gds_hash_component.mysessions, &s->super);
            }
            /* point the job at it */
            if (NULL == trk->session) {
                PMIX_RETAIN(s);
                trk->session = s;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_session_array(&info[n].value, trk))) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_INFO_ARRAY)) {
            if (PMIX_SUCCESS
                != (rc = pmix_gds_hash_process_job_array(&info[n], trk, &flags, &procs, &nodes))) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_APP_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_app_array(&info[n].value, trk))) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO_ARRAY)) {
            if (PMIX_SUCCESS
                != (rc = pmix_gds_hash_process_node_array(&info[n].value, &trk->nodeinfo))) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_MAP)) {
            /* not allowed to get this more than once */
            if (flags & PMIX_HASH_NODE_MAP) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            /* parse the regex to get the argv array of node names */
            if (PMIX_REGEX == info[n].value.type) {
                if (PMIX_SUCCESS
                    != (rc = pmix_preg.parse_nodes(info[n].value.data.bo.bytes, &nodes))) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
            } else if (PMIX_STRING == info[n].value.type) {
                if (PMIX_SUCCESS
                    != (rc = pmix_preg.parse_nodes(info[n].value.data.string, &nodes))) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
            } else {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                rc = PMIX_ERR_TYPE_MISMATCH;
                goto release;
            }
            /* mark that we got the map */
            flags |= PMIX_HASH_NODE_MAP;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_MAP)) {
            /* not allowed to get this more than once */
            if (flags & PMIX_HASH_PROC_MAP) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            /* parse the regex to get the argv array containing proc ranks on each node */
            if (PMIX_REGEX == info[n].value.type) {
                if (PMIX_SUCCESS
                    != (rc = pmix_preg.parse_procs(info[n].value.data.bo.bytes, &procs))) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
            } else if (PMIX_STRING == info[n].value.type) {
                if (PMIX_SUCCESS
                    != (rc = pmix_preg.parse_procs(info[n].value.data.string, &procs))) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
            } else {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                rc = PMIX_ERR_TYPE_MISMATCH;
                goto release;
            }
            /* mark that we got the map */
            flags |= PMIX_HASH_PROC_MAP;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_DATA)) {
            flags |= PMIX_HASH_PROC_DATA;
            found = false;
            /* an array of data pertaining to a specific proc */
            if (PMIX_DATA_ARRAY != info[n].value.type) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_TYPE_MISMATCH;
                goto release;
            }
            size = info[n].value.data.darray->size;
            iptr = (pmix_info_t *) info[n].value.data.darray->array;
            /* first element of the array must be the rank */
            if (0 != strcmp(iptr[0].key, PMIX_RANK) || PMIX_PROC_RANK != iptr[0].value.type) {
                rc = PMIX_ERR_TYPE_MISMATCH;
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                goto release;
            }
            rank = iptr[0].value.data.rank;
            /* cycle thru the values for this rank and store them */
            for (j = 1; j < size; j++) {
                kp2 = PMIX_NEW(pmix_kval_t);
                if (NULL == kp2) {
                    rc = PMIX_ERR_NOMEM;
                    goto release;
                }
                kp2->key = strdup(iptr[j].key);
                PMIX_VALUE_XFER(rc, kp2->value, &iptr[j].value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp2);
                    goto release;
                }
                /* if the value contains a string that is longer than the
                 * limit, then compress it */
                if (PMIX_STRING_SIZE_CHECK(kp2->value)) {
                    if (pmix_compress.compress_string(kp2->value->data.string, &tmp, &len)) {
                        if (NULL == tmp) {
                            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                            rc = PMIX_ERR_NOMEM;
                            goto release;
                        }
                        kp2->value->type = PMIX_COMPRESSED_STRING;
                        free(kp2->value->data.string);
                        kp2->value->data.bo.bytes = (char *) tmp;
                        kp2->value->data.bo.size = len;
                    }
                }
                pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                                    "[%s:%d] gds:hash:cache_job_info proc data for [%s:%u]: key %s",
                                    pmix_globals.myid.nspace, pmix_globals.myid.rank, trk->ns, rank,
                                    kp2->key);
                /* store it in the hash_table */
                if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, rank, kp2))) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp2);
                    goto release;
                }
                /* if this is the appnum, pass it to the pmdl framework */
                if (PMIX_CHECK_KEY(kp2, PMIX_APPNUM)) {
                    pmix_pmdl.setup_client(trk->nptr, rank, kp2->value->data.uint32);
                    found = true;
                    if (rank == pmix_globals.myid.rank) {
                        pmix_globals.appnum = kp2->value->data.uint32;
                    }
                }
                PMIX_RELEASE(kp2); // maintain acctg
            }
            if (!found) {
                /* if they didn't give us an appnum for this proc, we have
                 * to assume it is appnum=0 */
                uint32_t zero = 0;
                kp2 = PMIX_NEW(pmix_kval_t);
                if (NULL == kp2) {
                    rc = PMIX_ERR_NOMEM;
                    goto release;
                }
                kp2->key = strdup(PMIX_APPNUM);
                PMIX_VALUE_CREATE(kp2->value, 1);
                PMIX_VALUE_LOAD(kp2->value, &zero, PMIX_UINT32);
                if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, rank, kp2))) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp2);
                    goto release;
                }
                pmix_pmdl.setup_client(trk->nptr, rank, kp2->value->data.uint32);
                PMIX_RELEASE(kp2); // maintain acctg
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_MODEL_LIBRARY_NAME)
                   || PMIX_CHECK_KEY(&info[n], PMIX_PROGRAMMING_MODEL)
                   || PMIX_CHECK_KEY(&info[n], PMIX_MODEL_LIBRARY_VERSION)
                   || PMIX_CHECK_KEY(&info[n], PMIX_PERSONALITY)) {
            // pass this info to the pmdl framework
            pmix_pmdl.setup_nspace(trk->nptr, &info[n]);
        } else if (pmix_check_node_info(info[n].key)) {
            /* they are passing us the node-level info for just this
             * node - start by seeing if our node is on the list */
            nd = pmix_gds_hash_check_nodename(&trk->nodeinfo, pmix_globals.hostname);
            /* if not, then add it */
            if (NULL == nd) {
                nd = PMIX_NEW(pmix_nodeinfo_t);
                nd->hostname = strdup(pmix_globals.hostname);
                pmix_list_append(&trk->nodeinfo, &nd->super);
            }
            /* ensure the value isn't already on the node info */
            PMIX_LIST_FOREACH (kp2, &nd->info, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kp2, info[n].key)) {
                    pmix_list_remove_item(&nd->info, &kp2->super);
                    PMIX_RELEASE(kp2);
                    break;
                }
            }
            /* add the provided value */
            kp2 = PMIX_NEW(pmix_kval_t);
            kp2->key = strdup(info[n].key);
            PMIX_VALUE_XFER(rc, kp2->value, &info[n].value);
            pmix_list_append(&nd->info, &kp2->super);
        } else if (pmix_check_app_info(info[n].key)) {
            /* they are passing us app-level info for a default
             * app number - have to assume it is app=0 */
            if (0 == pmix_list_get_size(&trk->apps)) {
                apptr = PMIX_NEW(pmix_apptrkr_t);
                pmix_list_append(&trk->apps, &apptr->super);
            } else if (1 < pmix_list_get_size(&trk->apps)) {
                rc = PMIX_ERR_BAD_PARAM;
                goto release;
            } else {
                apptr = (pmix_apptrkr_t *) pmix_list_get_first(&trk->apps);
            }
            /* ensure the value isn't already on the app info */
            PMIX_LIST_FOREACH (kp2, &apptr->appinfo, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kp2, info[n].key)) {
                    pmix_list_remove_item(&apptr->appinfo, &kp2->super);
                    PMIX_RELEASE(kp2);
                    break;
                }
            }
            /* add the provided value */
            kp2 = PMIX_NEW(pmix_kval_t);
            kp2->key = strdup(info[n].key);
            PMIX_VALUE_XFER(rc, kp2->value, &info[n].value);
            pmix_list_append(&apptr->appinfo, &kp2->super);
        } else {
            /* just a value relating to the entire job */
            kp2 = PMIX_NEW(pmix_kval_t);
            if (NULL == kp2) {
                rc = PMIX_ERR_NOMEM;
                goto release;
            }
            kp2->key = strdup(info[n].key);
            PMIX_VALUE_XFER(rc, kp2->value, &info[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp2);
                goto release;
            }
            /* if the value contains a string that is longer than the
             * limit, then compress it */
            if (PMIX_STRING_SIZE_CHECK(kp2->value)) {
                if (pmix_compress.compress_string(kp2->value->data.string, &tmp, &len)) {
                    if (NULL == tmp) {
                        rc = PMIX_ERR_NOMEM;
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(kp2);
                        goto release;
                    }
                    kp2->value->type = PMIX_COMPRESSED_STRING;
                    free(kp2->value->data.string);
                    kp2->value->data.bo.bytes = (char *) tmp;
                    kp2->value->data.bo.size = len;
                }
            }
            if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, kp2))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp2);
                goto release;
            }
            PMIX_RELEASE(kp2); // maintain acctg
            /* if this is the job size, then store it in
             * the nptr tracker and flag that we were given it */
            if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_SIZE)) {
                nptr->nprocs = info[n].value.data.uint32;
                flags |= PMIX_HASH_JOB_SIZE;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_NUM_NODES)) {
                flags |= PMIX_HASH_NUM_NODES;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_MAX_PROCS)) {
                flags |= PMIX_HASH_MAX_PROCS;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_DEBUG_STOP_ON_EXEC)
                       || PMIX_CHECK_KEY(&info[n], PMIX_DEBUG_STOP_IN_INIT)
                       || PMIX_CHECK_KEY(&info[n], PMIX_DEBUG_STOP_IN_APP)) {
                if (PMIX_RANK_WILDCARD == info[n].value.data.rank) {
                    nptr->num_waiting = nptr->nlocalprocs;
                } else {
                    nptr->num_waiting = 1;
                }
            } else {
                pmix_iof_check_flags(&info[n], &nptr->iof_flags);
            }
        }
    }

    /* now add any global data that was provided */
    if (!trk->gdata_added) {
        PMIX_LIST_FOREACH (kvptr, &pmix_server_globals.gdata, pmix_kval_t) {
            /* sadly, the data cannot simultaneously exist on two lists,
             * so we must make a copy of it here */
            kp2 = PMIX_NEW(pmix_kval_t);
            if (NULL == kp2) {
                rc = PMIX_ERR_NOMEM;
                goto release;
            }
            kp2->key = strdup(kvptr->key);
            PMIX_VALUE_XFER(rc, kp2->value, kvptr->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp2);
                goto release;
            }
            if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, kp2))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp2);
                break;
            }
            PMIX_RELEASE(kp2); // maintain acctg
        }
        trk->gdata_added = true;
    }

    /* we must have the proc AND node maps */
    if (NULL != procs && NULL != nodes) {
        if (PMIX_SUCCESS != (rc = pmix_gds_hash_store_map(trk, nodes, procs, flags))) {
            PMIX_ERROR_LOG(rc);
        }
    }

release:
    if (NULL != nodes) {
        pmix_argv_free(nodes);
    }
    if (NULL != procs) {
        pmix_argv_free(procs);
    }
    return rc;
}

static pmix_status_t register_info(pmix_peer_t *peer, pmix_namespace_t *ns, pmix_buffer_t *reply)
{
    pmix_job_t *trk;
    pmix_hash_table_t *ht;
    pmix_value_t *val, blob;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_info_t *info;
    size_t ninfo, n;
    pmix_kval_t kv, *kvptr;
    pmix_buffer_t buf;
    pmix_rank_t rank;
    pmix_list_t results;
    char *hname;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "REGISTERING FOR PEER %s type %d.%d.%d",
                        PMIX_PNAME_PRINT(&peer->info->pname), peer->proc_type.major,
                        peer->proc_type.minor, peer->proc_type.release);

    trk = pmix_gds_hash_get_tracker(ns->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    /* the job data is stored on the internal hash table */
    ht = &trk->internal;

    /* fetch all values from the hash table tied to rank=wildcard */
    val = NULL;
    rc = pmix_hash_fetch(ht, PMIX_RANK_WILDCARD, NULL, &val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
        return rc;
    }

    if (NULL == val || NULL == val->data.darray || PMIX_INFO != val->data.darray->type
        || 0 == val->data.darray->size) {
        return PMIX_ERR_NOT_FOUND;
    }
    info = (pmix_info_t *) val->data.darray->array;
    ninfo = val->data.darray->size;
    for (n = 0; n < ninfo; n++) {
        kv.key = info[n].key;
        kv.value = &info[n].value;
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }

    /* add all values in the jobinfo list */
    PMIX_LIST_FOREACH (kvptr, &trk->jobinfo, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
    }

    /* get any node-level info for this job */
    PMIX_CONSTRUCT(&results, pmix_list_t);
    rc = pmix_gds_hash_fetch_nodeinfo(NULL, trk, &trk->nodeinfo, NULL, 0, &results);
    if (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
            /* if the peer is earlier than v3.2.x, it is expecting
             * node info to be in the form of an array, but with the
             * hostname as the key. Detect and convert that here */
            if (PMIX_PEER_IS_EARLIER(peer, 3, 1, 100)) {
                info = (pmix_info_t *) kvptr->value->data.darray->array;
                ninfo = kvptr->value->data.darray->size;
                hname = NULL;
                /* find the hostname */
                for (n = 0; n < ninfo; n++) {
                    if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
                        free(kvptr->key);
                        kvptr->key = strdup(info[n].value.data.string);
                        PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
                        hname = kvptr->key;
                        break;
                    }
                }
                if (NULL != hname && pmix_gds_hash_check_hostname(pmix_globals.hostname, hname)) {
                    /* older versions are looking for node-level keys for
                     * only their own node as standalone keys */
                    for (n = 0; n < ninfo; n++) {
                        if (pmix_check_node_info(info[n].key)) {
                            kv.key = strdup(info[n].key);
                            kv.value = &info[n].value;
                            PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
                        }
                    }
                }
            } else {
                PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
            }
        }
    }
    PMIX_LIST_DESTRUCT(&results);

    /* get any app-level info for this job */
    PMIX_CONSTRUCT(&results, pmix_list_t);
    rc = pmix_gds_hash_fetch_appinfo(NULL, trk, &trk->apps, NULL, 0, &results);
    if (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
            PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
        }
    }
    PMIX_LIST_DESTRUCT(&results);

    /* get the proc-level data for each proc in the job */
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "FETCHING PROC INFO FOR NSPACE %s NPROCS %u", ns->nspace, ns->nprocs);
    for (rank = 0; rank < ns->nprocs; rank++) {
        pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                            "FETCHING PROC INFO FOR RANK %s", PMIX_RANK_PRINT(rank));
        val = NULL;
        rc = pmix_hash_fetch(ht, rank, NULL, &val);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            PMIX_ERROR_LOG(rc);
            if (NULL != val) {
                PMIX_VALUE_RELEASE(val);
            }
            return rc;
        }
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, peer, &buf, &rank, 1, PMIX_PROC_RANK);

        if (NULL != val) {
            info = (pmix_info_t *) val->data.darray->array;
            ninfo = val->data.darray->size;
            for (n = 0; n < ninfo; n++) {
                kv.key = info[n].key;
                kv.value = &info[n].value;
                PMIX_BFROPS_PACK(rc, peer, &buf, &kv, 1, PMIX_KVAL);
            }
        }
        kv.key = PMIX_PROC_BLOB;
        kv.value = &blob;
        blob.type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(&buf, blob.data.bo.bytes, blob.data.bo.size);
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
        PMIX_VALUE_DESTRUCT(&blob);
        PMIX_DESTRUCT(&buf);

        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
    }
    return rc;
}

/* the purpose of this function is to pack the job-level
 * info stored in the pmix_namespace_t into a buffer and send
 * it to the given client */
static pmix_status_t hash_register_job_info(struct pmix_peer_t *pr, pmix_buffer_t *reply)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_namespace_t *ns = peer->nptr;
    char *msg;
    pmix_status_t rc;
    pmix_job_t *trk;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) && !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        /* this function is only available on servers */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:register_job_info for peer [%s:%d]",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, peer->info->pname.nspace,
                        peer->info->pname.rank);

    /* first see if we already have processed this data
     * for another peer in this nspace so we don't waste
     * time doing it again */
    if (NULL != ns->jobbkt) {
        pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                            "[%s:%d] gds:hash:register_job_info copying prepacked payload",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank);
        /* we have packed this before - can just deliver it */
        PMIX_BFROPS_COPY_PAYLOAD(rc, peer, reply, ns->jobbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* now see if we have delivered it to all our local
         * clients for this nspace */
        if (!PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) && ns->ndelivered == ns->nlocalprocs) {
            /* we have, so let's get rid of the packed
             * copy of the data */
            PMIX_RELEASE(ns->jobbkt);
            ns->jobbkt = NULL;
        }
        return rc;
    }

    /* setup a tracker for this nspace as we will likely
     * need it again */
    trk = pmix_gds_hash_get_tracker(ns->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* the job info for the specified nspace has
     * been given to us in the info array - pack
     * them for delivery */
    /* pack the name of the nspace */
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:register_job_info packing new payload",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);
    msg = ns->nspace;
    PMIX_BFROPS_PACK(rc, peer, reply, &msg, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = register_info(peer, ns, reply);
    if (PMIX_SUCCESS == rc) {
        /* if we have more than one local client for this nspace,
         * save this packed object so we don't do this again */
        if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) || 1 < ns->nlocalprocs) {
            PMIX_RETAIN(reply);
            ns->jobbkt = reply;
        }
    } else {
        PMIX_ERROR_LOG(rc);
    }

    return rc;
}

static pmix_status_t hash_store_job_info(const char *nspace, pmix_buffer_t *buf)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_kval_t *kptr, *kp2, *kp3, kv;
    int32_t cnt;
    size_t nnodes, len;
    uint32_t i, j;
    char **procs = NULL;
    uint8_t *tmp;
    pmix_byte_object_t *bo;
    pmix_buffer_t buf2;
    int rank;
    pmix_job_t *trk;
    pmix_hash_table_t *ht;
    char **nodelist = NULL;
    pmix_nodeinfo_t *nd;
    pmix_namespace_t *ns, *nptr;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%u] pmix:gds:hash store job info for nspace %s",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, nspace);

    /* check buf data */
    if ((NULL == buf) || (0 == buf->bytes_used)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    ht = &trk->internal;

    /* retrieve the nspace pointer */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* only can happen if we are out of mem */
        return PMIX_ERR_NOMEM;
    }

    cnt = 1;
    kptr = PMIX_NEW(pmix_kval_t);
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, kptr, &cnt, PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                            "[%s:%u] pmix:gds:hash store job info working key %s",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank, kptr->key);
        if (PMIX_CHECK_KEY(kptr, PMIX_PROC_BLOB)) {
            bo = &(kptr->value->data.bo);
            PMIX_CONSTRUCT(&buf2, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &buf2, bo->bytes, bo->size);
            /* start by unpacking the rank */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &rank, &cnt,
                               PMIX_PROC_RANK);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kptr);
                PMIX_DESTRUCT(&buf2);
                return rc;
            }
            /* unpack the blob and save the values for this rank */
            cnt = 1;
            kp2 = PMIX_NEW(pmix_kval_t);
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, kp2, &cnt, PMIX_KVAL);
            while (PMIX_SUCCESS == rc) {
                /* if the value contains a string that is longer than the
                 * limit, then compress it */
                if (PMIX_STRING_SIZE_CHECK(kp2->value)) {
                    if (pmix_compress.compress_string(kp2->value->data.string, &tmp, &len)) {
                        if (NULL == tmp) {
                            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                            rc = PMIX_ERR_NOMEM;
                            return rc;
                        }
                        kp2->value->type = PMIX_COMPRESSED_STRING;
                        free(kp2->value->data.string);
                        kp2->value->data.bo.bytes = (char *) tmp;
                        kp2->value->data.bo.size = len;
                    }
                }
                /* this is data provided by a job-level exchange, so store it
                 * in the job-level data hash_table */
                pmix_output_verbose(
                    2, pmix_gds_base_framework.framework_output,
                    "[%s:%u] pmix:gds:hash store proc info for rank %u working key %s",
                    pmix_globals.myid.nspace, pmix_globals.myid.rank, rank, kp2->key);
                if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, rank, kp2))) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp2);
                    PMIX_RELEASE(kptr);
                    PMIX_DESTRUCT(&buf2);
                    return rc;
                }
                PMIX_RELEASE(kp2); // maintain accounting
                cnt = 1;
                kp2 = PMIX_NEW(pmix_kval_t);
                PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, kp2, &cnt, PMIX_KVAL);
            }
            /* cleanup */
            PMIX_DESTRUCT(&buf2); // releases the original kptr data
            PMIX_RELEASE(kp2);
        } else if (PMIX_CHECK_KEY(kptr, PMIX_MAP_BLOB)) {
            /* transfer the byte object for unpacking */
            bo = &(kptr->value->data.bo);
            PMIX_CONSTRUCT(&buf2, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &buf2, bo->bytes, bo->size);
            /* start by unpacking the number of nodes */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &nnodes, &cnt, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kptr);
                PMIX_DESTRUCT(&buf2);
                return rc;
            }
            for (i = 0; i < nnodes; i++) {
                /* unpack the list of procs on each node */
                cnt = 1;
                PMIX_CONSTRUCT(&kv, pmix_kval_t);
                PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &kv, &cnt, PMIX_KVAL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kptr);
                    PMIX_DESTRUCT(&buf2);
                    PMIX_DESTRUCT(&kv);
                    return rc;
                }
                /* track the nodes in this nspace */
                pmix_argv_append_nosize(&nodelist, kv.key);
                /* check and see if we already have this node */
                nd = pmix_gds_hash_check_nodename(&trk->nodeinfo, kv.key);
                if (NULL == nd) {
                    nd = PMIX_NEW(pmix_nodeinfo_t);
                    nd->hostname = strdup(kv.key);
                    pmix_list_append(&trk->nodeinfo, &nd->super);
                }
                /* save the list of peers for this node */
                kp2 = PMIX_NEW(pmix_kval_t);
                if (NULL == kp2) {
                    PMIX_RELEASE(kptr);
                    return PMIX_ERR_NOMEM;
                }
                kp2->key = strdup(PMIX_LOCAL_PEERS);
                kp2->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                if (NULL == kp2->value) {
                    PMIX_RELEASE(kp2);
                    PMIX_RELEASE(kptr);
                    return PMIX_ERR_NOMEM;
                }
                kp2->value->type = PMIX_STRING;
                kp2->value->data.string = strdup(kv.value->data.string);
                /* ensure this item only appears once on the list */
                PMIX_LIST_FOREACH (kp3, &nd->info, pmix_kval_t) {
                    if (PMIX_CHECK_KEY(kp3, kp2->key)) {
                        pmix_list_remove_item(&nd->info, &kp3->super);
                        PMIX_RELEASE(kp3);
                        break;
                    }
                }
                pmix_list_append(&nd->info, &kp2->super);
                /* split the list of procs so we can store their
                 * individual location data */
                procs = pmix_argv_split(kv.value->data.string, ',');
                for (j = 0; NULL != procs[j]; j++) {
                    /* store the hostname for each proc - again, this is
                     * data obtained via a job-level exchange, so store it
                     * in the job-level data hash_table */
                    kp2 = PMIX_NEW(pmix_kval_t);
                    kp2->key = strdup(PMIX_HOSTNAME);
                    kp2->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                    kp2->value->type = PMIX_STRING;
                    kp2->value->data.string = strdup(kv.key);
                    rank = strtol(procs[j], NULL, 10);
                    pmix_output_verbose(
                        2, pmix_gds_base_framework.framework_output,
                        "[%s:%u] pmix:gds:hash store map info for rank %u working key %s",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, rank, kp2->key);
                    if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, rank, kp2))) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(kp2);
                        PMIX_RELEASE(kptr);
                        PMIX_DESTRUCT(&kv);
                        PMIX_DESTRUCT(&buf2);
                        pmix_argv_free(procs);
                        return rc;
                    }
                    PMIX_RELEASE(kp2); // maintain acctg
                }
                pmix_argv_free(procs);
                PMIX_DESTRUCT(&kv);
            }
            if (NULL != nodelist) {
                /* store the comma-delimited list of nodes hosting
                 * procs in this nspace */
                kp2 = PMIX_NEW(pmix_kval_t);
                kp2->key = strdup(PMIX_NODE_LIST);
                kp2->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                kp2->value->type = PMIX_STRING;
                kp2->value->data.string = pmix_argv_join(nodelist, ',');
                pmix_argv_free(nodelist);
                if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, kp2))) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp2);
                    PMIX_RELEASE(kptr);
                    PMIX_DESTRUCT(&kv);
                    PMIX_DESTRUCT(&buf2);
                    return rc;
                }
                PMIX_RELEASE(kp2); // maintain acctg
            }
            /* cleanup */
            PMIX_DESTRUCT(&buf2);
        } else if (PMIX_CHECK_KEY(kptr, PMIX_APP_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_app_array(kptr->value, trk))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kptr);
                return rc;
            }
        } else if (PMIX_CHECK_KEY(kptr, PMIX_NODE_INFO_ARRAY)) {
            if (PMIX_SUCCESS
                != (rc = pmix_gds_hash_process_node_array(kptr->value, &trk->nodeinfo))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kptr);
                return rc;
            }
        } else {
            /* if the value contains a string that is longer than the
             * limit, then compress it */
            if (PMIX_STRING_SIZE_CHECK(kptr->value)) {
                if (pmix_compress.compress_string(kptr->value->data.string, &tmp, &len)) {
                    if (NULL == tmp) {
                        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                        rc = PMIX_ERR_NOMEM;
                        PMIX_RELEASE(kptr);
                        return rc;
                    }
                    kptr->value->type = PMIX_COMPRESSED_STRING;
                    free(kptr->value->data.string);
                    kptr->value->data.bo.bytes = (char *) tmp;
                    kptr->value->data.bo.size = len;
                }
            }
            pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                                "[%s:%u] pmix:gds:hash store job info storing key %s for WILDCARD rank",
                                pmix_globals.myid.nspace, pmix_globals.myid.rank, kptr->key);
            if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, kptr))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kptr);
                return rc;
            }
            /* if this is the job size, then store it in
             * the nptr tracker */
            if (0 == nptr->nprocs && PMIX_CHECK_KEY(kptr, PMIX_JOB_SIZE)) {
                nptr->nprocs = kptr->value->data.uint32;
            }
        }
        PMIX_RELEASE(kptr);
        kptr = PMIX_NEW(pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, kptr, &cnt, PMIX_KVAL);
    }
    /* need to release the leftover kptr */
    PMIX_RELEASE(kptr);

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

pmix_status_t pmix_gds_hash_store(const pmix_proc_t *proc, pmix_scope_t scope, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;
    pmix_kval_t *kp;
    pmix_rank_t rank;
    size_t j, size, len;
    pmix_info_t *iptr;
    uint8_t *tmp;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "%s gds:hash:hash_store for proc %s key %s type %s scope %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIX_NAME_PRINT(proc), kv->key,
                        PMIx_Data_type_string(kv->value->type), PMIx_Scope_string(scope));

    if (NULL == kv->key) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(proc->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* if this is node/app data, then process it accordingly */
    if (PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY)) {
        rc = pmix_gds_hash_process_node_array(kv->value, &trk->nodeinfo);
        return rc;
    } else if (PMIX_CHECK_KEY(kv, PMIX_APP_INFO_ARRAY)) {
        rc = pmix_gds_hash_process_app_array(kv->value, trk);
        return rc;
    } else if (PMIX_CHECK_KEY(kv, PMIX_SESSION_INFO_ARRAY)) {
        rc = pmix_gds_hash_process_session_array(kv->value, trk);
        return rc;
    } else if (PMIX_CHECK_KEY(kv, PMIX_JOB_INFO_ARRAY)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* see if the proc is me - cannot use CHECK_PROCID as
     * we don't want rank=wildcard to match */
    if (proc->rank == pmix_globals.myid.rank
        && PMIX_CHECK_NSPACE(proc->nspace, pmix_globals.myid.nspace)) {
        if (PMIX_INTERNAL != scope) {
            /* always maintain a copy of my own info here to simplify
             * later retrieval */
            kp = PMIX_NEW(pmix_kval_t);
            if (NULL == kp) {
                return PMIX_ERR_NOMEM;
            }
            kp->key = strdup(kv->key);
            kp->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            if (NULL == kp->value) {
                PMIX_RELEASE(kp);
                return PMIX_ERR_NOMEM;
            }
            PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kp->value, kv->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kp);
                return rc;
            }
            if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->internal, proc->rank, kp))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp);
                return rc;
            }
            PMIX_RELEASE(kp); // maintain accounting
        }
    }

    /* if the number of procs for the nspace object is new, then update it */
    if (0 == trk->nptr->nprocs && PMIX_CHECK_KEY(kv, PMIX_JOB_SIZE)) {
        trk->nptr->nprocs = kv->value->data.uint32;
    }

    /* store it in the corresponding hash table */
    if (PMIX_INTERNAL == scope) {
        /* if this is proc data, then we have to expand it and
         * store the values on that rank */
        if (PMIX_CHECK_KEY(kv, PMIX_PROC_DATA)) {
            /* an array of data pertaining to a specific proc */
            if (PMIX_DATA_ARRAY != kv->value->type) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                return PMIX_ERR_TYPE_MISMATCH;
            }
            size = kv->value->data.darray->size;
            iptr = (pmix_info_t *) kv->value->data.darray->array;
            /* first element of the array must be the rank */
            if (0 != strcmp(iptr[0].key, PMIX_RANK) || PMIX_PROC_RANK != iptr[0].value.type) {
                rc = PMIX_ERR_TYPE_MISMATCH;
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            rank = iptr[0].value.data.rank;
            /* cycle thru the values for this rank and store them */
            for (j = 1; j < size; j++) {
                kp = PMIX_NEW(pmix_kval_t);
                if (NULL == kp) {
                    rc = PMIX_ERR_NOMEM;
                    return rc;
                }
                kp->key = strdup(iptr[j].key);
                PMIX_VALUE_XFER(rc, kp->value, &iptr[j].value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp);
                    return rc;
                }
                /* if the value contains a string that is longer than the
                 * limit, then compress it */
                if (PMIX_STRING_SIZE_CHECK(kp->value)) {
                    if (pmix_compress.compress_string(kp->value->data.string, &tmp, &len)) {
                        if (NULL == tmp) {
                            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                            rc = PMIX_ERR_NOMEM;
                            return rc;
                        }
                        kp->value->type = PMIX_COMPRESSED_STRING;
                        free(kp->value->data.string);
                        kp->value->data.bo.bytes = (char *) tmp;
                        kp->value->data.bo.size = len;
                    }
                }
                pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                                    "%s gds:hash:STORE data for nspace %s rank %u: key %s",
                                    PMIX_NAME_PRINT(&pmix_globals.myid), trk->ns, rank, kp->key);
                /* store it in the hash_table */
                if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->internal, rank, kp))) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp);
                    return rc;
                }
                PMIX_RELEASE(kp); // maintain acctg
            }
            return PMIX_SUCCESS;
        }
        if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->internal, proc->rank, kv))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else if (PMIX_REMOTE == scope) {
        if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->remote, proc->rank, kv))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else if (PMIX_LOCAL == scope) {
        if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->local, proc->rank, kv))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else if (PMIX_GLOBAL == scope) {
        if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->remote, proc->rank, kv))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* a pmix_kval_t can only be on one list at a time, so we
         * have to duplicate it here */
        kp = PMIX_NEW(pmix_kval_t);
        if (NULL == kp) {
            return PMIX_ERR_NOMEM;
        }
        kp->key = strdup(kv->key);
        kp->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        if (NULL == kp->value) {
            PMIX_RELEASE(kp);
            return PMIX_ERR_NOMEM;
        }
        PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kp->value, kv->value);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kp);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->local, proc->rank, kp))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kp);
            return rc;
        }
        PMIX_RELEASE(kp); // maintain accounting
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    return PMIX_SUCCESS;
}

/* this function is only called by the PMIx server when its
 * host has received data from some other peer. It therefore
 * always contains data solely from remote procs, and we
 * shall store it accordingly */
static pmix_status_t hash_store_modex(struct pmix_namespace_t *nspace, pmix_buffer_t *buf,
                                      void *cbdata)
{
    return pmix_gds_base_store_modex(nspace, buf, NULL, _hash_store_modex, cbdata);
}

static pmix_status_t _hash_store_modex(pmix_gds_base_ctx_t ctx, pmix_proc_t *proc,
                                       pmix_gds_modex_key_fmt_t key_fmt, char **kmap,
                                       pmix_buffer_t *pbkt)
{
    pmix_job_t *trk;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_kval_t *kv;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:store_modex for nspace %s", pmix_globals.myid.nspace,
                        pmix_globals.myid.rank, proc->nspace);

    PMIX_HIDE_UNUSED_PARAMS(ctx);

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(proc->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* this is data returned via the PMIx_Fence call when
     * data collection was requested, so it only contains
     * REMOTE/GLOBAL data. The byte object contains
     * the rank followed by pmix_kval_t's. The list of callbacks
     * contains all local participants. */

    /* unpack the remaining values until we hit the end of the buffer */
    kv = PMIX_NEW(pmix_kval_t);
    rc = pmix_gds_base_modex_unpack_kval(key_fmt, pbkt, kmap, kv);

    while (PMIX_SUCCESS == rc) {
        if (PMIX_RANK_UNDEF == proc->rank) {
            /* if the rank is undefined, then we store it on the
             * remote table of rank=0 as we know that rank must
             * always exist */
            if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->remote, 0, kv))) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        } else {
            /* store this in the hash table */
            if (PMIX_SUCCESS != (rc = pmix_hash_store(&trk->remote, proc->rank, kv))) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        PMIX_RELEASE(kv); // maintain accounting as the hash increments the ref count
        /* continue along */
        kv = PMIX_NEW(pmix_kval_t);
        rc = pmix_gds_base_modex_unpack_kval(key_fmt, pbkt, kmap, kv);
    }
    PMIX_RELEASE(kv); // maintain accounting
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{

    PMIX_HIDE_UNUSED_PARAMS(proc, env);

    /* we don't need to add anything */
    return PMIX_SUCCESS;
}

static pmix_status_t nspace_add(const char *nspace, uint32_t nlocalprocs, pmix_info_t info[],
                                size_t ninfo)
{

    PMIX_HIDE_UNUSED_PARAMS(nspace, nlocalprocs, info, ninfo);

    /* we don't need to do anything here */
    return PMIX_SUCCESS;
}

static pmix_status_t nspace_del(const char *nspace)
{
    pmix_job_t *t;


    /* find the hash table for this nspace */
    PMIX_LIST_FOREACH (t, &mca_gds_hash_component.myjobs, pmix_job_t) {
        if (0 == strcmp(nspace, t->ns)) {
            /* release it */
            pmix_list_remove_item(&mca_gds_hash_component.myjobs, &t->super);
            PMIX_RELEASE(t);
            break;
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t assemb_kvs_req(const pmix_proc_t *proc, pmix_list_t *kvs, pmix_buffer_t *buf,
                                    void *cbdata)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;
    pmix_kval_t *kv;

    if (!PMIX_PEER_IS_V1(cd->peer)) {
        PMIX_BFROPS_PACK(rc, cd->peer, buf, proc, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    PMIX_LIST_FOREACH (kv, kvs, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, cd->peer, buf, kv, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return rc;
}

static pmix_status_t store_session_info(pmix_nspace_t nspace, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_gds_hash_process_session_array(kv->value, trk);
    return rc;
}

static pmix_status_t store_node_info(pmix_nspace_t nspace, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_gds_hash_process_node_array(kv->value, &trk->nodeinfo);
    return rc;
}

static pmix_status_t store_app_info(pmix_nspace_t nspace, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_gds_hash_process_app_array(kv->value, trk);
    return rc;
}

static pmix_status_t accept_kvs_resp(pmix_buffer_t *buf)
{
    pmix_status_t rc = PMIX_SUCCESS;
    int32_t cnt;
    pmix_byte_object_t bo;
    pmix_buffer_t pbkt;
    pmix_kval_t *kv;
    pmix_proc_t proct;

    /* the incoming payload is provided as a set of packed
     * byte objects, one for each rank. A pmix_proc_t is the first
     * entry in the byte object. If the rank=PMIX_RANK_WILDCARD,
     * then that byte object contains job level info
     * for the provided nspace. Otherwise, the byte
     * object contains the pmix_kval_t's that were "put" by the
     * referenced process */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    while (PMIX_SUCCESS == rc) {
        /* setup the byte object for unpacking */
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &pbkt, bo.bytes, bo.size);
        /* unpack the id of the providing process */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &pbkt, &proct, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* if the rank is UNDEF, then we store this on our own
         * rank tables */
        if (PMIX_RANK_UNDEF == proct.rank) {
            proct.rank = pmix_globals.myid.rank;
        }

        cnt = 1;
        kv = PMIX_NEW(pmix_kval_t);
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &pbkt, kv, &cnt, PMIX_KVAL);
        while (PMIX_SUCCESS == rc) {
            /* if this is an info array, then store it here as dstore
             * doesn't know how to handle it */
            if (PMIX_CHECK_KEY(kv, PMIX_SESSION_INFO_ARRAY)) {
                rc = store_session_info(proct.nspace, kv);
            } else if (PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY)) {
                rc = store_node_info(proct.nspace, kv);
            } else if (PMIX_CHECK_KEY(kv, PMIX_APP_INFO_ARRAY)) {
                rc = store_app_info(proct.nspace, kv);
            } else {
                rc = pmix_gds_hash_store(&proct, PMIX_INTERNAL, kv);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                PMIX_DESTRUCT(&pbkt);
                return rc;
            }
            PMIX_RELEASE(kv); // maintain accounting
            /* get the next one */
            kv = PMIX_NEW(pmix_kval_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &pbkt, kv, &cnt, PMIX_KVAL);
        }
        PMIX_RELEASE(kv); // maintain accounting
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&pbkt);
            return rc;
        }
        PMIX_DESTRUCT(&pbkt);
        /* get the next one */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    return rc;
}
