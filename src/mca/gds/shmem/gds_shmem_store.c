/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem_store.h"
#include "gds_shmem_utils.h"
// TODO(skg) This will eventually go away.
#include "pmix_hash2.h"

#include "src/common/pmix_iof.h"

#include "src/mca/pcompress/base/base.h"

#include "src/mca/pmdl/pmdl.h"

static pmix_status_t
cache_node_info(
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *cache,
    pmix_gds_shmem_nodeinfo_t **nodeinfo
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Construct the cache. We'll cleanup after ourselves if something fails.
    PMIX_CONSTRUCT(cache, pmix_list_t);
    pmix_gds_shmem_nodeinfo_t *inodeinfo = NULL;
    // Cache the values while searching for the nodeid and/or hostname.
    for (size_t j = 0; j < ninfo; j++) {
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s for key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), info[j].key
        );
        if (PMIX_CHECK_KEY(&info[j], PMIX_NODEID)) {
            if (!inodeinfo) {
                inodeinfo = PMIX_NEW(pmix_gds_shmem_nodeinfo_t);
            }
            PMIX_VALUE_GET_NUMBER(
                rc, &info[j].value, inodeinfo->nodeid, uint32_t
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_HOSTNAME)) {
            if (!inodeinfo) {
                inodeinfo = PMIX_NEW(pmix_gds_shmem_nodeinfo_t);
            }
            inodeinfo->hostname = strdup(info[j].value.data.string);
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_HOSTNAME_ALIASES)) {
            if (!inodeinfo) {
                inodeinfo = PMIX_NEW(pmix_gds_shmem_nodeinfo_t);
            }
            inodeinfo->aliases = pmix_argv_split(info[j].value.data.string, ',');
            // Need to cache this value as well.
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(info[j].key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, &info[j].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                break;
            }
            pmix_list_append(cache, &kv->super);
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(info[j].key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, &info[j].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                break;
            }
            pmix_list_append(cache, &kv->super);
        }
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_LIST_DESTRUCT(cache);
        PMIX_RELEASE(inodeinfo);
        inodeinfo = NULL;
    }
    *nodeinfo = inodeinfo;
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_node_array(
    pmix_value_t *val,
    pmix_list_t *target
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_nodeinfo_t *nodeinfo = NULL;
    bool update = false;

    PMIX_GDS_SHMEM_VOUT(
        "%s: STORING NODE ARRAY",
        PMIX_NAME_PRINT(&pmix_globals.myid)
    );
    // We are expecting an array of node-level info for a
    // specific node. Make sure we were given the correct type.
    if (PMIX_DATA_ARRAY != val->type) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Construct node info cache from provided data.
    pmix_list_t cache;
    rc = cache_node_info(
        (pmix_info_t *)val->data.darray->array,
        val->data.darray->size, &cache, &nodeinfo
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Make sure that the caller passed us node identifier info.
    if (!nodeinfo) {
        rc = PMIX_ERR_BAD_PARAM;
        goto out;
    }
    // See if we already have this node on the provided list.
    pmix_gds_shmem_nodeinfo_t *ni;
    PMIX_LIST_FOREACH (ni, target, pmix_gds_shmem_nodeinfo_t) {
        if (UINT32_MAX != ni->nodeid && UINT32_MAX != nodeinfo->nodeid) {
            if (ni->nodeid == nodeinfo->nodeid) {
                if (NULL == ni->hostname && NULL != nodeinfo->hostname) {
                    ni->hostname = strdup(nodeinfo->hostname);
                }
                if (NULL != nodeinfo->aliases) {
                    for (size_t n = 0; NULL != nodeinfo->aliases[n]; n++) {
                        pmix_argv_append_unique_nosize(
                            &ni->aliases, nodeinfo->aliases[n]
                        );
                    }
                }
                PMIX_RELEASE(nodeinfo);
                nodeinfo = ni;
                update = true;
                break;
            }
        }
        else if (NULL != ni->hostname && NULL != nodeinfo->hostname) {
            if (0 == strcmp(ni->hostname, nodeinfo->hostname)) {
                if (UINT32_MAX == ni->nodeid && UINT32_MAX != nodeinfo->nodeid) {
                    ni->nodeid = nodeinfo->nodeid;
                }
                if (NULL != nodeinfo->aliases) {
                    for (size_t n = 0; NULL != nodeinfo->aliases[n]; n++) {
                        pmix_argv_append_unique_nosize(
                            &ni->aliases, nodeinfo->aliases[n]
                        );
                    }
                }
                PMIX_RELEASE(nodeinfo);
                nodeinfo = ni;
                update = true;
                break;
            }
        }
    }
    // If the target didn't have the info, then add it.
    if (!update) {
        pmix_list_append(target, &nodeinfo->super);
    }
    // TODO(skg) I think this is wasted work in the not updated case: nodeinfo
    // will not be updated to point to an item in the target's list, so
    // processing this might not make sense. Ask Ralph.
    // Transfer the cached items to the nodeinfo list.
    pmix_kval_t *cache_kv;
    cache_kv = (pmix_kval_t *)pmix_list_remove_first(&cache);
    while (NULL != cache_kv) {
        // If this is an update, we have to ensure each
        // data item only appears once on the list.
        if (update) {
            pmix_kval_t *kv;
            PMIX_LIST_FOREACH (kv, &nodeinfo->info, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kv, cache_kv->key)) {
                    pmix_list_remove_item(&nodeinfo->info, &kv->super);
                    PMIX_RELEASE(kv);
                    break;
                }
            }
        }
        pmix_list_append(&nodeinfo->info, &cache_kv->super);
        cache_kv = (pmix_kval_t *)pmix_list_remove_first(&cache);
    }
out:
    PMIX_LIST_DESTRUCT(&cache);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_app_array(
    pmix_value_t *val,
    pmix_gds_shmem_job_t *job
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_app_t *app = NULL;
    bool update = false;

    PMIX_GDS_SHMEM_VOUT(
        "%s: PROCESSING APP ARRAY",
        PMIX_NAME_PRINT(&pmix_globals.myid)
    );
    // Apps must belong to a job.
    if (!job) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    // We are expecting an array of app-level info.
    // Make sure we were given the correct type.
    if (PMIX_DATA_ARRAY != val->type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    // setup arrays and lists.
    pmix_list_t app_cache, node_cache;
    PMIX_CONSTRUCT(&app_cache, pmix_list_t);
    PMIX_CONSTRUCT(&node_cache, pmix_list_t);

    const size_t size = val->data.darray->size;
    pmix_info_t *info = (pmix_info_t *)val->data.darray->array;
    for (size_t j = 0; j < size; j++) {
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s for key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), info[j].key
        );
        if (PMIX_CHECK_KEY(&info[j], PMIX_APPNUM)) {
            uint32_t appnum;
            PMIX_VALUE_GET_NUMBER(rc, &info[j].value, appnum, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
            // This is an error: there can be only
            // one app described in this array.
            if (NULL != app) {
                PMIX_RELEASE(app);
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_ERROR_LOG(rc);
                goto out;
            }
            app = PMIX_NEW(pmix_gds_shmem_app_t);
            app->appnum = appnum;
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_node_array(&info[j].value, &node_cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(info[j].key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, &info[j].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                goto out;
            }
            pmix_list_append(&app_cache, &kv->super);
        }
    }

    if (NULL == app) {
        // Per the standard, they don't have to provide us with
        // an appnum so long as only one app is in the job.
        if (0 == pmix_list_get_size(&job->apps)) {
            app = PMIX_NEW(pmix_gds_shmem_app_t);
            app->appnum = 0;
        }
        else {
            // This is not allowed to happen: they are required
            // to provide us with an app number per the standard.
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            goto out;
        }
    }
    // See if we already have this app on the provided list.
    pmix_gds_shmem_app_t *appi;
    PMIX_LIST_FOREACH (appi, &job->apps, pmix_gds_shmem_app_t) {
        if (appi->appnum == app->appnum) {
            // We assume that the data is updating the current values.
            PMIX_RELEASE(app);
            app = appi;
            update = true;
            break;
        }
    }
    // We didn't already have it, so add it now.
    if (!update) {
        pmix_list_append(&job->apps, &app->super);
    }
    // Point the app at its job.
    if (NULL == app->job) {
        // Do NOT retain the tracker. We will not release it in the app
        // destructor. If we retain the tracker, then we won't release it later
        // because the refcount is wrong.
        app->job = job;
    }
    // Transfer the app-level data across.
    pmix_kval_t *akv;
    akv = (pmix_kval_t *)pmix_list_remove_first(&app_cache);
    while (NULL != akv) {
        // If this is an update, we have to ensure each
        // data item only appears once on the list.
        if (update) {
            pmix_kval_t *kv;
            PMIX_LIST_FOREACH (kv, &app->appinfo, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kv, akv->key)) {
                    pmix_list_remove_item(&app->appinfo, &kv->super);
                    PMIX_RELEASE(kv);
                    break;
                }
            }
        }
        if (PMIX_CHECK_KEY(akv, PMIX_MODEL_LIBRARY_NAME) ||
            PMIX_CHECK_KEY(akv, PMIX_PROGRAMMING_MODEL) ||
            PMIX_CHECK_KEY(akv, PMIX_MODEL_LIBRARY_VERSION) ||
            PMIX_CHECK_KEY(akv, PMIX_PERSONALITY)) {
            // Pass this info to the pmdl framework
            pmix_pmdl.setup_nspace_kv(job->nspace, akv);
        }
        pmix_list_append(&app->appinfo, &akv->super);
        akv = (pmix_kval_t *)pmix_list_remove_first(&app_cache);
    }
    // Transfer the associated node-level data across.
    pmix_gds_shmem_nodeinfo_t *nd;
    nd = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(&node_cache);
    while (NULL != nd) {
        pmix_list_append(&app->nodeinfo, &nd->super);
        nd = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(&node_cache);
    }
out:
    PMIX_LIST_DESTRUCT(&app_cache);
    PMIX_LIST_DESTRUCT(&node_cache);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_session_array(
    pmix_value_t *val,
    pmix_gds_shmem_job_t *job
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_session_t *session = NULL;

    PMIX_GDS_SHMEM_VOUT(
        "%s: PROCESSING SESSION ARRAY",
        PMIX_NAME_PRINT(&pmix_globals.myid)
    );
    // We are expecting an array of session-level info.
    // Make sure we were given the correct type.
    if (PMIX_DATA_ARRAY != val->type) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    // Construct the node and session caches.
    pmix_list_t sesh_cache, node_cache;
    PMIX_CONSTRUCT(&sesh_cache, pmix_list_t);
    PMIX_CONSTRUCT(&node_cache, pmix_list_t);

    const size_t size = val->data.darray->size;
    pmix_info_t *info = (pmix_info_t *)val->data.darray->array;
    for (size_t j = 0; j < size; j++) {
        if (PMIX_CHECK_KEY(&info[j], PMIX_SESSION_ID)) {
            uint32_t sid;
            PMIX_VALUE_GET_NUMBER(rc, &info[j].value, sid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
            // See if we already have this session - it could have
            // been defined by a separate PMIX_SESSION_ID key.
            pmix_gds_shmem_component_t *comp = &pmix_mca_gds_shmem_component;
            pmix_gds_shmem_session_t *si;
            PMIX_LIST_FOREACH (si, &comp->sessions, pmix_gds_shmem_session_t) {
                if (si->id == sid) {
                    session = si;
                    break;
                }
            }
            if (NULL == session) {
                /* wasn't found, so create one */
                session = PMIX_NEW(pmix_gds_shmem_session_t);
                session->id = sid;
                pmix_list_append(&comp->sessions, &session->super);
            }
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_node_array(&info[j].value, &node_cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(info[j].key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, &info[j].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                goto out;
            }
            pmix_list_append(&sesh_cache, &kv->super);
        }
    }
    // This is not allowed to happen: they are required
    // to provide us with a session ID per the standard.
    if (NULL == session) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Point the job at it.
    if (NULL != job->session) {
        PMIX_RELEASE(job->session);
    }
    PMIX_RETAIN(session);
    job->session = session;
    // Transfer the session data across.
    pmix_kval_t *skv;
    skv = (pmix_kval_t *)pmix_list_remove_first(&sesh_cache);
    while (NULL != skv) {
        pmix_list_append(&session->sessioninfo, &skv->super);
        skv = (pmix_kval_t *)pmix_list_remove_first(&sesh_cache);
    }
    // Transfer the node info data across.
    pmix_gds_shmem_nodeinfo_t *ni;
    ni = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(&node_cache);
    while (NULL != ni) {
        pmix_list_append(&session->nodeinfo, &ni->super);
        ni = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(&node_cache);
    }
out:
    PMIX_LIST_DESTRUCT(&sesh_cache);
    PMIX_LIST_DESTRUCT(&node_cache);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_proc_data_in_hashtab(
    const pmix_kval_t *kval,
    const char *namespace_id,
    pmix_hash_table_t *target_ht
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // First, make sure this is proc data.
    if (!PMIX_CHECK_KEY(kval, PMIX_PROC_DATA)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    // We are expecting an array of data pertaining to a specific proc.
    if (PMIX_DATA_ARRAY != kval->value->type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }

    const size_t size = kval->value->data.darray->size;
    pmix_info_t *iptr = (pmix_info_t *)kval->value->data.darray->array;
    // First element of the array must be the rank.
    if (0 != strcmp(iptr[0].key, PMIX_RANK) ||
        PMIX_PROC_RANK != iptr[0].value.type) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    const pmix_rank_t rank = iptr[0].value.data.rank;
    // Cycle through the values for this rank and store them.
    for (size_t j = 1; j < size; j++) {
        pmix_kval_t kv;
        if (PMIX_CHECK_KEY(&iptr[j], PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_shmem_store_qualified(
                target_ht, rank, &iptr[j].value
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            continue;
        }
        kv.key = iptr[j].key;
        kv.value = &iptr[j].value;
        PMIX_GDS_SHMEM_VOUT(
            "%s: %s for nspace=%s rank=%u key=%s",
            __func__, PMIX_NAME_PRINT(&pmix_globals.myid),
            namespace_id, rank, kv.key
        );
        // Store it in the hash_table.
        rc = pmix_hash2_store(target_ht, rank, &kv, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_qualified(
    pmix_hash_table_t *ht,
    pmix_rank_t rank,
    pmix_value_t *value
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // The value contains a pmix_data_array_t whose first position contains the
    // key-value being stored, followed by one or more qualifiers.
    pmix_info_t *info = (pmix_info_t *)value->data.darray->array;
    const size_t sz = value->data.darray->size;
    // Extract the primary value.
    pmix_kval_t kv;
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = info[0].key;
    kv.value = &info[0].value;

    const size_t nquals = sz - 1;
    pmix_info_t *quals;
    PMIX_INFO_CREATE(quals, nquals);
    for (size_t n = 1; n < sz; n++) {
        PMIX_INFO_SET_QUALIFIER(&quals[n - 1]);
        PMIX_INFO_XFER(&quals[n - 1], &info[n]);
    }
    // Store the result.
    rc = pmix_hash2_store(ht, rank, &kv, quals, nquals);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_INFO_FREE(quals, nquals);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_job_array(
    pmix_info_t *info,
    pmix_gds_shmem_job_t *job,
    uint32_t *flags,
    char ***procs,
    char ***nodes
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s STORING JOB ARRAY", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid)
    );

    // We are expecting an array of job-level info.
    if (PMIX_DATA_ARRAY != info->value.type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }

    pmix_list_t cache;
    PMIX_CONSTRUCT(&cache, pmix_list_t);

    const size_t size = info->value.data.darray->size;
    pmix_info_t *iptr = (pmix_info_t *)info->value.data.darray->array;
    for (size_t j = 0; j < size; j++) {
        if (PMIX_CHECK_KEY(&iptr[j], PMIX_APP_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_app_array(&iptr[j].value, job);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        else if (PMIX_CHECK_KEY(&iptr[j], PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_node_array(
                &iptr[j].value, &job->nodeinfo
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        else if (PMIX_CHECK_KEY(&iptr[j], PMIX_PROC_MAP)) {
            // TODO(skg) A candidate for shared-memory?
            // Not allowed to get this more than once.
            if (*flags & PMIX_GDS_SHMEM_PROC_MAP) {
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            // Parse the regex to get the argv array
            // containing proc ranks on each node.
            rc = pmix_preg.parse_procs(iptr[j].value.data.bo.bytes, procs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            // Mark that we got the map.
            *flags |= PMIX_GDS_SHMEM_PROC_MAP;
        }
        else if (PMIX_CHECK_KEY(&iptr[j], PMIX_NODE_MAP)) {
            // TODO(skg) A candidate for shared-memory?
            // Not allowed to get this more than once.
            if (*flags & PMIX_GDS_SHMEM_NODE_MAP) {
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            // Parse the regex to get the argv array of node names.
            rc = pmix_preg.parse_nodes(iptr[j].value.data.bo.bytes, nodes);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            /* mark that we got the map */
            *flags |= PMIX_GDS_SHMEM_NODE_MAP;
        }
        else if (PMIX_CHECK_KEY(&iptr[j], PMIX_MODEL_LIBRARY_NAME) ||
                 PMIX_CHECK_KEY(&iptr[j], PMIX_PROGRAMMING_MODEL) ||
                 PMIX_CHECK_KEY(&iptr[j], PMIX_MODEL_LIBRARY_VERSION) ||
                 PMIX_CHECK_KEY(&iptr[j], PMIX_PERSONALITY)) {
            // Pass this info to the pmdl framework.
            pmix_pmdl.setup_nspace(job->nspace, &iptr[j]);
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(iptr[j].key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, &iptr[j].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_LIST_DESTRUCT(&cache);
                return rc;
            }
            pmix_list_append(&job->jobinfo, &kv->super);
            // Check for job size.
            if (PMIX_CHECK_KEY(&iptr[j], PMIX_JOB_SIZE)) {
                if (!(PMIX_GDS_SHMEM_JOB_SIZE & *flags)) {
                    job->nspace->nprocs = iptr[j].value.data.uint32;
                    *flags |= PMIX_GDS_SHMEM_JOB_SIZE;
                }
            }
            else if (PMIX_CHECK_KEY(&iptr[j], PMIX_DEBUG_STOP_ON_EXEC) ||
                     PMIX_CHECK_KEY(&iptr[j], PMIX_DEBUG_STOP_IN_INIT) ||
                     PMIX_CHECK_KEY(&iptr[j], PMIX_DEBUG_STOP_IN_APP)) {
                //
                if (PMIX_RANK_WILDCARD == iptr[j].value.data.rank) {
                    job->nspace->num_waiting = job->nspace->nlocalprocs;
                }
                else {
                    job->nspace->num_waiting = 1;
                }
            }
            else {
                pmix_iof_check_flags(&iptr[j], &job->nspace->iof_flags);
            }
        }
    }
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_map(
    pmix_gds_shmem_job_t *job,
    char **nodes,
    char **ppn,
    uint32_t flags
) {
    pmix_status_t rc = PMIX_SUCCESS;
    uint32_t totalprocs = 0;
    pmix_hash_table_t *ht = &job->hashtab_internal;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s STORING MAP", __func__, PMIX_NAME_PRINT(&pmix_globals.myid)
    );
    // If the list sizes don't match, then that's an error.
    if (pmix_argv_count(nodes) != pmix_argv_count(ppn)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    // If they didn't provide the number of nodes,
    // then compute it from the list of nodes.
    if (!(PMIX_GDS_SHMEM_NUM_NODES & flags)) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_NUM_NODES);
        kv->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        kv->value->type = PMIX_UINT32;
        kv->value->data.uint32 = pmix_argv_count(nodes);
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s adding key=%s to job info", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kv->key
        );
        rc = pmix_hash2_store(ht, PMIX_RANK_WILDCARD, kv, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        PMIX_RELEASE(kv); // Maintain accounting.
    }

    for (size_t n = 0; NULL != nodes[n]; n++) {
        pmix_gds_shmem_nodeinfo_t *nd = NULL;
        pmix_kval_t *kvi = NULL;
        // Check and see if we already have this node.
        nd = pmix_gds_shmem_get_nodeinfo_by_nodename(&job->nodeinfo, nodes[n]);
        if (NULL == nd) {
            nd = PMIX_NEW(pmix_gds_shmem_nodeinfo_t);
            nd->hostname = strdup(nodes[n]);
            nd->nodeid = n;
            pmix_list_append(&job->nodeinfo, &nd->super);
        }
        // Store the proc list as-is.
        pmix_kval_t *kval = PMIX_NEW(pmix_kval_t);
        if (NULL == kval) {
            return PMIX_ERR_NOMEM;
        }
        kval->key = strdup(PMIX_LOCAL_PEERS);
        kval->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        if (NULL == kval->value) {
            PMIX_RELEASE(kval);
            return PMIX_ERR_NOMEM;
        }
        kval->value->type = PMIX_STRING;
        kval->value->data.string = strdup(ppn[n]);

        PMIX_GDS_SHMEM_VOUT(
            "%s:%s adding key=%s to node=%s info", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kval->key, nodes[n]
        );
        // Ensure this item only appears once on the list.
        PMIX_LIST_FOREACH (kvi, &nd->info, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kvi, kval->key)) {
                pmix_list_remove_item(&nd->info, &kvi->super);
                PMIX_RELEASE(kvi);
                break;
            }
        }
        pmix_list_append(&nd->info, &kval->super);

        // Save the local leader.
        pmix_rank_t rank = strtoul(ppn[n], NULL, 10);
        kval = PMIX_NEW(pmix_kval_t);
        if (NULL == kval) {
            return PMIX_ERR_NOMEM;
        }
        kval->key = strdup(PMIX_LOCALLDR);
        kval->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        if (NULL == kval->value) {
            PMIX_RELEASE(kval);
            return PMIX_ERR_NOMEM;
        }
        kval->value->type = PMIX_PROC_RANK;
        kval->value->data.rank = rank;
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s adding key=%s to node=%s info", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kval->key, nodes[n]
        );
        // Ensure this item only appears once on the list.
        PMIX_LIST_FOREACH (kvi, &nd->info, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kvi, kval->key)) {
                pmix_list_remove_item(&nd->info, &kvi->super);
                PMIX_RELEASE(kvi);
                break;
            }
        }
        pmix_list_append(&nd->info, &kval->super);

        // Split the list of procs so we can
        // store their individual location data.
        char **procs = pmix_argv_split(ppn[n], ',');
        // Save the local size in case they don't give it to us.
        kval = PMIX_NEW(pmix_kval_t);
        if (NULL == kval) {
            pmix_argv_free(procs);
            return PMIX_ERR_NOMEM;
        }
        kval->key = strdup(PMIX_LOCAL_SIZE);
        kval->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        if (NULL == kval->value) {
            PMIX_RELEASE(kval);
            pmix_argv_free(procs);
            return PMIX_ERR_NOMEM;
        }
        kval->value->type = PMIX_UINT32;
        kval->value->data.uint32 = pmix_argv_count(procs);
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s adding key=%s to node=%s info", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kval->key, nodes[n]
        );
        // Ensure this item only appears once on the list.
        PMIX_LIST_FOREACH (kvi, &nd->info, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kvi, kval->key)) {
                pmix_list_remove_item(&nd->info, &kvi->super);
                PMIX_RELEASE(kvi);
                break;
            }
        }
        pmix_list_append(&nd->info, &kval->super);

        // Track total procs in job in case they didn't give it to us.
        totalprocs += pmix_argv_count(procs);
        for (size_t m = 0; NULL != procs[m]; m++) {
            // Store the hostname for each proc.
            kval = PMIX_NEW(pmix_kval_t);
            kval->key = strdup(PMIX_HOSTNAME);
            kval->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            kval->value->type = PMIX_STRING;
            kval->value->data.string = strdup(nodes[n]);
            rank = strtol(procs[m], NULL, 10);
            PMIX_GDS_SHMEM_VOUT(
                "%s:%s for [%s:%u]: key=%s", __func__,
                PMIX_NAME_PRINT(&pmix_globals.myid),
                job->nspace_id, rank, kval->key
            );
            rc = pmix_hash2_store(ht, rank, kval, NULL, 0);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kval);
                pmix_argv_free(procs);
                return rc;
            }
            PMIX_RELEASE(kval); // Maintain accounting.
            if (!(PMIX_GDS_SHMEM_PROC_DATA & flags)) {
                // Add an entry for the nodeid.
                kval = PMIX_NEW(pmix_kval_t);
                kval->key = strdup(PMIX_NODEID);
                kval->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                kval->value->type = PMIX_UINT32;
                PMIX_GDS_SHMEM_VOUT(
                    "%s:%s for [%s:%u]: key=%s", __func__,
                    PMIX_NAME_PRINT(&pmix_globals.myid),
                    job->nspace_id, rank, kval->key
                );
                kval->value->data.uint32 = n;
                rc = pmix_hash2_store(ht, rank, kval, NULL, 0);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kval);
                    pmix_argv_free(procs);
                    return rc;
                }
                PMIX_RELEASE(kval); // Maintain accounting.
                // Add an entry for the local rank.
                kval = PMIX_NEW(pmix_kval_t);
                kval->key = strdup(PMIX_LOCAL_RANK);
                kval->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
                kval->value->type = PMIX_UINT16;
                kval->value->data.uint16 = m;
                PMIX_GDS_SHMEM_VOUT(
                    "%s:%s for [%s:%u]: key=%s", __func__,
                    PMIX_NAME_PRINT(&pmix_globals.myid),
                    job->nspace_id, rank, kval->key
                );
                rc = pmix_hash2_store(ht, rank, kval, NULL, 0);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kval);
                    pmix_argv_free(procs);
                    return rc;
                }
                PMIX_RELEASE(kval); // Maintain accounting.
                // Add an entry for the node rank. For now
                // we assume only the one job is running.
                kval = PMIX_NEW(pmix_kval_t);
                kval->key = strdup(PMIX_NODE_RANK);
                kval->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
                kval->value->type = PMIX_UINT16;
                kval->value->data.uint16 = m;
                PMIX_GDS_SHMEM_VOUT(
                    "%s:%s for [%s:%u]: key=%s", __func__,
                    PMIX_NAME_PRINT(&pmix_globals.myid),
                    job->nspace_id, rank, kval->key
                );
                rc = pmix_hash2_store(ht, rank, kval, NULL, 0);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kval);
                    pmix_argv_free(procs);
                    return rc;
                }
                PMIX_RELEASE(kval); // Maintain accounting.
            }
        }
        pmix_argv_free(procs);
    }
    // Store the comma-delimited list of nodes hosting
    // procs in this nspace in case someone using PMIx v2
    // requests it.
    pmix_kval_t *kval = PMIX_NEW(pmix_kval_t);
    kval->key = strdup(PMIX_NODE_LIST);
    kval->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
    kval->value->type = PMIX_STRING;
    kval->value->data.string = pmix_argv_join(nodes, ',');
    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for nspace=%s: key=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        job->nspace_id, kval->key
    );

    rc = pmix_hash2_store(ht, PMIX_RANK_WILDCARD, kval, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kval);
        return rc;
    }
    PMIX_RELEASE(kval); // Maintain accounting.

    // If they didn't provide the job size, compute it as being the number of
    // provided procs (i.e., size of ppn list).
    if (!(PMIX_GDS_SHMEM_JOB_SIZE & flags)) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_JOB_SIZE);
        kv->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        kv->value->type = PMIX_UINT32;
        kv->value->data.uint32 = totalprocs;
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s for nspace=%s: key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid),
            job->nspace_id, kv->key
        );
        rc = pmix_hash2_store(ht, PMIX_RANK_WILDCARD, kv, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        PMIX_RELEASE(kv); // Maintain accounting.
        flags |= PMIX_GDS_SHMEM_JOB_SIZE;
        job->nspace->nprocs = totalprocs;
    }

    // If they didn't provide a value for max procs, just assume it is the same
    // as the number of procs in the job and store it.
    if (!(PMIX_GDS_SHMEM_MAX_PROCS & flags)) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_MAX_PROCS);
        kv->value = (pmix_value_t *)malloc(sizeof(pmix_value_t));
        kv->value->type = PMIX_UINT32;
        kv->value->data.uint32 = totalprocs;
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s for nspace=%s: key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid),
            job->nspace_id, kv->key
        );

        rc = pmix_hash2_store(ht, PMIX_RANK_WILDCARD, kv, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        PMIX_RELEASE(kv); // Maintain accounting.
        flags |= PMIX_GDS_SHMEM_MAX_PROCS;
    }
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
