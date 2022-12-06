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
#include "gds_shmem_tma.h"
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
    pmix_tma_t *const tma = pmix_obj_get_tma(&list->super);

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

static pmix_status_t
cache_node_info(
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *cache,
    pmix_gds_shmem_nodeinfo_t **nodeinfo
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *const tma = pmix_obj_get_tma(&cache->super);
    bool have_node_id_info = false;

    pmix_gds_shmem_nodeinfo_t *inodeinfo;
    inodeinfo = PMIX_NEW(pmix_gds_shmem_nodeinfo_t, tma);
    if (!inodeinfo) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Cache the values while searching for the nodeid and/or hostname.
    for (size_t j = 0; j < ninfo; j++) {
        PMIX_GDS_SHMEM_VVOUT(
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
            PMIX_GDS_SHMEM_TMA_VALUE_XFER(rc, kv->value, &info[j].value, tma);
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
            PMIX_GDS_SHMEM_TMA_VALUE_XFER(rc, kv->value, &info[j].value, tma);
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

    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(inodeinfo);
        inodeinfo = NULL;
    }
    *nodeinfo = inodeinfo;
    return rc;
}

static pmix_status_t
store_node_array(
    pmix_value_t *val,
    pmix_list_t *target
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // We expect an array of node-level info for a specific
    // node. Make sure we were given the correct type.
    if (PMIX_DATA_ARRAY != val->type) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    // Construct node info cache from provided data.
    pmix_tma_t *const tma = pmix_obj_get_tma(&target->super);
    pmix_list_t *cache = PMIX_NEW(pmix_list_t, tma);
    if (!cache) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_gds_shmem_nodeinfo_t *nodeinfo;
    rc = cache_node_info(
        (pmix_info_t *)val->data.darray->array,
        val->data.darray->size,
        cache,
        &nodeinfo
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }

    pmix_kval_t *cache_kv;
    while ((cache_kv = (pmix_kval_t *)pmix_list_remove_first(cache))) {
        pmix_list_append(nodeinfo->info, &cache_kv->super);
    }

    pmix_list_append(target, &nodeinfo->super);
out:
    PMIX_LIST_DESTRUCT(cache);
    return rc;
}

static pmix_status_t
store_app_array(
    pmix_gds_shmem_job_t *job,
    pmix_value_t *val
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_app_t *app = NULL;

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

    pmix_tma_t *const tma = pmix_gds_shmem_get_job_tma(job);
    // Setup arrays and lists.
    pmix_list_t *app_cache = PMIX_NEW(pmix_list_t, tma);
    if (!app_cache) {
        return PMIX_ERR_NOMEM;
    }

    pmix_list_t *node_cache = PMIX_NEW(pmix_list_t, tma);
    if (!node_cache) {
        PMIX_LIST_DESTRUCT(app_cache);
        return PMIX_ERR_NOMEM;
    }

    const size_t size = val->data.darray->size;
    pmix_info_t *info = (pmix_info_t *)val->data.darray->array;
    for (size_t j = 0; j < size; j++) {
        PMIX_GDS_SHMEM_VVOUT(
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
            rc = store_node_array(&info[j].value, node_cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, info[j].key);
            PMIX_GDS_SHMEM_TMA_VALUE_XFER(rc, kv->value, &info[j].value, tma);
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
    while ((akv = (pmix_kval_t *)pmix_list_remove_first(app_cache))) {
        pmix_list_append(app->appinfo, &akv->super);
    }
    // Transfer the associated node-level data across.
    pmix_gds_shmem_nodeinfo_t *nd;
    while ((nd = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(node_cache))) {
        pmix_list_append(app->nodeinfo, &nd->super);
    }
out:
    PMIX_LIST_DESTRUCT(app_cache);
    PMIX_LIST_DESTRUCT(node_cache);
    return rc;
}

static pmix_status_t
store_proc_data(
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

    pmix_info_t *const info = (pmix_info_t *)kval->value->data.darray->array;
    // First element of the array must be the rank.
    if (!PMIX_CHECK_KEY(&info[0], PMIX_RANK) ||
        info[0].value.type != PMIX_PROC_RANK) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_hash_table2_t *const ht = job->smdata->local_hashtab;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);

    const pmix_rank_t rank = info[0].value.data.rank;
    const size_t size = kval->value->data.darray->size;
    // Cycle through the values for this rank and store them.
    for (size_t j = 1; j < size; j++) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
        kv->key = info[j].key;
        kv->value = &info[j].value;
        PMIX_GDS_SHMEM_VVOUT(
            "%s:%s for nspace=%s rank=%u key=%s",
            __func__, PMIX_NAME_PRINT(&pmix_globals.myid),
            job->nspace_id, rank, kv->key
        );
        // Store it in the hash_table.
        rc = pmix_hash2_store(ht, rank, kv, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return rc;
}

static pmix_status_t
store_session_array(
    pmix_gds_shmem_job_t *job,
    pmix_value_t *val
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // We expect an array of session-level info.
    if (PMIX_DATA_ARRAY != val->type) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_TYPE_MISMATCH;
    }

    // The first value is required to be the session ID.
    pmix_info_t *const info = (pmix_info_t *)val->data.darray->array;
    if (!PMIX_CHECK_KEY(&info[0], PMIX_SESSION_ID)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    uint32_t sid;
    PMIX_VALUE_GET_NUMBER(rc, &info[0].value, sid, uint32_t);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_gds_shmem_session_t *sesh;
    sesh = pmix_gds_shmem_check_session(job, sid, true);
    if (!sesh) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_tma_t *const tma = pmix_gds_shmem_get_job_tma(job);
    pmix_list_t *ncache = PMIX_NEW(pmix_list_t, tma);
    if (!ncache) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    const size_t size = val->data.darray->size;
    for (size_t j = 1; j < size; j++) {
        PMIX_GDS_SHMEM_VOUT(
            "%s:%s key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid),
            info[j].key
        );
        if (PMIX_CHECK_KEY(&info[j], PMIX_NODE_INFO_ARRAY)) {
            rc = store_node_array(&info[j].value, ncache);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto out;
            }
        }
        else {
            pmix_kval_t *kval = PMIX_NEW(pmix_kval_t, tma);
            kval->key = pmix_tma_strdup(tma, info[j].key);
            PMIX_GDS_SHMEM_TMA_VALUE_XFER(rc, kval->value, &info[j].value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kval);
                goto out;
            }
            pmix_list_append(sesh->sessioninfo, &kval->super);
        }
    }

    pmix_gds_shmem_nodeinfo_t *ni;
    while ((ni = (pmix_gds_shmem_nodeinfo_t *)pmix_list_remove_first(ncache))) {
        pmix_list_append(sesh->nodeinfo, &ni->super);
    }
out:
    PMIX_LIST_DESTRUCT(ncache);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_qualified(
    pmix_hash_table2_t *ht,
    pmix_rank_t rank,
    pmix_value_t *value
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);

    // The value contains a pmix_data_array_t whose first position contains the
    // key-value being stored, followed by one or more qualifiers.
    pmix_info_t *const info = (pmix_info_t *)value->data.darray->array;
    const size_t ninfo = value->data.darray->size;

    // This does not need to use the TMA since its contents are later copied in
    // hash_store() using a TMA. This is just temporary storage.
    const size_t nquals = ninfo - 1;
    pmix_info_t *quals;
    PMIX_INFO_CREATE(quals, nquals);
    for (size_t i = 1; i < ninfo; i++) {
        PMIX_INFO_SET_QUALIFIER(&quals[i - 1]);
        PMIX_INFO_XFER(&quals[i - 1], &info[i]);
    }

    // Extract the primary value.
    pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
    kv->key = info[0].key;
    kv->value = &info[0].value;

    // Store the result.
    rc = pmix_hash2_store(ht, rank, kv, quals, nquals);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_INFO_FREE(quals, nquals);
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_local_job_data_in_shmem(
    pmix_gds_shmem_job_t *job,
    pmix_list_t *job_data
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_hash_table2_t *const local_ht = job->smdata->local_hashtab;
    pmix_tma_t *const tma = pmix_obj_get_tma(&local_ht->super);

    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, job_data, pmix_kval_t) {
        PMIX_GDS_SHMEM_VVOUT("%s: key=%s", __func__, kvi->key);
        if (PMIX_DATA_ARRAY == kvi->value->type) {
            // We support the following data array keys.
            if (PMIX_CHECK_KEY(kvi, PMIX_APP_INFO_ARRAY)) {
                rc = store_app_array(job, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else if (PMIX_CHECK_KEY(kvi, PMIX_NODE_INFO_ARRAY)) {
                rc = store_node_array(
                    kvi->value, job->smdata->nodeinfo
                );
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else if (PMIX_CHECK_KEY(kvi, PMIX_PROC_INFO_ARRAY)) {
                rc = store_proc_data(job, kvi);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else if (PMIX_CHECK_KEY(kvi, PMIX_SESSION_INFO_ARRAY)) {
                rc = store_session_array(job, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else {
                PMIX_GDS_SHMEM_VVOUT(
                    "%s: ERROR unsupported array type=%s", __func__, kvi->key
                );
                rc = PMIX_ERR_NOT_SUPPORTED;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, kvi->key);
            PMIX_GDS_SHMEM_TMA_VALUE_XFER(rc, kv->value, kvi->value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_ERROR_LOG(rc);
                break;
            }
            rc = pmix_hash2_store(
                local_ht, PMIX_RANK_WILDCARD, kv, NULL, 0
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
    }
    return rc;
}

pmix_status_t
pmix_gds_shmem_store_modex_in_shmem(
    pmix_gds_base_ctx_t ctx,
    pmix_proc_t *proc,
    pmix_gds_modex_key_fmt_t key_fmt,
    char **kmap,
    pmix_buffer_t *pbkt
) {
    PMIX_HIDE_UNUSED_PARAMS(ctx);
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "[%s:%d] %s for nspace %s",
        pmix_globals.myid.nspace,
        pmix_globals.myid.rank,
        __func__, proc->nspace
    );

    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_hash_table2_t *const ht = job->smdata->smmodex->hashtab;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);

    // This is data returned via the PMIx_Fence call when data collection was
    // requested, so it only contains REMOTE/GLOBAL data. The byte object
    // contains the rank followed by pmix_kval_ts. The list of callbacks
    // contains all local participants.
    pmix_kval_t *kv;
    // Unpack the values until we hit the end of the buffer.
    while (true) {
        kv = PMIX_NEW(pmix_kval_t, tma);

        rc = pmix_gds_base_modex_unpack_kval(key_fmt, pbkt, kmap, kv);
        if (PMIX_SUCCESS != rc) {
            break;
        }

        if (PMIX_RANK_UNDEF == proc->rank) {
            // If the rank is undefined, then we store it on the
            // remote table of rank=0 as we know that rank must
            // always exist.
            if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_shmem_store_qualified(ht, 0, kv->value);
            }
            else {
                rc = pmix_hash2_store(ht, 0, kv, NULL, 0);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        else {
            // Store this in the hash table.
            if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_shmem_store_qualified(ht, proc->rank, kv->value);
            }
            else {
                rc = pmix_hash2_store(ht, proc->rank, kv, NULL, 0);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        PMIX_DESTRUCT(kv);
    }
    PMIX_DESTRUCT(kv);

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }
    else {
        rc = PMIX_SUCCESS;
    }

    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
