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

/**
 * Populates the provided with the elements present in the given comma-delimited
 * string. If the list is not empty, it is first cleared and then set.
 */
static pmix_status_t
set_host_aliases_from_cds(
    pmix_list_t *list,
    const char *cds
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *tma = pmix_obj_get_tma(&list->super);
    // If the list isn't empty, release remove and release its items.
    if (!pmix_list_is_empty(list)) {
        pmix_list_item_t *it;
        while (NULL != (it = pmix_list_remove_first(list))) {
            PMIX_RELEASE(it);
        }
    }
    // Now, add each element present in the comma-delimited string list.
    char **tmp = pmix_argv_split(cds, ',');
    if (!tmp) {
        rc = PMIX_ERR_NOMEM;
        goto out;
    }
    for (size_t i = 0; NULL != tmp[i]; i++) {
        pmix_gds_shmem_host_alias_t *alias;
        alias = PMIX_NEW(pmix_gds_shmem_host_alias_t, tma);
        if (!alias) {
            rc = PMIX_ERR_NOMEM;
            break;
        }

        alias->name = pmix_tma_strdup(tma, tmp[i]);
        if (!alias->name) {
            rc = PMIX_ERR_NOMEM;
            break;
        }

        pmix_list_append(list, &alias->super);
    }
out:
    pmix_argv_free(tmp);
    return rc;
}

/**
 * Adds the given host name to the provided list. If the host name already
 * exists, then it is not added.
 */
#if 0
static pmix_status_t
add_unique_hostname(
    pmix_list_t *list,
    const char *hostname
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *tma = pmix_obj_get_tma(&list->super);
    bool add_host_name = true;

    pmix_gds_shmem_host_alias_t *ai;
    PMIX_LIST_FOREACH (ai, list, pmix_gds_shmem_host_alias_t) {
        // The host name is already present in the list.
        if (0 == strcmp(hostname, ai->name)) {
            add_host_name = false;
            break;
        }
    }
    if (add_host_name) {
        pmix_gds_shmem_host_alias_t *alias;
        alias = PMIX_NEW(pmix_gds_shmem_host_alias_t, tma);
        if (!alias) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        alias->name = pmix_tma_strdup(tma, hostname);
        if (!alias->name) {
            rc = PMIX_ERR_NOMEM;
        }
    }
out:
    return rc;
}
#endif

static pmix_status_t
cache_node_info(
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *cache,
    pmix_gds_shmem_nodeinfo_t **nodeinfo
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *tma = pmix_obj_get_tma(&cache->super);
    bool have_node_id_info = false;

    pmix_gds_shmem_nodeinfo_t *inodeinfo = NULL;
    inodeinfo = PMIX_NEW(pmix_gds_shmem_nodeinfo_t, tma);
    if (!inodeinfo) {
        rc = PMIX_ERR_NOMEM;
        goto out;
    }
    // Cache the values while searching for the nodeid and/or hostname.
    for (size_t j = 0; j < ninfo; j++) {
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s for key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), info[j].key
        );
        if (PMIX_CHECK_KEY(&info[j], PMIX_NODEID)) {
            have_node_id_info = true;
            PMIX_VALUE_GET_NUMBER(
                rc, &info[j].value, inodeinfo->nodeid, uint32_t
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_HOSTNAME)) {
            have_node_id_info = true;
            inodeinfo->hostname = pmix_tma_strdup(
                tma, info[j].value.data.string
            );
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_HOSTNAME_ALIASES)) {
            have_node_id_info = true;
            // info[j].value.data.string is a
            // comma-delimited string of hostnames.
            rc = set_host_aliases_from_cds(
                inodeinfo->aliases,
                info[j].value.data.string
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            // Need to cache this value as well.
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, info[j].key);
            kv->value = NULL;
            PMIX_GDS_SHMEM_VALUE_XFER(rc, kv->value, &info[j].value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                break;
            }
            pmix_list_append(cache, &kv->super);
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, info[j].key);
            kv->value = NULL;
            PMIX_GDS_SHMEM_VALUE_XFER(rc, kv->value, &info[j].value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                break;
            }
            pmix_list_append(cache, &kv->super);
        }
    }
    // Make sure that the caller passed us node identifier info.
    if (!have_node_id_info) {
        rc = PMIX_ERR_BAD_PARAM;
    }
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(inodeinfo);
        inodeinfo = NULL;
    }
    *nodeinfo = inodeinfo;
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_node_array(
    pmix_gds_shmem_job_t *job,
    pmix_value_t *val,
    pmix_list_t *target
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_nodeinfo_t *nodeinfo = NULL;
    pmix_tma_t *tma = pmix_gds_shmem_get_job_tma(job);
    // We expect an array of node-level info for a specific
    // node. Make sure we were given the correct type.
    if (PMIX_DATA_ARRAY != val->type) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_info_t *info = (pmix_info_t *)val->data.darray->array;
    const size_t ninfo = val->data.darray->size;
    // Construct node info cache from provided data.
    pmix_list_t *cache = PMIX_NEW(pmix_list_t, tma);
    if (!cache) {
        return PMIX_ERR_NOMEM;
    }

    rc = cache_node_info(info, ninfo, cache, &nodeinfo);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }

    pmix_kval_t *cache_kv;
    cache_kv = (pmix_kval_t *)pmix_list_remove_first(cache);
    while (NULL != cache_kv) {
        pmix_list_append(nodeinfo->info, &cache_kv->super);
        cache_kv = (pmix_kval_t *)pmix_list_remove_first(cache);
    }

    pmix_list_append(target, &nodeinfo->super);
out:
    PMIX_LIST_DESTRUCT(cache);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_app_array(
    pmix_gds_shmem_job_t *job,
    pmix_value_t *val
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_app_t *app = NULL;
    pmix_tma_t *tma = pmix_gds_shmem_get_job_tma(job);
    // Apps must belong to a job.
    if (!job) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    // We expect an array of node-level info for a specific
    // node. Make sure we were given the correct type.
    if (PMIX_DATA_ARRAY != val->type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    // Setup arrays and lists.
    pmix_list_t *app_cache = PMIX_NEW(pmix_list_t, tma);
    if (!app_cache) {
        return PMIX_ERR_NOMEM;
    }
    pmix_list_t *node_cache = PMIX_NEW(pmix_list_t, tma);
    if (!node_cache) {
        return PMIX_ERR_NOMEM;
    }

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
            app = PMIX_NEW(pmix_gds_shmem_app_t, tma);
            app->appnum = appnum;
        }
        else if (PMIX_CHECK_KEY(&info[j], PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_node_array(
                job, &info[j].value, node_cache
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, info[j].key);
            PMIX_GDS_SHMEM_VALUE_XFER(rc, kv->value, &info[j].value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                goto out;
            }
            pmix_list_append(app_cache, &kv->super);
        }
    }

    if (NULL == app) {
        // Per the standard, they don't have to provide us with
        // an appnum so long as only one app is in the job.
        if (0 == pmix_list_get_size(job->smdata->apps)) {
            app = PMIX_NEW(pmix_gds_shmem_app_t, tma);
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
    // Add it to our list of apps.
    pmix_list_append(job->smdata->apps, &app->super);
    // Point the app at its job.
    if (NULL == app->job) {
        // Do NOT retain the tracker. We will not release it in the app
        // destructor. If we retain the tracker, then we won't release it later
        // because the refcount is wrong.
        app->job = job;
    }
    // Transfer the app-level data across.
    pmix_kval_t *akv;
    akv = (pmix_kval_t *)pmix_list_remove_first(app_cache);
    while (NULL != akv) {
        pmix_list_append(app->appinfo, &akv->super);
        akv = (pmix_kval_t *)pmix_list_remove_first(app_cache);
    }
    // Transfer the associated node-level data across.
    pmix_gds_shmem_nodeinfo_t *nd;
    nd = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(node_cache);
    while (NULL != nd) {
        pmix_list_append(app->nodeinfo, &nd->super);
        nd = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(node_cache);
    }
out:
    PMIX_LIST_DESTRUCT(app_cache);
    PMIX_LIST_DESTRUCT(node_cache);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_proc_data(
    pmix_gds_shmem_job_t *job,
    const pmix_kval_t *kval
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
        pmix_kval_t kv = {
            .key = iptr[j].key,
            .value = &iptr[j].value
        };
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s for nspace=%s rank=%u key=%s",
            __func__, PMIX_NAME_PRINT(&pmix_globals.myid),
            job->nspace_id, rank, kv.key
        );
        // Store it in the hash_table.
        rc = pmix_hash2_store(job->smdata->local_hashtab, rank, &kv, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return rc;
}

// TODO(skg) Maybe we can use this to store data for register_job_info().
pmix_status_t
pmix_gds_shmem_store_job_array(
    pmix_info_t *info,
    pmix_gds_shmem_job_t *job,
    uint32_t *flags,
    // TODO(skg) Do we need to populate these?
    char ***procs,
    char ***nodes
) {
    PMIX_HIDE_UNUSED_PARAMS(procs, nodes);
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *tma = pmix_gds_shmem_get_job_tma(job);
    // We expect an array of job-level info.
    if (PMIX_DATA_ARRAY != info->value.type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }

    pmix_list_t *cache = PMIX_NEW(pmix_list_t, tma);
    if (!cache) {
        return PMIX_ERR_NOMEM;
    }

    const size_t size = info->value.data.darray->size;
    pmix_info_t *iptr = (pmix_info_t *)info->value.data.darray->array;
    for (size_t j = 0; j < size; j++) {
        if (PMIX_CHECK_KEY(&iptr[j], PMIX_APP_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_app_array(
                job, &iptr[j].value
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        else if (PMIX_CHECK_KEY(&iptr[j], PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_shmem_store_node_array(
                job, &iptr[j].value, job->smdata->nodeinfo
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, iptr[j].key);
            kv->value = NULL;
            PMIX_GDS_SHMEM_VALUE_XFER(rc, kv->value, &iptr[j].value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_LIST_DESTRUCT(cache);
                return rc;
            }
            pmix_list_append(job->smdata->jobinfo, &kv->super);
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
        }
    }
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
