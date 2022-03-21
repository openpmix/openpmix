/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem_fetch.h"
#include "gds_shmem_utils.h"

static pmix_status_t
node_info_fetch(
    pmix_gds_shmem_job_t *job,
    const char *key,
    pmix_list_t *tgt,
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *kvs
) {
#if 0
    size_t nds;
    pmix_status_t rc;
    uint32_t nid = UINT32_MAX;
    char *hostname = NULL;
    bool found = false;
    pmix_gds_shmem_nodeinfo_t *nd, *ndptr;
    pmix_kval_t *kv, *kp2;
    pmix_data_array_t *darray;
    pmix_info_t *iptr;
#endif
    PMIX_HIDE_UNUSED_PARAMS(job, key, tgt, info, ninfo, kvs);

    PMIX_GDS_SHMEM_VOUT(
        "%s fetching NODE INFO",
        PMIX_NAME_PRINT(&pmix_globals.myid)
    );

    return PMIX_ERR_NOT_SUPPORTED;
#if 0
    // Scan for the nodeID or hostname to
    // identify which node they are asking about.
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, nid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            found = true;
            break;
        }
        else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
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
                    rc = PMIx_Value_xfer(&iptr[n].value, kp2->value);
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
            rc = PMIx_Value_xfer(&iptr[n].value, kp2->value);
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
            rc = PMIx_Value_xfer(kv->value, kp2->value);
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
#endif
}

/**
 * Fetches session information.
 */
static pmix_status_t
session_info_fetch(
    pmix_gds_shmem_job_t *job,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM_VOUT(
        "%s fetching PMIX_SESSION_INFO key=%s",
        PMIX_NAME_PRINT(&pmix_globals.myid),
        (NULL == key) ? "NULL" : key
    );

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_component_t *component = &pmix_mca_gds_shmem_component;

    uint32_t sid = 0;
    for (size_t m = 0; m < nqual; m++) {
        // Not a session ID, so continue searching.
        if (!PMIX_CHECK_KEY(&qualifiers[m], PMIX_SESSION_ID)) {
            continue;
        }
        // A session ID!
        PMIX_VALUE_GET_NUMBER(rc, &qualifiers[m].value, sid, uint32_t);
        if (PMIX_SUCCESS != rc) {
            // Didn't provide a correct value.
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        // Do we have this session?
        pmix_gds_shmem_session_t *s = NULL;
        PMIX_LIST_FOREACH (s, &component->sessions, pmix_gds_shmem_session_t) {
            // Not the one that was requested, so continue searching.
            if (s->id != sid) {
                continue;
            }
            // We must have found the requested session ID,
            // so see if they want info for a specific node.
            rc = node_info_fetch(
                job, key, &s->nodeinfo,
                qualifiers, nqual, kvs
            );
            // If they did, then we are done!
            if (PMIX_ERR_DATA_VALUE_NOT_FOUND != rc) {
                return rc;
            }
            // TODO(skg) Not in shared-memory. Can we support sessioninfos?
            // Else, check the session info.
            pmix_kval_t *kv = NULL;
            PMIX_LIST_FOREACH (kv, &s->sessioninfo, pmix_kval_t) {
                if (NULL == key || PMIX_CHECK_KEY(kv, key)) {
                    pmix_kval_t *res = NULL;
                    res = PMIX_NEW(pmix_kval_t);
                    res->key = strdup(kv->key);
                    res->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
                    PMIX_VALUE_XFER(rc, res->value, kv->value);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_RELEASE(res);
                        return rc;
                    }
                    pmix_list_append(kvs, &res->super);
                    if (NULL != key) {
                        // We are done.
                        return PMIX_SUCCESS;
                    }
                }
            }
        }
    }
    // If we get here, then the session wasn't found.
    return PMIX_ERR_NOT_FOUND;
}

pmix_status_t
pmix_gds_shmem_fetch(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM_UNUSED(copy);

    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s fetching key=%s for proc=%s on scope=%s",
        PMIX_NAME_PRINT(&pmix_globals.myid),
        (NULL == key) ? "NULL" : key,
        PMIX_NAME_PRINT(proc),
        PMIx_Scope_string(scope)
    );
    // Get the tracker for this job. We should have already created one, so
    // that's why we pass false in pmix_gds_shmem_get_job_tracker().
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // TODO(skg) Implement this case
    // if (NULL == key && PMIX_RANK_WILDCARD == proc->rank) {
    //     ...
    // }
    /* see if they are asking for session, node, or app-level info */
    for (size_t n = 0; n < nqual; n++) {
        if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_SESSION_INFO)) {
            return session_info_fetch(
                job, key, qualifiers, nqual, kvs
            );
        }
#if 0
        else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_NODE_INFO)) {
            nodeinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            nigiven = true;
        } else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_APP_INFO)) {
            appinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            apigiven = true;
        }
#endif
    }
out:
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
