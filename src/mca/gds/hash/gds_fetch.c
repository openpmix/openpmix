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

static pmix_status_t dohash(pmix_hash_table_t *ht, const char *key, pmix_rank_t rank,
                            int skip_genvals, pmix_list_t *kvs)
{
    pmix_status_t rc;
    pmix_value_t *val;
    pmix_kval_t *kv, *k2;
    pmix_info_t *info;
    size_t n, ninfo;
    bool found;

    rc = pmix_hash_fetch(ht, rank, key, &val);
    if (PMIX_SUCCESS == rc) {
        /* if the key was NULL, then all found keys will be
         * returned as a pmix_data_array_t in the value */
        if (NULL == key) {
            if (NULL == val->data.darray || PMIX_INFO != val->data.darray->type
                || 0 == val->data.darray->size) {
                PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
                PMIX_VALUE_RELEASE(val);
                return PMIX_ERR_NOT_FOUND;
            }
            /* if they want the value returned in its array form,
             * then we are done */
            if (2 == skip_genvals) {
                kv = PMIX_NEW(pmix_kval_t);
                if (NULL == kv) {
                    PMIX_VALUE_RELEASE(val);
                    return PMIX_ERR_NOMEM;
                }
                kv->value = val;
                pmix_list_append(kvs, &kv->super);
                return PMIX_SUCCESS;
            }
            info = (pmix_info_t *) val->data.darray->array;
            ninfo = val->data.darray->size;
            for (n = 0; n < ninfo; n++) {
                /* if the rank is UNDEF, then we don't want
                 * anything that starts with "pmix" */
                if (1 == skip_genvals && 0 == strncmp(info[n].key, "pmix", 4)) {
                    continue;
                }
                /* see if we already have this on the list */
                found = false;
                PMIX_LIST_FOREACH (k2, kvs, pmix_kval_t) {
                    if (PMIX_CHECK_KEY(&info[n], k2->key)) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    continue;
                }
                kv = PMIX_NEW(pmix_kval_t);
                if (NULL == kv) {
                    PMIX_VALUE_RELEASE(val);
                    return PMIX_ERR_NOMEM;
                }
                kv->key = strdup(info[n].key);
                kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                if (NULL == kv->value) {
                    PMIX_VALUE_RELEASE(val);
                    PMIX_RELEASE(kv);
                    return PMIX_ERR_NOMEM;
                }
                PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kv->value, &info[n].value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_VALUE_RELEASE(val);
                    PMIX_RELEASE(kv);
                    return rc;
                }
                pmix_list_append(kvs, &kv->super);
            }
            PMIX_VALUE_RELEASE(val);
        } else {
            kv = PMIX_NEW(pmix_kval_t);
            if (NULL == kv) {
                PMIX_VALUE_RELEASE(val);
                return PMIX_ERR_NOMEM;
            }
            kv->key = strdup(key);
            kv->value = val;
            pmix_list_append(kvs, &kv->super);
        }
    }
    return rc;
}

pmix_status_t pmix_gds_hash_fetch_nodeinfo(const char *key, pmix_job_t *trk, pmix_list_t *tgt,
                                           pmix_info_t *info, size_t ninfo, pmix_list_t *kvs)
{
    size_t n, nds;
    pmix_status_t rc;
    uint32_t nid = UINT32_MAX;
    char *hostname = NULL;
    bool found = false;
    pmix_nodeinfo_t *nd, *ndptr;
    pmix_kval_t *kv, *kp2;
    pmix_data_array_t *darray;
    pmix_info_t *iptr;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output, "FETCHING NODE INFO");

    /* scan for the nodeID or hostname to identify
     * which node they are asking about */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, nid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            found = true;
            break;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
            hostname = info[n].value.data.string;
            found = true;
            break;
        }
    }
    if (!found) {
        /* if the key is NULL, then they want all the info from
         * all nodes */
        if (NULL == key) {
            PMIX_LIST_FOREACH (nd, tgt, pmix_nodeinfo_t) {
                kv = PMIX_NEW(pmix_kval_t);
                /* if the proc's version is earlier than v3.1, then the
                 * info must be provided as a data_array with a key
                 * of the node's name as earlier versions don't understand
                 * node_info arrays */
                if (trk->nptr->version.major < 3
                    || (3 == trk->nptr->version.major && 0 == trk->nptr->version.minor)) {
                    if (NULL == nd->hostname) {
                        /* skip this one */
                        continue;
                    }
                    kv->key = strdup(nd->hostname);
                } else {
                    /* everyone else uses a node_info array */
                    kv->key = strdup(PMIX_NODE_INFO_ARRAY);
                }
                kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                if (NULL == kv->value) {
                    PMIX_RELEASE(kv);
                    return PMIX_ERR_NOMEM;
                }
                nds = pmix_list_get_size(&nd->info);
                if (NULL != nd->hostname) {
                    ++nds;
                }
                if (UINT32_MAX != nd->nodeid) {
                    ++nds;
                }
                PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
                if (NULL == darray) {
                    PMIX_RELEASE(kv);
                    return PMIX_ERR_NOMEM;
                }
                iptr = (pmix_info_t *) darray->array;
                n = 0;
                if (NULL != nd->hostname) {
                    PMIX_INFO_LOAD(&iptr[n], PMIX_HOSTNAME, nd->hostname, PMIX_STRING);
                    ++n;
                }
                if (UINT32_MAX != nd->nodeid) {
                    PMIX_INFO_LOAD(&iptr[n], PMIX_NODEID, &nd->nodeid, PMIX_UINT32);
                    ++n;
                }
                PMIX_LIST_FOREACH (kp2, &nd->info, pmix_kval_t) {
                    pmix_output_verbose(12, pmix_gds_base_framework.framework_output,
                                        "%s gds:hash:fetch_nodearray adding key %s",
                                        PMIX_NAME_PRINT(&pmix_globals.myid), kp2->key);
                    PMIX_LOAD_KEY(iptr[n].key, kp2->key);
                    rc = pmix_value_xfer(&iptr[n].value, kp2->value);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_ARRAY_FREE(darray);
                        PMIX_RELEASE(kv);
                        return rc;
                    }
                    ++n;
                }
                kv->value->data.darray = darray;
                kv->value->type = PMIX_DATA_ARRAY;
                pmix_list_append(kvs, &kv->super);
            }
            return PMIX_SUCCESS;
        }
        /* assume they want it from this node */
        hostname = pmix_globals.hostname;
    }

    /* scan the list of nodes to find the matching entry */
    nd = NULL;
    if (UINT32_MAX!= nid) {
        PMIX_LIST_FOREACH (ndptr, tgt, pmix_nodeinfo_t) {
            if (UINT32_MAX != ndptr->nodeid &&
                nid == ndptr->nodeid) {
                nd = ndptr;
                break;
            }
        }
    } else if (NULL != hostname) {
        nd = pmix_gds_hash_check_nodename(tgt, hostname);
    }
    if (NULL == nd) {
        if (!found) {
            /* they didn't specify, so it is optional */
            return PMIX_ERR_DATA_VALUE_NOT_FOUND;
        }
        return PMIX_ERR_NOT_FOUND;
    }

    /* if they want it all, give it to them */
    if (NULL == key) {
        kv = PMIX_NEW(pmix_kval_t);
        /* if the proc's version is earlier than v3.1, then the
         * info must be provided as a data_array with a key
         * of the node's name as earlier versions don't understand
         * node_info arrays */
        if (trk->nptr->version.major < 3
            || (3 == trk->nptr->version.major && 0 == trk->nptr->version.minor)) {
            if (NULL == nd->hostname) {
                kv->key = strdup(pmix_globals.hostname);
            } else {
                kv->key = strdup(nd->hostname);
            }
        } else {
            /* everyone else uses a node_info array */
            kv->key = strdup(PMIX_NODE_INFO_ARRAY);
        }
        kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        nds = pmix_list_get_size(&nd->info);
        if (NULL != nd->hostname) {
            ++nds;
        }
        if (UINT32_MAX != nd->nodeid) {
            ++nds;
        }
        PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
        if (NULL == darray) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        iptr = (pmix_info_t *) darray->array;
        n = 0;
        if (NULL != nd->hostname) {
            PMIX_INFO_LOAD(&iptr[n], PMIX_HOSTNAME, nd->hostname, PMIX_STRING);
            ++n;
        }
        if (UINT32_MAX != nd->nodeid) {
            PMIX_INFO_LOAD(&iptr[n], PMIX_NODEID, &nd->nodeid, PMIX_UINT32);
            ++n;
        }
        PMIX_LIST_FOREACH (kp2, &nd->info, pmix_kval_t) {
            pmix_output_verbose(12, pmix_gds_base_framework.framework_output,
                                "%s gds:hash:fetch_nodearray adding key %s",
                                PMIX_NAME_PRINT(&pmix_globals.myid), kp2->key);
            PMIX_LOAD_KEY(iptr[n].key, kp2->key);
            rc = pmix_value_xfer(&iptr[n].value, kp2->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_ARRAY_FREE(darray);
                PMIX_RELEASE(kv);
                return rc;
            }
            ++n;
        }
        kv->value->data.darray = darray;
        kv->value->type = PMIX_DATA_ARRAY;
        pmix_list_append(kvs, &kv->super);
        return PMIX_SUCCESS;
    }

    /* scan the info list of this node to find the key they want */
    rc = PMIX_ERR_NOT_FOUND;
    PMIX_LIST_FOREACH (kp2, &nd->info, pmix_kval_t) {
        if (PMIX_CHECK_KEY(kp2, key)) {
            pmix_output_verbose(12, pmix_gds_base_framework.framework_output,
                                "%s gds:hash:fetch_nodearray adding key %s",
                                PMIX_NAME_PRINT(&pmix_globals.myid), kp2->key);
            /* since they only asked for one key, return just that value */
            kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(kp2->key);
            kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            if (NULL == kv->value) {
                PMIX_RELEASE(kv);
                return PMIX_ERR_NOMEM;
            }
            rc = pmix_value_xfer(kv->value, kp2->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                return rc;
            }
            pmix_list_append(kvs, &kv->super);
            break;
        }
    }
    return rc;
}

pmix_status_t pmix_gds_hash_fetch_appinfo(const char *key, pmix_job_t *trk, pmix_list_t *tgt,
                                          pmix_info_t *info, size_t ninfo, pmix_list_t *kvs)
{
    size_t n, nds;
    pmix_status_t rc;
    uint32_t appnum;
    bool found = false;
    pmix_apptrkr_t *app, *apptr;
    pmix_kval_t *kv, *kp2;
    pmix_data_array_t *darray;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "FETCHING APP INFO WITH %d APPS", (int) pmix_list_get_size(tgt));

    /* scan for the appnum to identify
     * which app they are asking about */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_APPNUM)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, appnum, uint32_t);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        /* if the key is NULL, then they want all the info from
         * all apps */
        if (NULL == key) {
            PMIX_LIST_FOREACH (apptr, tgt, pmix_apptrkr_t) {
                kv = PMIX_NEW(pmix_kval_t);
                kv->key = strdup(PMIX_APP_INFO_ARRAY);
                kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                if (NULL == kv->value) {
                    PMIX_RELEASE(kv);
                    return PMIX_ERR_NOMEM;
                }
                nds = pmix_list_get_size(&apptr->appinfo) + 1;
                PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
                if (NULL == darray) {
                    PMIX_RELEASE(kv);
                    return PMIX_ERR_NOMEM;
                }
                info = (pmix_info_t *) darray->array;
                n = 0;
                /* put in the appnum */
                PMIX_INFO_LOAD(&info[n], PMIX_APPNUM, &apptr->appnum, PMIX_UINT32);
                ++n;
                PMIX_LIST_FOREACH (kp2, &apptr->appinfo, pmix_kval_t) {
                    PMIX_LOAD_KEY(info[n].key, kp2->key);
                    rc = pmix_value_xfer(&info[n].value, kp2->value);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_ARRAY_FREE(darray);
                        PMIX_RELEASE(kv);
                        return rc;
                    }
                    ++n;
                }
                kv->value->data.darray = darray;
                kv->value->type = PMIX_DATA_ARRAY;
                pmix_list_append(kvs, &kv->super);
            }
            return PMIX_SUCCESS;
        }
        /* assume they are asking for our app */
        appnum = pmix_globals.appnum;
    }

    /* scan the list of apps to find the matching entry */
    app = NULL;
    PMIX_LIST_FOREACH (apptr, tgt, pmix_apptrkr_t) {
        if (appnum == apptr->appnum) {
            app = apptr;
            break;
        }
    }
    if (NULL == app) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* see if they wanted to know something about a node that
     * is associated with this app */
    rc = pmix_gds_hash_fetch_nodeinfo(key, trk, &app->nodeinfo, info, ninfo, kvs);
    if (PMIX_ERR_DATA_VALUE_NOT_FOUND != rc) {
        return rc;
    }

    /* scan the info list of this app to generate the results */
    rc = PMIX_ERR_NOT_FOUND;
    PMIX_LIST_FOREACH (kv, &app->appinfo, pmix_kval_t) {
        if (NULL == key || PMIX_CHECK_KEY(kv, key)) {
            kp2 = PMIX_NEW(pmix_kval_t);
            kp2->key = strdup(kv->key);
            kp2->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            rc = pmix_value_xfer(kp2->value, kv->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp2);
                return rc;
            }
            pmix_list_append(kvs, &kp2->super);
            rc = PMIX_SUCCESS;
            if (NULL != key) {
                break;
            }
        }
    }

    return rc;
}

pmix_status_t pmix_gds_hash_fetch(const pmix_proc_t *proc, pmix_scope_t scope, bool copy,
                                  const char *key, pmix_info_t qualifiers[], size_t nqual,
                                  pmix_list_t *kvs)
{
    pmix_job_t *trk;
    pmix_status_t rc;
    pmix_kval_t *kv, *kvptr;
    pmix_info_t *info, *iptr;
    size_t m, n, ninfo, niptr;
    pmix_hash_table_t *ht;
    pmix_session_t *sptr;
    uint32_t sid;
    pmix_rank_t rnk;
    pmix_list_t rkvs;
    bool nodeinfo = false;
    bool appinfo = false;
    bool nigiven = false;
    bool apigiven = false;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "%s pmix:gds:hash fetch %s for proc %s on scope %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), (NULL == key) ? "NULL" : key,
                        PMIX_NAME_PRINT(proc), PMIx_Scope_string(scope));


    PMIX_HIDE_UNUSED_PARAMS(copy);

    /* see if we have a tracker for this nspace - we will
     * if we already cached the job info for it. If we
     * didn't then we'll have no idea how to answer any
     * questions */
    trk = pmix_gds_hash_get_tracker(proc->nspace, false);
    if (NULL == trk) {
        /* let the caller know */
        return PMIX_ERR_INVALID_NAMESPACE;
    }

    /* if the rank is wildcard and the key is NULL, then
     * they are asking for a complete copy of the job-level
     * info for this nspace - retrieve it */
    if (NULL == key && PMIX_RANK_WILDCARD == proc->rank) {
        /* fetch all values from the hash table tied to rank=wildcard */
        rc = dohash(&trk->internal, NULL, PMIX_RANK_WILDCARD, 0, kvs);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            return rc;
        }
        /* also need to add any job-level info */
        PMIX_LIST_FOREACH (kvptr, &trk->jobinfo, pmix_kval_t) {
            kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(kvptr->key);
            kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            PMIX_VALUE_XFER(rc, kv->value, kvptr->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                return rc;
            }
            pmix_list_append(kvs, &kv->super);
        }
        /* collect the relevant node-level info */
        rc = pmix_gds_hash_fetch_nodeinfo(NULL, trk, &trk->nodeinfo, qualifiers, nqual, kvs);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        /* collect the relevant app-level info */
        rc = pmix_gds_hash_fetch_appinfo(NULL, trk, &trk->apps, qualifiers, nqual, kvs);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        /* finally, we need the job-level info for each rank in the job */
        for (rnk = 0; rnk < trk->nptr->nprocs; rnk++) {
            PMIX_CONSTRUCT(&rkvs, pmix_list_t);
            rc = dohash(&trk->internal, NULL, rnk, 2, &rkvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            if (0 == pmix_list_get_size(&rkvs)) {
                PMIX_DESTRUCT(&rkvs);
                continue;
            }
            /* should only have one entry on list */
            kvptr = (pmix_kval_t *) pmix_list_get_first(&rkvs);
            /* we have to assemble the results into a proc blob
             * so the remote end will know what to do with it */
            info = (pmix_info_t *) kvptr->value->data.darray->array;
            ninfo = kvptr->value->data.darray->size;
            /* setup to return the result */
            kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(PMIX_PROC_DATA);
            kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            kv->value->type = PMIX_DATA_ARRAY;
            niptr = ninfo + 1; // need space for the rank
            PMIX_DATA_ARRAY_CREATE(kv->value->data.darray, niptr, PMIX_INFO);
            iptr = (pmix_info_t *) kv->value->data.darray->array;
            /* start with the rank */
            PMIX_INFO_LOAD(&iptr[0], PMIX_RANK, &rnk, PMIX_PROC_RANK);
            /* now transfer rest of data across */
            for (n = 0; n < ninfo; n++) {
                PMIX_INFO_XFER(&iptr[n + 1], &info[n]);
            }
            /* add to the results */
            pmix_list_append(kvs, &kv->super);
            /* release the search result */
            PMIX_LIST_DESTRUCT(&rkvs);
        }
        return PMIX_SUCCESS;
    }

    /* see if they are asking for session, node, or app-level info */
    for (n = 0; n < nqual; n++) {
        if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_SESSION_INFO)) {
            /* they must have provided a session ID */
            for (m = 0; m < nqual; m++) {
                if (PMIX_CHECK_KEY(&qualifiers[m], PMIX_SESSION_ID)) {
                    /* see if we have this session */
                    PMIX_VALUE_GET_NUMBER(rc, &qualifiers[m].value, sid, uint32_t);
                    if (PMIX_SUCCESS != rc) {
                        /* didn't provide a correct value */
                        PMIX_ERROR_LOG(rc);
                        return rc;
                    }
                    PMIX_LIST_FOREACH (sptr, &mca_gds_hash_component.mysessions, pmix_session_t) {
                        if (sptr->session == sid) {
                            /* see if they want info for a specific node */
                            rc = pmix_gds_hash_fetch_nodeinfo(key, trk, &sptr->nodeinfo, qualifiers,
                                                              nqual, kvs);
                            /* if they did, then we are done */
                            if (PMIX_ERR_DATA_VALUE_NOT_FOUND != rc) {
                                return rc;
                            }
                            /* check the session info */
                            PMIX_LIST_FOREACH (kvptr, &sptr->sessioninfo, pmix_kval_t) {
                                if (NULL == key || PMIX_CHECK_KEY(kvptr, key)) {
                                    kv = PMIX_NEW(pmix_kval_t);
                                    kv->key = strdup(kvptr->key);
                                    kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                                    PMIX_VALUE_XFER(rc, kv->value, kvptr->value);
                                    if (PMIX_SUCCESS != rc) {
                                        PMIX_RELEASE(kv);
                                        return rc;
                                    }
                                    pmix_list_append(kvs, &kv->super);
                                    if (NULL != key) {
                                        /* we are done */
                                        return PMIX_SUCCESS;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            /* if we get here, then the session wasn't found */
            return PMIX_ERR_NOT_FOUND;
        } else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_NODE_INFO)) {
            nodeinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            nigiven = true;
        } else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_APP_INFO)) {
            appinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            apigiven = true;
        }
    }

    /* check for node/app keys in the absence of corresponding qualifier */
    if (NULL != key && !nigiven && !apigiven) {
        if (pmix_check_node_info(key)) {
            nodeinfo = true;
        } else if (pmix_check_app_info(key)) {
            appinfo = true;
        }
    }

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(proc->nspace, false);
    if (NULL == trk) {
        return PMIX_ERR_INVALID_NAMESPACE;
    }

    if (!PMIX_RANK_IS_VALID(proc->rank)) {
        if (nodeinfo) {
            rc = pmix_gds_hash_fetch_nodeinfo(key, trk, &trk->nodeinfo, qualifiers, nqual, kvs);
            if (PMIX_SUCCESS != rc && PMIX_RANK_WILDCARD == proc->rank) {
                /* need to check internal as we might have an older peer */
                ht = &trk->internal;
                goto doover;
            }
            return rc;
        } else if (appinfo) {
            rc = pmix_gds_hash_fetch_appinfo(key, trk, &trk->apps, qualifiers, nqual, kvs);
            if (PMIX_SUCCESS != rc && PMIX_RANK_WILDCARD == proc->rank) {
                /* need to check internal as we might have an older peer */
                ht = &trk->internal;
                goto doover;
            }
            return rc;
        }
    }

    /* fetch from the corresponding hash table - note that
     * we always provide a copy as we don't support
     * shared memory */
    if (PMIX_INTERNAL == scope || PMIX_SCOPE_UNDEF == scope || PMIX_GLOBAL == scope
        || PMIX_RANK_WILDCARD == proc->rank) {
        ht = &trk->internal;
    } else if (PMIX_LOCAL == scope || PMIX_GLOBAL == scope) {
        ht = &trk->local;
    } else if (PMIX_REMOTE == scope) {
        ht = &trk->remote;
    } else {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

doover:
    /* if rank=PMIX_RANK_UNDEF, then we need to search all
     * known ranks for this nspace as any one of them could
     * be the source */
    if (PMIX_RANK_UNDEF == proc->rank) {
        for (rnk = 0; rnk < trk->nptr->nprocs; rnk++) {
            rc = dohash(ht, key, rnk, true, kvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            if (PMIX_SUCCESS == rc && NULL != key) {
                return rc;
            }
        }
        /* also need to check any job-level info */
        PMIX_LIST_FOREACH (kvptr, &trk->jobinfo, pmix_kval_t) {
            if (NULL == key || PMIX_CHECK_KEY(kvptr, key)) {
                kv = PMIX_NEW(pmix_kval_t);
                kv->key = strdup(kvptr->key);
                kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                PMIX_VALUE_XFER(rc, kv->value, kvptr->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_RELEASE(kv);
                    return rc;
                }
                pmix_list_append(kvs, &kv->super);
                if (NULL != key) {
                    break;
                }
            }
        }
        if (NULL == key) {
            /* and need to add all job info just in case that was
             * passed via a different GDS component */
            rc = dohash(&trk->internal, NULL, PMIX_RANK_WILDCARD, false, kvs);
        } else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    } else {
        rc = dohash(ht, key, proc->rank, false, kvs);
    }
    if (PMIX_SUCCESS == rc) {
        if (PMIX_GLOBAL == scope) {
            if (ht == &trk->local) {
                /* need to do this again for the remote data */
                ht = &trk->remote;
                goto doover;
            } else if (ht == &trk->internal) {
                /* check local */
                ht = &trk->local;
                goto doover;
            }
        }
    } else {
        if (PMIX_GLOBAL == scope || PMIX_SCOPE_UNDEF == scope) {
            if (ht == &trk->internal) {
                /* need to also try the local data */
                ht = &trk->local;
                goto doover;
            } else if (ht == &trk->local) {
                /* need to also try the remote data */
                ht = &trk->remote;
                goto doover;
            }
        }
    }
    if (0 == pmix_list_get_size(kvs)) {
        /* if we didn't find it and the rank was valid, then
         * check to see if the data exists in a different scope.
         * This is done to avoid having the process go into a
         * timeout wait when the data will never appear within
         * the specified scope */
        if (PMIX_RANK_IS_VALID(proc->rank)) {
            if (PMIX_LOCAL == scope) {
                /* check the remote scope */
                rc = dohash(&trk->remote, key, proc->rank, false, kvs);
                if (PMIX_SUCCESS == rc || 0 < pmix_list_get_size(kvs)) {
                    while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(kvs))) {
                        PMIX_RELEASE(kv);
                    }
                    rc = PMIX_ERR_EXISTS_OUTSIDE_SCOPE;
                } else {
                    rc = PMIX_ERR_NOT_FOUND;
                }
            } else if (PMIX_REMOTE == scope) {
                /* check the local scope */
                rc = dohash(&trk->local, key, proc->rank, false, kvs);
                if (PMIX_SUCCESS == rc || 0 < pmix_list_get_size(kvs)) {
                    while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(kvs))) {
                        PMIX_RELEASE(kv);
                    }
                    rc = PMIX_ERR_EXISTS_OUTSIDE_SCOPE;
                } else {
                    rc = PMIX_ERR_NOT_FOUND;
                }
            }
        } else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    }

    return rc;
}
