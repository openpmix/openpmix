/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2024 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem3_fetch.h"
#include "gds_shmem3_utils.h"

#include "src/util/pmix_hash.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/ptl_types.h"

// TODO(skg) Avoid copies where appropriate.

static pmix_gds_shmem3_nodeinfo_t *
get_nodeinfo_by_nodename(
    pmix_list_t *nodes,
    const char *hostname
) {
    bool aliases_exist = false;

    if (PMIX_UNLIKELY(NULL == hostname)) {
        return NULL;
    }
    // First, just check all the node names as this is the most likely match.
    pmix_gds_shmem3_nodeinfo_t *ni;
    PMIX_LIST_FOREACH (ni, nodes, pmix_gds_shmem3_nodeinfo_t) {
        if (pmix_gds_shmem3_hostnames_eq(ni->hostname, hostname)) {
            return ni;
        }
        if (!pmix_list_is_empty(ni->aliases)) {
            aliases_exist = true;
        }
    }
    // We didn't find it by name and name aliases do not exists.
    if (!aliases_exist) {
        return NULL;
    }
    // If a match wasn't found, then we have to try the aliases.
    PMIX_LIST_FOREACH (ni, nodes, pmix_gds_shmem3_nodeinfo_t) {
        pmix_gds_shmem3_host_alias_t *nai = NULL;
        PMIX_LIST_FOREACH (nai, ni->aliases, pmix_gds_shmem3_host_alias_t) {
            if (pmix_gds_shmem3_hostnames_eq(nai->name, hostname)) {
                return ni;
            }
        }
    }
    // No match found.
    return NULL;
}

/**
 * Fetches all node info from a given nodeinfo.
 *
 * Takes ownership of key in every case: on success it becomes the
 * returned kval's key, and on failure it is released along with the
 * partially-built kval. Callers must not free it themselves - doing so
 * used to double-free it on the error paths.
 */
static pmix_status_t
fetch_all_node_info(
    char *key,
    pmix_gds_shmem3_nodeinfo_t *nodeinfo,
    pmix_list_t *kvs
) {
    size_t i = 0;

    pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
    if (PMIX_UNLIKELY(NULL == kv)) {
        free(key);
        return PMIX_ERR_NOMEM;
    }
    kv->key = key;
    kv->value = (pmix_value_t *)pmix_calloc(1, sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }

    size_t nds = pmix_list_get_size(nodeinfo->info);
    if (NULL != nodeinfo->hostname) {
        ++nds;
    }
    if (UINT32_MAX != nodeinfo->nodeid) {
        ++nds;
    }
    // Create the data array.
    pmix_data_array_t *darray;
    PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
    if (NULL == darray) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    pmix_info_t *dainfo = (pmix_info_t *)darray->array;
    if (NULL != nodeinfo->hostname) {
        PMIX_INFO_LOAD(
            &dainfo[i++], PMIX_HOSTNAME, nodeinfo->hostname, PMIX_STRING
        );
    }
    if (UINT32_MAX != nodeinfo->nodeid) {
        PMIX_INFO_LOAD(
            &dainfo[i++], PMIX_NODEID, &nodeinfo->nodeid, PMIX_UINT32
        );
    }
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, nodeinfo->info, pmix_kval_t) {
        PMIX_GDS_SHMEM3_VVOUT(
            "%s:%s adding key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kvi->key
        );
        PMIX_LOAD_KEY(dainfo[i].key, kvi->key);
        pmix_status_t rc = PMIx_Value_xfer(&dainfo[i].value, kvi->value);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_ARRAY_FREE(darray);
            PMIX_RELEASE(kv);
            return rc;
        }
        i++;
    }
    kv->value->data.darray = darray;
    kv->value->type = PMIX_DATA_ARRAY;
    pmix_list_append(kvs, &kv->super);
    return PMIX_SUCCESS;
}

/**
 * Fetches all node info from a given list of node infos.
 */
static pmix_status_t
fetch_all_node_info_from_list(
    pmix_peer_t *peer,
    pmix_list_t *nodeinfos,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem3_nodeinfo_t *ni;
    PMIX_LIST_FOREACH (ni, nodeinfos, pmix_gds_shmem3_nodeinfo_t) {
        char *key = NULL;
        // If the proc's version is earlier than v3.1, then the info must be
        // provided as a data_array with a key of the node's name as earlier
        // versions don't understand node_info arrays.
        if (PMIX_PEER_IS_EARLIER(peer, 3, 1, 0)) {
            if (NULL == ni->hostname) {
                // Skip this one.
                continue;
            }
            key = strdup(ni->hostname);
        }
        else {
            // Everyone else uses a node_info array.
            key = strdup(PMIX_NODE_INFO_ARRAY);
        }
        // fetch_all_node_info() owns key from here on, pass or fail.
        rc = fetch_all_node_info(key, ni, kvs);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            break;
        }
    }
    return rc;
}

static pmix_status_t
fetch_nodeinfo(
    pmix_peer_t *peer,
    const char *key,
    pmix_list_t *nodeinfos,
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem3_nodeinfo_t *nodeinfo = NULL;
    uint32_t nid = UINT32_MAX;
    char *hostname = NULL;
    bool found = false;

    PMIX_GDS_SHMEM3_VOUT(
        "%s:%s key=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        (NULL == key) ? "NULL" : key
    );
    // Scan for the nodeID or hostname to identify
    // which node they are asking about.
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID)) {
            rc = PMIx_Value_get_number(&info[n].value, &nid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            found = true;
            break;
        }
        else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
            // This qualifier is whatever the application passed to
            // PMIx_Get; a non-string here would be handed to strcmp.
            if (PMIX_STRING != info[n].value.type) {
                return PMIX_ERR_TYPE_MISMATCH;
            }
            hostname = info[n].value.data.string;
            found = true;
            break;
        }
    }
    if (!found) {
        // If the key is NULL, then they want all the info from all nodes.
        if (NULL == key) {
            return fetch_all_node_info_from_list(peer, nodeinfos, kvs);
        }
        // Else assume they want it from this node.
        hostname = pmix_globals.hostname;
    }
    // Scan the list of nodes to find the matching entry.
    if (UINT32_MAX != nid) {
        pmix_gds_shmem3_nodeinfo_t *ndi;
        PMIX_LIST_FOREACH (ndi, nodeinfos, pmix_gds_shmem3_nodeinfo_t) {
            if (UINT32_MAX != ndi->nodeid &&
                nid == ndi->nodeid) {
                nodeinfo = ndi;
                break;
            }
        }
    }
    else if (NULL != hostname) {
        nodeinfo = get_nodeinfo_by_nodename(nodeinfos, hostname);
    }

    if (NULL == nodeinfo) {
        // the node is not present in the job, which means
        // there were no procs on that node - or the node
        // is unknown, which would indicate an error in
        // specifying it
        return PMIX_ERR_NOT_FOUND;
    }
    // If they want it all, give it to them.
    if (NULL == key) {
        // The key used for the node info.
        char *nikey = NULL;
        // If the proc's version is earlier than v3.1, then the info must be
        // provided as a data_array with a key of the node's name as earlier
        // versions don't understand node_info arrays.
        if (PMIX_PEER_IS_EARLIER(peer, 3, 1, 0)) {
            if (NULL == nodeinfo->hostname) {
                nikey = strdup(pmix_globals.hostname);
            }
            else {
                nikey = strdup(nodeinfo->hostname);
            }
        }
        else {
            // Everyone else uses a node_info array.
            nikey = strdup(PMIX_NODE_INFO_ARRAY);
        }
        // fetch_all_node_info() owns nikey from here on, pass or fail.
        return fetch_all_node_info(nikey, nodeinfo, kvs);
    }
    // If we are here, then they want a specific key/value pair. So, scan the
    // info list of this node to find the key they want.
    rc = PMIX_ERR_DATA_VALUE_NOT_FOUND;
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, nodeinfo->info, pmix_kval_t) {
        if (!PMIX_CHECK_KEY(kvi, key)) {
            continue;
        }
        PMIX_GDS_SHMEM3_VOUT(
            "%s:%s: adding key=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), kvi->key
        );
        // Since they only asked for one key, return just that value.
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(kvi->key);
        kv->value = NULL;
        PMIX_VALUE_XFER(rc, kv->value, kvi->value);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kv);
            return rc;
        }
        pmix_list_append(kvs, &kv->super);
        break;
    }
    return rc;
}

static pmix_status_t
fetch_all_app_info(
    pmix_list_t *apps,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem3_app_t *appi;
    PMIX_LIST_FOREACH (appi, apps, pmix_gds_shmem3_app_t) {
        pmix_data_array_t *darray = NULL;
        size_t i = 0;

        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_APP_INFO_ARRAY);
        kv->value = (pmix_value_t *)pmix_calloc(1, sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        const size_t nds = pmix_list_get_size(appi->appinfo) + 1;
        PMIX_DATA_ARRAY_CREATE(darray, nds, PMIX_INFO);
        if (NULL == darray) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }

        pmix_info_t *info = (pmix_info_t *)darray->array;
        // Put in the appnum.
        PMIX_INFO_LOAD(&info[i++], PMIX_APPNUM, &appi->appnum, PMIX_UINT32);
        // Transfer the app infos.
        pmix_kval_t *kvi;
        PMIX_LIST_FOREACH (kvi, appi->appinfo, pmix_kval_t) {
            PMIX_LOAD_KEY(info[i].key, kvi->key);
            rc = PMIx_Value_xfer(&info[i].value, kvi->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_ARRAY_FREE(darray);
                PMIX_RELEASE(kv);
                return rc;
            }
            i++;
        }
        kv->value->data.darray = darray;
        kv->value->type = PMIX_DATA_ARRAY;
        pmix_list_append(kvs, &kv->super);
    }
    return rc;
}

static pmix_status_t
fetch_appinfo(
    pmix_peer_t *peer,
    const char *key,
    pmix_list_t *target,
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem3_app_t *app = NULL;
    uint32_t appnum = 0;
    bool found = false;

    PMIX_GDS_SHMEM3_VOUT(
        "%s FETCHING APP INFO WITH NAPPS=%zd",
        PMIX_NAME_PRINT(&pmix_globals.myid),
        pmix_list_get_size(target)
    );
    // Scan for the appnum to identify which app they are asking about.
    for (size_t n = 0; n < ninfo; n++) {
        if (!PMIX_CHECK_KEY(&info[n], PMIX_APPNUM)) {
            continue;
        }
        rc = PMIx_Value_get_number(&info[n].value, &appnum, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        found = true;
        break;
    }
    if (!found) {
        /* if the key is NULL, then they want all the info from
         * all apps */
        if (NULL == key) {
            rc = fetch_all_app_info(target, kvs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            return rc;
        }
        // Else assume they are asking for our app.
        appnum = pmix_globals.appnum;
    }
    // Scan the list of apps to find the matching entry.
    pmix_gds_shmem3_app_t *appi;
    PMIX_LIST_FOREACH (appi, target, pmix_gds_shmem3_app_t) {
        if (appnum == appi->appnum) {
            app = appi;
            break;
        }
    }
    if (NULL == app) {
        return PMIX_ERR_NOT_FOUND;
    }
    // See if they wanted to know something about
    // a node that is associated with this app.
    rc = fetch_nodeinfo(
        peer, key, app->nodeinfo, info, ninfo, kvs
    );
    if (PMIX_ERR_DATA_VALUE_NOT_FOUND != rc &&
        PMIX_ERR_NOT_FOUND != rc) {
        return rc;
    }
    // Scan the info list of this app to generate the results.
    rc = PMIX_ERR_NOT_FOUND;
    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, app->appinfo, pmix_kval_t) {
        if (NULL == key || PMIX_CHECK_KEY(kvi, key)) {
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(kvi->key);
            kv->value = NULL;
            PMIX_VALUE_XFER(rc, kv->value, kvi->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                return rc;
            }
            pmix_list_append(kvs, &kv->super);
            rc = PMIX_SUCCESS;
            if (NULL != key) {
                break;
            }
        }
    }
    return rc;
}

static pmix_status_t
xfer_sessioninfo(
    pmix_peer_t *peer,
    pmix_gds_shmem3_session_t *sesh,
    const char *key,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem3_seg_t *const head =
        pmix_gds_shmem3_chain_head(&sesh->segments);
    pmix_gds_shmem3_seg_t *seg;

    /* A session segment was delivered and could not be mapped - decline
     * rather than answer with the values it was published to replace.
     * Same reasoning as job_fetch(); see the note on chain_incomplete. */
    if (sesh->chain_incomplete) {
        return PMIX_ERR_NOT_FOUND;
    }

    if (NULL == head) {
        /* nothing published for this session yet */
        return PMIX_ERR_NOT_FOUND;
    }
    const uint32_t sid = sesh->id;

    /* A keyed request is answered by the newest segment carrying the
     * key: an update publishes only what changed, so everything it did
     * not restate is still answered by the segments behind it. Stopping
     * at the first hit is what makes the newer value win. */
    if (NULL != key) {
        for (seg = head; NULL != seg; seg = seg->prior) {
            pmix_gds_shmem3_shared_session_data_t *const sd = seg->smdata;
            pmix_kval_t *kvi;
            if (NULL == sd) {
                continue;
            }
            PMIX_LIST_FOREACH (kvi, sd->sessioninfo, pmix_kval_t) {
                if (!PMIX_CHECK_KEY(kvi, key)) {
                    continue;
                }
                pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
                kv->key = strdup(kvi->key);
                PMIX_VALUE_XFER(rc, kv->value, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_RELEASE(kv);
                    return rc;
                }
                pmix_list_append(kvs, &kv->super);
                return PMIX_SUCCESS;
            }
        }
        return PMIX_ERR_NOT_FOUND;
    }

    /* The whole-session form. Collect newest-first and keep the first
     * value seen for each key, which is the newest one: a key an update
     * restated must not also be reported with the value it replaced. */
    pmix_list_t merged;
    PMIX_CONSTRUCT(&merged, pmix_list_t);
    for (seg = head; NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_session_data_t *const sd = seg->smdata;
        pmix_kval_t *kvi;
        if (NULL == sd) {
            continue;
        }
        PMIX_LIST_FOREACH (kvi, sd->sessioninfo, pmix_kval_t) {
            pmix_kval_t *seen;
            bool shadowed = false;
            PMIX_LIST_FOREACH (seen, &merged, pmix_kval_t) {
                if (PMIX_CHECK_KEY(seen, kvi->key)) {
                    shadowed = true;
                    break;
                }
            }
            if (shadowed) {
                continue;
            }
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(kvi->key);
            PMIX_VALUE_XFER(rc, kv->value, kvi->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_LIST_DESTRUCT(&merged);
                return rc;
            }
            pmix_list_append(&merged, &kv->super);
        }
    }
    pmix_list_t *const sessionlist = &merged;

    {
        if (PMIX_PEER_IS_EARLIER(peer, 4, 2, 0)) {
            // We can only transfer the data as independent values.
            pmix_kval_t *kvi;
            PMIX_LIST_FOREACH(kvi, sessionlist, pmix_kval_t) {
                pmix_kval_t *kv = PMIX_NEW(pmix_kval_t);
                kv->key = strdup(kvi->key);
                PMIX_VALUE_XFER(rc, kv->value, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_RELEASE(kv);
                    PMIX_LIST_DESTRUCT(&merged);
                    return rc;
                }
                pmix_list_append(kvs, &kv->super);
            }
        }
        else {
            // Return it as an info array.
            pmix_kval_t *kv;
            PMIX_KVAL_NEW(kv, PMIX_SESSION_INFO_ARRAY);
            kv->value->type = PMIX_DATA_ARRAY;
            const size_t n = pmix_list_get_size(sessionlist) + 1;
            PMIX_DATA_ARRAY_CREATE(kv->value->data.darray, n, PMIX_INFO);
            pmix_info_t *info = (pmix_info_t *)kv->value->data.darray->array;
            // First element is the session id.
            PMIX_INFO_LOAD(&info[0], PMIX_SESSION_ID, &sid, PMIX_UINT32);
            // Populate the rest of the array.
            size_t i = 1;
            pmix_kval_t *kvi;
            PMIX_LIST_FOREACH(kvi, sessionlist, pmix_kval_t) {
                PMIX_LOAD_KEY(info[i].key, kvi->key);
                rc = PMIx_Value_xfer(&info[i].value, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_RELEASE(kv);
                    PMIX_LIST_DESTRUCT(&merged);
                    return rc;
                }
                i++;
            }
            pmix_list_append(kvs, &kv->super);
        }
        PMIX_LIST_DESTRUCT(&merged);
        return PMIX_SUCCESS;
    }
    PMIX_LIST_DESTRUCT(&merged);
    return PMIX_SUCCESS;
}

static pmix_status_t
fetch_sessioninfo(
    pmix_peer_t *peer,
    const char *key,
    pmix_gds_shmem3_job_t *job,
    pmix_info_t *info,
    size_t ninfo,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM3_VOUT("%s: FETCHING SESSION INFO", __func__);

    // Scan for the session ID to identify which session they are asking about.
    uint32_t sid = UINT32_MAX;
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_ID)) {
            rc = PMIx_Value_get_number(&info[n].value, &sid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            break;
        }
    }

    /* A read changes nothing. A session the get names only qualifies it:
     * it does not say the job is in that session, so it is looked up on
     * its own, and the job keeps the session it has -
     * pmix_gds_shmem3_get_session_tracker() would point the job at it.
     * With none named, the job's own session answers */
    pmix_gds_shmem3_session_t *sesh;
    if (NULL != job &&
        (UINT32_MAX == sid || (NULL != job->session && sid == job->session->id))) {
        sesh = job->session;
    } else {
        sesh = pmix_gds_shmem3_find_session(sid, false);
    }
    if (PMIX_UNLIKELY(NULL == sesh)) {
        return PMIX_ERR_NOT_FOUND;
    }

    return xfer_sessioninfo(peer, sesh, key, kvs);
}

/* Which chain a tombstone is being compared against - each carries its
 * own generation numbers. See pmix_gds_shmem3_tombstone_t. */
typedef enum {
    TS_JOB,
    TS_MODEX,
    TS_LOCAL
} ts_chain_t;

/* Has this job been told to stop answering for this key?
 *
 * "generation" is that of the segment the answer would come from. Job
 * data is written once and never re-published, so a tombstone against it
 * always applies. Modex and local data can legitimately come back, so a
 * tombstone only shadows the generations of that chain recorded before
 * it: a modex generation up to the one current when it was made, and a
 * local generation below the count held then (pmix_gds_shmem3_local_count).
 *
 * The list is empty for every job nobody has deleted from, which is the
 * ordinary case, so this costs a NULL test on the usual path. */
static bool tombstoned(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    const char *key,
    uint32_t generation,
    ts_chain_t chain
) {
    pmix_gds_shmem3_tombstone_t *t;

    if (NULL == key) {
        return false;
    }
    /* Entering the chain is one acquire-load; the ->prior pointers are
     * immutable, so the rest of the walk needs nothing. */
    for (t = atomic_load_explicit(&job->tombstones, memory_order_acquire);
         NULL != t; t = t->prior) {
        if (t->rank != rank || NULL == t->key || 0 != strcmp(t->key, key)) {
            continue;
        }
        switch (chain) {
            case TS_JOB:
                return true;
            case TS_MODEX:
                if (generation <= t->generation) {
                    return true;
                }
                break;
            case TS_LOCAL:
                if (generation < t->local_generation) {
                    return true;
                }
                break;
        }
    }
    return false;
}

/* Has this job withdrawn any key of this rank - of any rank, for
 * PMIX_RANK_UNDEF? Same lock-free walk as tombstoned(). */
static bool has_tombstone(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank
) {
    pmix_gds_shmem3_tombstone_t *t;

    for (t = atomic_load_explicit(&job->tombstones, memory_order_acquire);
         NULL != t; t = t->prior) {
        if (PMIX_RANK_UNDEF == rank || t->rank == rank) {
            return true;
        }
    }
    return false;
}

/* The key a tombstone is matched against. A qualified value is stored
 * and returned under PMIX_QUALIFIED_VALUE with its real key inside, and
 * a deletion names the real key - the same terms gds/hash removes one
 * on. */
static const char *tombstone_key(
    const pmix_kval_t *kv
) {
    pmix_info_t *info;
    size_t ninfo;

    if (NULL == kv || NULL == kv->key) {
        return NULL;
    }
    if (!PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
        return kv->key;
    }
    if (!pmix_gds_base_info_array(kv->value, &info, &ninfo) || 0 == ninfo) {
        return NULL;
    }
    return info[0].key;
}

/* Drop from a fetch result anything this job has stopped answering for.
 * The NULL-key form of a fetch asks for everything a process published,
 * so the filtering has to happen on the way out rather than on the way
 * in. */
static void drop_tombstoned(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    pmix_list_t *kvs,
    uint32_t generation,
    ts_chain_t chain,
    size_t mark
) {
    pmix_kval_t *kv, *nxt;
    size_t n = 0;

    if (NULL == atomic_load_explicit(&job->tombstones,
                                     memory_order_acquire)) {
        return;
    }
    /* Only what this generation contributed - entries before "mark"
     * came from newer generations and must be judged against theirs,
     * which the walk already did. */
    PMIX_LIST_FOREACH_SAFE (kv, nxt, kvs, pmix_kval_t) {
        if (n++ < mark) {
            continue;
        }
        if (tombstoned(job, rank, tombstone_key(kv), generation, chain)) {
            pmix_list_remove_item(kvs, &kv->super);
            PMIX_RELEASE(kv);
        }
    }
}

/* Drop entries added at or after "mark" whose key an earlier entry
 * already supplied. The list is filled newest generation first, so the
 * earlier copy is the current one and the later copy is stale. */
static void
drop_shadowed(
    pmix_list_t *kvs,
    size_t mark
) {
    pmix_kval_t *later, *nxt, *earlier;
    size_t n, m;

    n = 0;
    PMIX_LIST_FOREACH_SAFE (later, nxt, kvs, pmix_kval_t) {
        if (n++ < mark) {
            continue;
        }
        m = 0;
        PMIX_LIST_FOREACH (earlier, kvs, pmix_kval_t) {
            if (m++ >= mark) {
                break;
            }
            if (NULL != earlier->key && NULL != later->key
                && 0 == strcmp(earlier->key, later->key)) {
                pmix_list_remove_item(kvs, &later->super);
                PMIX_RELEASE(later);
                break;
            }
        }
    }
}

/* Node and app info across the job's chain, newest first.
 *
 * Both live per segment, so an addition published later carries its own
 * lists and a read has to consult all of them. Shadowing is left to the
 * per-entry logic in fetch_nodeinfo()/fetch_appinfo(), which already
 * match on node or app identity. */
static pmix_status_t chain_fetch_nodeinfo(
    pmix_gds_shmem3_job_t *job,
    pmix_peer_t *peer,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_seg_t *seg;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND, r2;

    for (seg = pmix_gds_shmem3_chain_head(&job->job_chain);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_job_data_t *const sd = seg->smdata;
        if (NULL == sd || NULL == sd->nodeinfo) {
            continue;
        }
        r2 = fetch_nodeinfo(peer, key, sd->nodeinfo, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == r2) {
            return r2;
        }
        if (PMIX_SUCCESS == r2) {
            rc = PMIX_SUCCESS;
            if (NULL != key) {
                return rc;
            }
        }
    }
    return rc;
}

static pmix_status_t chain_fetch_appinfo(
    pmix_gds_shmem3_job_t *job,
    pmix_peer_t *peer,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_seg_t *seg;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND, r2;

    for (seg = pmix_gds_shmem3_chain_head(&job->job_chain);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_job_data_t *const sd = seg->smdata;
        if (NULL == sd || NULL == sd->appinfo) {
            continue;
        }
        r2 = fetch_appinfo(peer, key, sd->appinfo, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == r2) {
            return r2;
        }
        if (PMIX_SUCCESS == r2) {
            rc = PMIX_SUCCESS;
            if (NULL != key) {
                return rc;
            }
        }
    }
    return rc;
}

/* Read the job's data, honoring anything this job has been told to stop
 * answering for.
 *
 * A chain, newest first, exactly as the modex is: job data used to be
 * one segment, but a host adding to it after the job was registered
 * publishes a segment carrying what is new, and that has to shadow what
 * it replaces. Each segment's table is paired with THAT segment's key
 * index - an index is minted per segment, so the two cannot be
 * separated.
 *
 * A tombstone against job data always applies, whichever segment
 * answered - hence UINT32_MAX. Job data is not re-published the way a
 * modex generation is: a newer segment adds to it rather than restating
 * an earlier state, so there is no generation for a tombstone to be
 * older than. */
static pmix_status_t job_fetch(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_seg_t *seg;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;

    /* A segment of this chain was delivered and could not be mapped, so
     * what is left cannot be trusted to be current. Decline rather than
     * answer: the walk below stops at the newest segment holding the
     * key, and the segment we are missing is by construction newer than
     * the ones we have - so answering would return the value that
     * segment was published to REPLACE, with PMIX_SUCCESS, and no miss
     * to send anyone to the server. Declining makes it a miss, and the
     * server answers correctly. See the note on chain_incomplete. */
    if (job->chain_incomplete) {
        return PMIX_ERR_NOT_FOUND;
    }

    for (seg = pmix_gds_shmem3_chain_head(&job->job_chain);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_job_data_t *const sd = seg->smdata;
        size_t mark;
        pmix_status_t r2;

        if (NULL == sd || NULL == sd->local_hashtab) {
            continue;
        }
        mark = pmix_list_get_size(kvs);
        r2 = pmix_hash_fetch(sd->local_hashtab, rank, key, qualifiers,
                             nqual, kvs, sd->keyindex);
        if (PMIX_ERR_NOMEM == r2) {
            return r2;
        }
        if (PMIX_SUCCESS != r2) {
            continue;
        }
        drop_tombstoned(job, rank, kvs, UINT32_MAX, TS_JOB, mark);
        if (NULL != key) {
            /* a keyed request stops at the newest segment that has it */
            if (pmix_list_get_size(kvs) > mark) {
                return PMIX_SUCCESS;
            }
            continue;
        }
        /* "everything" - a key a newer segment already supplied must not
         * come back a second time with the value it replaced */
        drop_shadowed(kvs, mark);
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* Remove entries added at or after "mark" whose value is PMIX_UNDEF, and
 * say whether there were any.
 *
 * Such an entry is not data: it is the contributing process saying the
 * key is gone, carried in the modex because a contribution that merely
 * stops naming a key removes nothing at the far end. gds/hash acts on it
 * by taking the key out; this component cannot, because its copy is in a
 * segment that is never rewritten - so the entry stays and every read
 * steps over it. */
static bool strip_undef(
    pmix_list_t *kvs,
    size_t mark
) {
    pmix_kval_t *kv, *nxt;
    size_t n = 0;
    bool found = false;

    PMIX_LIST_FOREACH_SAFE (kv, nxt, kvs, pmix_kval_t) {
        if (n++ < mark) {
            continue;
        }
        if (NULL != kv->value && PMIX_UNDEF == kv->value->type) {
            pmix_list_remove_item(kvs, &kv->super);
            PMIX_RELEASE(kv);
            found = true;
        }
    }
    return found;
}

/* Read the modex, newest generation first.
 *
 * A generation built from a delta contribution holds only what changed,
 * so a key may live only in an older one. For a keyed lookup the newest
 * generation that has it wins and the walk stops there. For a NULL-key
 * lookup - "everything this proc published" - every generation has to be
 * consulted, and a copy of a key a newer one already supplied has to be
 * dropped, or the caller gets that key twice with the stale value
 * second.
 *
 * job->modex_chain is empty unless a delta has been stored, so in the
 * ordinary case this is the single lookup it has always been. */
static pmix_status_t
modex_fetch(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_seg_t *seg;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;
    pmix_status_t r2;
    size_t mark;

    /* Enter the chain once and follow ->prior. The head is the current
     * generation - there is no separate "current" to consult first, and
     * that is what makes this one uniform walk. The head load is the
     * only synchronization it needs: nodes are complete before they are
     * published and never written again. */
    for (seg = pmix_gds_shmem3_chain_head(&job->modex_chain);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_modex_data_t *const smmodex = seg->smdata;
        if (NULL == smmodex) {
            continue;
        }
        mark = pmix_list_get_size(kvs);
        r2 = pmix_hash_fetch(smmodex->hashtab, rank, key, qualifiers,
                             nqual, kvs, smmodex->keyindex);
        if (PMIX_ERR_NOMEM == r2) {
            return r2;
        }
        if (PMIX_SUCCESS == r2) {
            drop_tombstoned(job, rank, kvs, seg->generation, TS_MODEX, mark);
            if (NULL != key) {
                if (strip_undef(kvs, mark)) {
                    return PMIX_ERR_NOT_FOUND;
                }
                if (pmix_list_get_size(kvs) > mark) {
                    return r2;
                }
                continue;
            }
            /* the deletions stay in place for now: drop_shadowed() is
             * what makes them shadow the older values they take back,
             * and they are stripped once the walk is done */
            drop_shadowed(kvs, mark);
            rc = PMIX_SUCCESS;
        }
    }
    if (NULL == key) {
        strip_undef(kvs, 0);
    }
    return rc;
}

/* Read the local chain, newest generation first - the modex walk again,
 * over what the server's own clients put at PMIX_LOCAL and PMIX_GLOBAL
 * scope. Each generation holds only what was committed since the one
 * before, so a NULL-key read consults all of them and drops what a newer
 * one already supplied. See "Local data" in shmem3.rst. */
static pmix_status_t
local_fetch(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_seg_t *seg;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;
    pmix_status_t r2;
    size_t mark;

    for (seg = pmix_gds_shmem3_chain_head(&job->local_chain);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_modex_data_t *const sm = seg->smdata;
        if (NULL == sm) {
            continue;
        }
        mark = pmix_list_get_size(kvs);
        r2 = pmix_hash_fetch(sm->hashtab, rank, key, qualifiers,
                             nqual, kvs, sm->keyindex);
        if (PMIX_ERR_NOMEM == r2) {
            return r2;
        }
        if (PMIX_SUCCESS != r2) {
            continue;
        }
        drop_tombstoned(job, rank, kvs, seg->generation, TS_LOCAL, mark);
        if (NULL != key) {
            if (pmix_list_get_size(kvs) > mark) {
                return PMIX_SUCCESS;
            }
            continue;
        }
        drop_shadowed(kvs, mark);
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* Did the server leave a value out of any segment this job's reads
 * consult? Then a request for "everything" cannot be answered here -
 * see pmix_gds_shmem3_segment_can_hold(). */
static bool
job_incomplete(
    pmix_gds_shmem3_job_t *job
) {
    pmix_gds_shmem3_seg_t *seg;

    for (seg = pmix_gds_shmem3_chain_head(&job->job_chain); NULL != seg; seg = seg->prior) {
        if (NULL != seg->smdata &&
            ((pmix_gds_shmem3_shared_job_data_t *) seg->smdata)->incomplete) {
            return true;
        }
    }
    for (seg = pmix_gds_shmem3_chain_head(&job->modex_chain); NULL != seg; seg = seg->prior) {
        if (NULL != seg->smdata &&
            ((pmix_gds_shmem3_shared_modex_data_t *) seg->smdata)->incomplete) {
            return true;
        }
    }
    for (seg = pmix_gds_shmem3_chain_head(&job->local_chain); NULL != seg; seg = seg->prior) {
        if (NULL != seg->smdata &&
            ((pmix_gds_shmem3_shared_modex_data_t *) seg->smdata)->incomplete) {
            return true;
        }
    }
    if (NULL != job->session) {
        for (seg = pmix_gds_shmem3_chain_head(&job->session->segments); NULL != seg;
             seg = seg->prior) {
            if (NULL != seg->smdata &&
                ((pmix_gds_shmem3_shared_session_data_t *) seg->smdata)->incomplete) {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------
 * The PMIx_Get visibility rule - see the "visibility" argument of the
 * fetch in src/mca/gds/gds.h, and docs/how-things-work/data_scope.rst.
 *
 * This module keeps the two copies of a process's posted data in two
 * chains: the local chain holds what the server's own clients posted at
 * PMIX_LOCAL and PMIX_GLOBAL scope, and the modex chain what every
 * process posted at PMIX_REMOTE and PMIX_GLOBAL. A same-node target is
 * read from the first and any other from the second, never both. A
 * PMIX_DATA_SCOPE that names the location the target is not at reads
 * neither. Everything read here is in segments or on chains, so it is
 * safe on the application's thread.
 * ------------------------------------------------------------------ */

/* Same exemptions as gds/hash: job-level data (a wildcard rank), a
 * reserved key, a scope that is none of the four, and our own data are
 * not anybody else's put data. */
static bool
visibility_applies(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    const char *key
) {
    if (PMIX_SCOPE_UNDEF != scope && PMIX_LOCAL != scope &&
        PMIX_REMOTE != scope && PMIX_GLOBAL != scope) {
        return false;
    }
    if (NULL != key && PMIX_CHECK_RESERVED_KEY(key)) {
        return false;
    }
    if (PMIX_RANK_UNDEF == proc->rank) {
        return true;
    }
    if (!PMIX_RANK_IS_VALID(proc->rank)) {
        return false;
    }
    return !pmix_gds_base_is_me(proc);
}

/* Where a rank of this job is: its PMIX_HOSTNAME in the job's data,
 * which the server took from its own store (stated by the host or
 * derived from the maps), against ours. */
static pmix_gds_base_locality_t
rank_locality(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank
) {
    pmix_gds_base_locality_t loc = PMIX_GDS_LOC_UNKNOWN;
    pmix_list_t kvs;
    pmix_kval_t *kv;

    PMIX_CONSTRUCT(&kvs, pmix_list_t);
    if (PMIX_SUCCESS == job_fetch(job, rank, PMIX_HOSTNAME, NULL, 0, &kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&kvs);
        if (NULL != kv && NULL != kv->value && PMIX_STRING == kv->value->type) {
            loc = pmix_gds_base_host_locality(kv->value->data.string);
        }
    }
    PMIX_LIST_DESTRUCT(&kvs);
    return loc;
}

static void
drain_from(
    pmix_list_t *kvs,
    size_t mark
) {
    pmix_kval_t *kv;

    while (pmix_list_get_size(kvs) > mark &&
           NULL != (kv = (pmix_kval_t *) pmix_list_remove_last(kvs))) {
        PMIX_RELEASE(kv);
    }
}

/* One rank's put data, as a reader there may see it. "loc" has already
 * been resolved, scope included. */
static pmix_status_t
visible_rank_data(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    pmix_gds_base_locality_t loc,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    const size_t mark = pmix_list_get_size(kvs);
    pmix_status_t rc;

    if (PMIX_GDS_LOC_REMOTE != loc) {
        rc = local_fetch(job, rank, key, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
        if (NULL != key && pmix_list_get_size(kvs) > mark) {
            return PMIX_SUCCESS;
        }
    }
    if (PMIX_GDS_LOC_LOCAL != loc) {
        rc = modex_fetch(job, rank, key, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
    }
    return (pmix_list_get_size(kvs) > mark) ? PMIX_SUCCESS : PMIX_ERR_NOT_FOUND;
}

/* A named rank. A miss is PMIX_ERR_NOT_FOUND even when the copy the
 * reader may not see is here: between fences the local chain can be
 * behind the server - a key committed or replaced since the last fence is
 * not in it yet - so a miss here is not proof the reader can never be
 * answered. The server's own store is current, and it says so. A scope
 * that contradicts where the target is IS proof, and is answered at once. */
static pmix_status_t
visible_fetch_rank(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    pmix_scope_t scope,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    const pmix_gds_base_locality_t loc =
        pmix_gds_base_locality_or_scope(rank_locality(job, rank), scope);
    const size_t mark = pmix_list_get_size(kvs);
    pmix_status_t rc;

    if (pmix_gds_base_scope_contradicts(scope, loc)) {
        return PMIX_ERR_EXISTS_OUTSIDE_SCOPE;
    }
    if (PMIX_SCOPE_UNDEF == scope || PMIX_GLOBAL == scope) {
        /* what the host told us about this rank */
        rc = job_fetch(job, rank, key, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
        if (NULL != key && pmix_list_get_size(kvs) > mark) {
            return PMIX_SUCCESS;
        }
    }
    rc = visible_rank_data(job, rank, loc, key, qualifiers, nqual, kvs);
    if (PMIX_ERR_NOMEM == rc) {
        return rc;
    }
    return (pmix_list_get_size(kvs) > mark) ? PMIX_SUCCESS : PMIX_ERR_NOT_FOUND;
}

/* PMIX_RANK_UNDEF: any rank of the namespace, in the order gds/hash
 * answers it - job-level data first, then the lowest-numbered rank whose
 * copy the reader may see. A scope narrows the ranks searched: PMIX_LOCAL
 * to those on our node, PMIX_REMOTE to the others.
 *
 * When nothing visible is found but the key is here in a copy the
 * reader's own scope excluded - a rank on another node for PMIX_LOCAL, one
 * on ours for PMIX_REMOTE - the answer is PMIX_ERR_EXISTS_OUTSIDE_SCOPE,
 * as gds/hash gives it, so nobody waits for it. A copy hidden by where its
 * poster is (a same-node rank's PMIX_REMOTE data) does not count: as for
 * a named rank, the local chain may be behind the server. */
static pmix_status_t
visible_fetch_any(
    pmix_gds_shmem3_job_t *job,
    pmix_scope_t scope,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    const pmix_rank_t nranks = job->nspace->nprocs;
    const size_t mark = pmix_list_get_size(kvs);
    pmix_status_t rc;
    bool excluded = false;

    if (PMIX_SCOPE_UNDEF == scope || PMIX_GLOBAL == scope) {
        if (NULL != key) {
            for (pmix_rank_t rnk = 0; rnk < nranks; rnk++) {
                rc = job_fetch(job, rnk, key, qualifiers, nqual, kvs);
                if (PMIX_ERR_NOMEM == rc || PMIX_SUCCESS == rc) {
                    return rc;
                }
            }
            rc = job_fetch(job, PMIX_RANK_WILDCARD, key, qualifiers, nqual, kvs);
            if (PMIX_ERR_NOMEM == rc || PMIX_SUCCESS == rc) {
                return rc;
            }
        } else {
            for (pmix_rank_t rnk = 0; rnk < nranks; rnk++) {
                rc = job_fetch(job, rnk, NULL, NULL, 0, kvs);
                if (PMIX_ERR_NOMEM == rc) {
                    return rc;
                }
            }
            rc = job_fetch(job, PMIX_RANK_WILDCARD, NULL, NULL, 0, kvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
        }
    }

    if (NULL != key) {
        /* A rank's place is looked up only when the modex has the key for
         * it: whatever the local chain holds was posted by one of our own
         * clients, so a hit there is a same-node rank by construction, and
         * most ranks have the key in neither. */
        for (pmix_rank_t rnk = 0; rnk < nranks; rnk++) {
            const size_t rmark = pmix_list_get_size(kvs);

            rc = local_fetch(job, rnk, key, qualifiers, nqual, kvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            if (pmix_list_get_size(kvs) > rmark) {
                if (PMIX_REMOTE != scope) {
                    return PMIX_SUCCESS;
                }
                /* a same-node rank, which this scope excluded */
                excluded = true;
                drain_from(kvs, rmark);
            }
            rc = modex_fetch(job, rnk, key, qualifiers, nqual, kvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            if (pmix_list_get_size(kvs) == rmark) {
                continue;
            }
            const pmix_gds_base_locality_t where =
                pmix_gds_base_locality_or_scope(rank_locality(job, rnk), scope);
            /* A rank the reader's scope excludes - one on another node for
             * PMIX_LOCAL, one on ours for PMIX_REMOTE - is not searched at
             * all, and that is final: no commit changes a contradiction. */
            if (pmix_gds_base_scope_contradicts(scope, where)) {
                excluded = true;
                drain_from(kvs, rmark);
                continue;
            }
            /* the modex copy is what a reader on another node sees */
            if (PMIX_GDS_LOC_LOCAL != where) {
                return PMIX_SUCCESS;
            }
            /* Not visible: a same-node rank's PMIX_REMOTE copy under no
             * scope. That says nothing on its own: the rank may have
             * committed the key at PMIX_LOCAL since the last fence, and
             * only the server has that yet. */
            drain_from(kvs, rmark);
        }
        return excluded ? PMIX_ERR_EXISTS_OUTSIDE_SCOPE : PMIX_ERR_NOT_FOUND;
    }

    /* "everything from every rank": each rank gives what we may see of it */
    for (pmix_rank_t rnk = 0; rnk < nranks; rnk++) {
        const pmix_gds_base_locality_t loc =
            pmix_gds_base_locality_or_scope(rank_locality(job, rnk), scope);

        if (pmix_gds_base_scope_contradicts(scope, loc)) {
            continue;
        }
        rc = visible_rank_data(job, rnk, loc, NULL, NULL, 0, kvs);
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
    }
    if (pmix_list_get_size(kvs) > mark) {
        return PMIX_SUCCESS;
    }
    return excluded ? PMIX_ERR_EXISTS_OUTSIDE_SCOPE : PMIX_ERR_NOT_FOUND;
}

/* Which store a non-PMIx_Get fetch reads. */
typedef enum {
    STORE_JOB,
    STORE_LOCAL,
    STORE_MODEX
} store_t;

static pmix_status_t
store_fetch(
    pmix_gds_shmem3_job_t *job,
    store_t store,
    pmix_rank_t rank,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    switch (store) {
        case STORE_JOB:
            return job_fetch(job, rank, key, qualifiers, nqual, kvs);
        case STORE_LOCAL:
            return local_fetch(job, rank, key, qualifiers, nqual, kvs);
        case STORE_MODEX:
            return modex_fetch(job, rank, key, qualifiers, nqual, kvs);
    }
    return PMIX_ERR_NOT_FOUND;
}

/* One store, for a rank or - PMIX_RANK_UNDEF - for any rank, job-level
 * data included on the job store's pass only (see the note on the
 * PMIX_RANK_UNDEF sweep in docs/how-things-work/shmem3.rst, step 8f). */
static pmix_status_t
store_fetch_rank(
    pmix_gds_shmem3_job_t *job,
    store_t store,
    pmix_rank_t rank,
    const char *key,
    pmix_info_t *qualifiers,
    size_t nqual,
    pmix_list_t *kvs
) {
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;

    if (PMIX_RANK_UNDEF != rank) {
        return store_fetch(job, store, rank, key, qualifiers, nqual, kvs);
    }
    /* read once - see the note on the whole-job case above */
    const pmix_rank_t nranks = job->nspace->nprocs;
    for (pmix_rank_t rnk = 0; rnk < nranks; rnk++) {
        rc = store_fetch(job, store, rnk, key, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
        if (PMIX_SUCCESS == rc && NULL != key) {
            return rc;
        }
    }
    if (STORE_JOB == store) {
        rc = job_fetch(job, PMIX_RANK_WILDCARD, key, qualifiers, nqual, kvs);
    }
    return rc;
}

static pmix_status_t
shmem3_fetch_from_job(
    pmix_gds_shmem3_job_t *job,
    struct pmix_peer_t *pr,
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_realm_t realm,
    bool visibility,
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    pmix_peer_t *peer = (pmix_peer_t*)pr;
    pmix_status_t rc = PMIX_SUCCESS;
    /* Which realm answers this was decided before we were called - see
     * pmix_gds_base_request_realm(). This module used to derive it again
     * from the key and the three realm qualifiers, which is how it came
     * to disagree with the client about PMIX_JOB_INFO: the client
     * cleared the realm a key had selected, this end re-selected it, and
     * a request the client had judged safe to answer on the application
     * thread reached the session tracker there. */
    const bool sessioninfo = (PMIX_REALM_SESSION == realm);
    const bool nodeinfo = (PMIX_REALM_NODE == realm);
    const bool appinfo = (PMIX_REALM_APP == realm);

    PMIX_HIDE_UNUSED_PARAMS(copy);

    PMIX_GDS_SHMEM3_VOUT(
        "%s:%s key=%s for proc=%s on scope=%s on behalf of %s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), !key ? "NULL" : key,
        PMIX_NAME_PRINT(proc), PMIx_Scope_string(scope),
        PMIX_PEER_PRINT(peer)
    );

    /* A tracker can exist before any segment does - server_add_nspace()
     * creates one at PMIx_server_register_nspace time, well before the
     * first client connects and drives register_job_info() - so a fetch
     * arriving in that window has nothing to read. Ask the chain: the
     * build slot is empty both before the first segment is built AND
     * after it is published. */
    pmix_gds_shmem3_seg_t *const jobhead =
        pmix_gds_shmem3_chain_head(&job->job_chain);
    if (PMIX_UNLIKELY(NULL == jobhead)) {
        return PMIX_ERR_NOT_FOUND;
    }

    // If the rank is wildcard and key is NULL, then the caller is asking for a
    // complete copy of the job-level info for this nspace, so retrieve it.
    if (NULL == key && PMIX_RANK_WILDCARD == proc->rank) {
        // Fetch all values from the hash table tied to rank=wildcard.
        rc = job_fetch(job, PMIX_RANK_WILDCARD, NULL, NULL, 0, kvs);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            return rc;
        }
        // The fetch above is the whole of the job-level data: everything
        // the host gave us for this job is in the local table under
        // PMIX_RANK_WILDCARD, whatever shape it arrived in.
        // Collect all the relevant session-level info.
        rc = fetch_sessioninfo(peer, NULL, job, qualifiers, nqual, kvs);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            return rc;
        }
        // Collect the relevant node-level info.
        rc = chain_fetch_nodeinfo(
            job, peer, NULL, qualifiers, nqual, kvs
        );
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            return rc;
        }
        // Collect the relevant app-level info.
        rc = chain_fetch_appinfo(
            job, peer, NULL, qualifiers, nqual, kvs
        );
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            return rc;
        }
        // Finally, we need the job-level info for each rank in the job.
        /* Read the bound ONCE. It lives on the shared namespace object
         * and gds/hash assigns it from PMIX_JOB_SIZE on the progress
         * thread, while this walk can be running on the application's -
         * so re-reading it per iteration lets the range move underneath
         * the loop. One read gives the loop a bound that does not change
         * while it runs; which of the two values it gets is the ordinary
         * "answered just before the update" question every reader has. */
        const pmix_rank_t nranks = job->nspace->nprocs;
        for (pmix_rank_t rank = 0; rank < nranks; rank++) {
            pmix_list_t rkvs;
            PMIX_CONSTRUCT(&rkvs, pmix_list_t);
            rc = job_fetch(job, rank, NULL, NULL, 0, &rkvs);
            if (PMIX_UNLIKELY(PMIX_ERR_NOMEM == rc)) {
                PMIX_LIST_DESTRUCT(&rkvs);
                return rc;
            }

            const size_t ninfo = pmix_list_get_size(&rkvs);
            if (0 == ninfo) {
                PMIX_DESTRUCT(&rkvs);
                continue;
            }
            // Setup to return the result.
            // TODO(skg) Maybe place to help with zero-copy?
            pmix_kval_t *kv;
            PMIX_KVAL_NEW(kv, PMIX_PROC_INFO_ARRAY);
            kv->value->type = PMIX_DATA_ARRAY;
            const size_t niptr = ninfo + 1; // Need space for the rank.
            PMIX_DATA_ARRAY_CREATE(kv->value->data.darray, niptr, PMIX_INFO);
            pmix_info_t *iptr = (pmix_info_t *)kv->value->data.darray->array;
            // Start with the rank.
            PMIX_INFO_LOAD(&iptr[0], PMIX_RANK, &rank, PMIX_PROC_RANK);
            // Now transfer rest of data across.
            size_t i = 1;
            pmix_kval_t *kvi;
            PMIX_LIST_FOREACH(kvi, &rkvs, pmix_kval_t) {
                PMIX_LOAD_KEY(iptr[i].key, kvi->key);
                PMIx_Value_xfer(&iptr[i].value, kvi->value);
                i++;
            }
            // Add to the results.
            pmix_list_append(kvs, &kv->super);
            // Release the search result.
            PMIX_LIST_DESTRUCT(&rkvs);
        }
        return PMIX_SUCCESS;
    }

    if (sessioninfo) {
        return fetch_sessioninfo(peer, key, job, qualifiers, nqual, kvs);
    }

    if (!PMIX_RANK_IS_VALID(proc->rank)) {
        if (nodeinfo) {
            rc = chain_fetch_nodeinfo(
                job, peer, key, qualifiers, nqual, kvs
            );
            if (PMIX_SUCCESS != rc &&
                PMIX_ERR_NOT_FOUND != rc &&
                (PMIX_RANK_WILDCARD == proc->rank ||
                 PMIX_RANK_UNDEF == proc->rank)) {
                // Let hash deal with this one.
                rc = PMIX_ERR_NOT_FOUND;
            }
            return rc;
        }
        else if (appinfo) {
            rc = chain_fetch_appinfo(
                job, peer, key, qualifiers, nqual, kvs
            );
            if (PMIX_SUCCESS != rc && PMIX_RANK_WILDCARD == proc->rank) {
                // Let hash deal with this one.
                rc = PMIX_ERR_NOT_FOUND;
            }
            return rc;
        }
    }

    /* A request for "everything" from data the server left a value out
     * of cannot be answered here - see pmix_gds_shmem3_segment_can_hold().
     * The server has all of it. Both this and the tombstone test below
     * decline on a client only: the server reads its segments to merge
     * them UNDER its own store (_satisfy_request in pmix_server_get.c),
     * which holds every value left out of a segment and is current, so
     * declining there left its answer with the host's data and nothing
     * the process posted. */
    const bool server = PMIX_PEER_IS_SERVER(pmix_globals.mypeer);

    if (NULL == key && !server && job_incomplete(job)) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* Our own put data is in our own store, which is current: a shared
     * chain carries only what we had committed by the last fence. So of
     * our own rank, only what the host said about it is read here. */
    if (PMIX_RANK_IS_VALID(proc->rank) && pmix_gds_base_is_me(proc)) {
        return job_fetch(job, proc->rank, key, qualifiers, nqual, kvs);
    }

    /* Nor can an aggregate over a rank whose key has been withdrawn. A
     * tombstone hides a deleted key, but also one replaced since the last
     * fence (see
     * server_local_committed()) - whose new value only the server has yet.
     * A keyed get of it misses and asks the server; an aggregate would
     * simply come back without it. So an aggregate over a rank with a
     * tombstone - over any rank, for PMIX_RANK_UNDEF - goes to the server,
     * whose store is current. Deletions and overwrites are rare enough
     * that this costs little. */
    if (NULL == key && !server && has_tombstone(job, proc->rank)) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* A PMIx_Get of another process's data - see visible_fetch_rank(). */
    if (visibility && visibility_applies(proc, scope, key)) {
        if (PMIX_RANK_UNDEF == proc->rank) {
            return visible_fetch_any(job, scope, key, qualifiers, nqual, kvs);
        }
        return visible_fetch_rank(job, proc->rank, scope, key, qualifiers, nqual, kvs);
    }

    /* Everything else reads the stores as the scope names them: the job's
     * data, the local chain (PMIX_LOCAL and PMIX_GLOBAL puts by this
     * node's processes), the modex (PMIX_REMOTE and PMIX_GLOBAL puts by
     * all). A wildcard rank or PMIX_INTERNAL is job data only. */
    store_t stores[3];
    size_t nstores = 0;
    if (PMIX_INTERNAL == scope || PMIX_RANK_WILDCARD == proc->rank) {
        stores[nstores++] = STORE_JOB;
    } else if (PMIX_LOCAL == scope) {
        stores[nstores++] = STORE_JOB;
        stores[nstores++] = STORE_LOCAL;
    } else if (PMIX_REMOTE == scope) {
        stores[nstores++] = STORE_MODEX;
    } else if (PMIX_GLOBAL == scope || PMIX_SCOPE_UNDEF == scope) {
        stores[nstores++] = STORE_JOB;
        stores[nstores++] = STORE_LOCAL;
        stores[nstores++] = STORE_MODEX;
    } else {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    for (size_t n = 0; n < nstores; n++) {
        rc = store_fetch_rank(job, stores[n], proc->rank, key, qualifiers, nqual, kvs);
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
        if (PMIX_SUCCESS != rc) {
            continue;
        }
        if (NULL != key && PMIX_CHECK_RESERVED_KEY(key)) {
            // there is no need to check other scopes for reserved keys
            return PMIX_SUCCESS;
        }
        /* PMIX_GLOBAL collects from every store, as it always has; the
         * rest stop at the first that answers */
        if (PMIX_GLOBAL != scope) {
            break;
        }
    }

    if (0 < pmix_list_get_size(kvs)) {
        // since we found something, the fetch is a success, whatever the
        // last store we tried said
        return PMIX_SUCCESS;
    }
    /* Not found where the scope names. If the rank was valid, say whether
     * it is in a different scope, so a server does not wait for data that
     * will never appear in this one. */
    rc = PMIX_ERR_NOT_FOUND;
    if (PMIX_RANK_IS_VALID(proc->rank) &&
        (PMIX_LOCAL == scope || PMIX_REMOTE == scope)) {
        if (PMIX_LOCAL == scope) {
            rc = modex_fetch(job, proc->rank, key, qualifiers, nqual, kvs);
        } else {
            rc = job_fetch(job, proc->rank, key, qualifiers, nqual, kvs);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOMEM != rc) {
                rc = local_fetch(job, proc->rank, key, qualifiers, nqual, kvs);
            }
        }
        if (PMIX_ERR_NOMEM == rc) {
            return rc;
        }
        rc = (0 < pmix_list_get_size(kvs)) ? PMIX_ERR_EXISTS_OUTSIDE_SCOPE : PMIX_ERR_NOT_FOUND;
        drain_from(kvs, 0);
    }
    return rc;
}

/**
 * Retrieve data from this job's shared segments.
 *
 * The reference taken here is what makes the module thread-safe to read
 * from. A job tracker owns the mappings of its segments, so releasing
 * the last one detaches them; holding one for the duration of the read
 * means the progress thread can deregister the nspace underneath us and
 * the memory we are walking still stays put until we are done.
 *
 * Nothing below takes a lock, and nothing below needs one. What is IN a
 * segment is written once, before any client can see it, and never
 * again; no segment is ever unmapped, so a mapping cannot be withdrawn
 * mid-read; and the two things this walks alongside them - the modex
 * generations and the tombstones - are chains whose nodes are complete
 * before they are published, never rewritten, and never removed. So
 * entering either is one acquire-load and the walk that follows needs
 * no synchronization at all.
 *
 * That is the property this component exists for: a library pulling
 * thousands of values through PMIx_Get during its init pays no lock on
 * any of them. If you add state that a read consults, make it immutable
 * or make it a chain - see the note on pmix_gds_shmem3_job_t.
 */
pmix_status_t
pmix_gds_shmem3_fetch(
    struct pmix_peer_t *pr,
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_realm_t realm,
    bool visibility,
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_job_t *job = NULL;
    pmix_status_t rc;

    rc = pmix_gds_shmem3_acquire_job_tracker(proc->nspace, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        return rc;
    }
    rc = shmem3_fetch_from_job(job, pr, proc, scope, copy, key,
                               qualifiers, nqual, realm, visibility, kvs);
    PMIX_RELEASE(job);
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
