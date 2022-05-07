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

#include "src/mca/pmdl/pmdl.h"
#include "src/mca/pcompress/base/base.h"

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
            "%s processing node array key=%s",
            PMIX_NAME_PRINT(&pmix_globals.myid),
            info[j].key
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
        "%s: PROCESSING NODE ARRAY",
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
    // Apps have to belong to a job.
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
            "%s processing app array key=%s",
            PMIX_NAME_PRINT(&pmix_globals.myid),
            info[j].key
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
        PMIX_RETAIN(job);
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
    if (PMIX_DATA_ARRAY != kval->value->type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    // Because this is proc data we have to expand
    // it and store the values on that rank.
    pmix_info_t *info = (pmix_info_t *)kval->value->data.darray->array;
    // First element of the array must be the rank.
    if (0 != strcmp(info[0].key, PMIX_RANK) ||
        PMIX_PROC_RANK != info[0].value.type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    const pmix_rank_t rank = info[0].value.data.rank;
    const size_t size = kval->value->data.darray->size;
    // Cycle through the values for this rank and store them.
    for (size_t j = 1; j < size; j++) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            rc = PMIX_ERR_NOMEM;
            return rc;
        }
        kv->key = strdup(info[j].key);
        // TODO(skg) Look at constructor to see if this is required.
        kv->value = NULL;
        PMIX_VALUE_XFER(rc, kv->value, &info[j].value);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        // If the value contains a string that is
        // longer than the limit, then compress it.
        if (PMIX_STRING_SIZE_CHECK(kv->value)) {
            char *str = kv->value->data.string;
            uint8_t *cdata = NULL;
            size_t cdata_size = 0;
            if (pmix_compress.compress_string(str, &cdata, &cdata_size)) {
                if (NULL == cdata) {
                    PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                    rc = PMIX_ERR_NOMEM;
                    return rc;
                }
                kv->value->type = PMIX_COMPRESSED_STRING;
                free(kv->value->data.string);
                kv->value->data.bo.bytes = (char *)cdata;
                kv->value->data.bo.size = cdata_size;
            }
        }
        PMIX_GDS_SHMEM_VOUT(
            "%s: STORE proc data for nspace=%s rank=%u key=%s",
            PMIX_NAME_PRINT(&pmix_globals.myid),
            namespace_id, rank, kv->key
        );
        // Store it in the target hash table.
        rc = pmix_hash_store(target_ht, rank, kv);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        PMIX_RELEASE(kv);
    }
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
