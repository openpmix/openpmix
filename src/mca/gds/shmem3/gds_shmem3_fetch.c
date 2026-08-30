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
    kv->value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
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
        kv->value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
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

    pmix_gds_shmem3_session_t *sesh;
    sesh = pmix_gds_shmem3_get_session_tracker(job, sid, false);
    if (PMIX_UNLIKELY(NULL == sesh)) {
        return PMIX_ERR_NOT_FOUND;
    }

    return xfer_sessioninfo(peer, sesh, key, kvs);
}

// TODO(skg) This needs plenty of work.
/* Has this job been told to stop answering for this key?
 *
 * "generation" is the modex generation the answer would come from, or
 * UINT32_MAX for the job segment - which is written once and never
 * re-published, so a tombstone against it always applies. Modex data can
 * legitimately come back, so a tombstone only shadows generations up to
 * the one at which it was recorded.
 *
 * The list is empty for every job nobody has deleted from, which is the
 * ordinary case, so this costs a NULL test on the usual path. */
static bool tombstoned(
    pmix_gds_shmem3_job_t *job,
    pmix_rank_t rank,
    const char *key,
    uint32_t generation
) {
    pmix_gds_shmem3_tombstone_t *t;

    if (NULL == key) {
        return false;
    }
    /* Entering the chain is one acquire-load; the ->prior pointers are
     * immutable, so the rest of the walk needs nothing. Empty for every
     * job nobody has deleted from, which is the ordinary case. */
    for (t = atomic_load_explicit(&job->tombstones, memory_order_acquire);
         NULL != t; t = t->prior) {
        if (t->rank != rank || NULL == t->key || 0 != strcmp(t->key, key)) {
            continue;
        }
        if (UINT32_MAX == generation || generation <= t->generation) {
            return true;
        }
    }
    return false;
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
        if (tombstoned(job, rank, kv->key, generation)) {
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
        drop_tombstoned(job, rank, kvs, UINT32_MAX, mark);
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
            drop_tombstoned(job, rank, kvs, seg->generation, mark);
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
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    pmix_peer_t *peer = (pmix_peer_t*)pr;
    pmix_status_t rc = PMIX_SUCCESS;
    bool sessioninfo = false;
    bool nodeinfo = false;
    bool appinfo = false;
    bool sidgiven = false;
    bool nigiven = false;
    bool apigiven = false;

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
    const bool have_job = true;

    /* Modex data are stored in PMIX_REMOTE, but there is no single table
     * to name here: the modex is a chain of generations, and modex_fetch()
     * walks it. Everything past "doover" therefore dispatches on the
     * useremote flag, and "ht" is only ever read on the local side. */
    // Every index in these tables was minted by the server against the
    // keyindex living in the same segment, never against this process's
    // global one, so a read has to translate through that same table.
    // See the comment on pmix_gds_shmem3_shared_modex_data_t.keyindex.
    //
    // Every segment carries its own, which is what lets a read of shared
    // data be independent of pmix_globals.keyindex - a structure the
    // progress thread rewrites. The modex side of that lives in
    // modex_fetch(), which has to pair each generation's table with that
    // generation's index; only the job segment's is needed here.
    /* There is modex data to read exactly when a generation has been
     * published. The build slot says nothing about that: a generation
     * under construction is not readable, and a published one has
     * already left the slot. */
    const bool have_modex =
        NULL != pmix_gds_shmem3_chain_head(&job->modex_chain);
    /* Which store the lookup below is against. This cannot be inferred
     * from ht any more: the modex is a chain of generations rather than
     * one table, and its newest may be absent while older ones are not. */
    bool useremote = false;

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
        for (pmix_rank_t rank = 0; rank < job->nspace->nprocs; rank++) {
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

    for (size_t n = 0; n < nqual; n++) {
        if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_SESSION_INFO)) {
            sessioninfo = PMIX_INFO_TRUE(&qualifiers[n]);
            sidgiven = true;
        }
        else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_NODE_INFO)) {
            nodeinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            nigiven = true;
        }
        else if (PMIX_CHECK_KEY(&qualifiers[n], PMIX_APP_INFO)) {
            appinfo = PMIX_INFO_TRUE(&qualifiers[n]);
            apigiven = true;
        }
    }

    // Check for node/app keys in the absence of corresponding qualifier.
    if (NULL != key && !sidgiven && !nigiven && !apigiven) {
        if (pmix_check_session_info(key)) {
            sessioninfo = true;
        }
        else if (pmix_check_node_info(key)) {
            nodeinfo = true;
        }
        else if (pmix_check_app_info(key)) {
            appinfo = true;
        }
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

    /* Which store answers. Neither is named by a table any more - both
     * are chains, reached through job_fetch() and modex_fetch() - so
     * this only has to say which. */
    if (PMIX_INTERNAL == scope ||
        PMIX_LOCAL == scope ||
        PMIX_GLOBAL == scope ||
        PMIX_SCOPE_UNDEF == scope ||
        PMIX_RANK_WILDCARD == proc->rank) {
        useremote = false;
    }
    else if (PMIX_REMOTE == scope) {
        useremote = true;
    }
    else {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

doover:
    // If rank=PMIX_RANK_UNDEF, then we need to search all
    // known ranks for this nspace as any one of them could
    // be the source.
    if (PMIX_RANK_UNDEF == proc->rank && (useremote ? have_modex : have_job)) {
        for (pmix_rank_t rnk = 0; rnk < job->nspace->nprocs; rnk++) {
            rc = useremote
                     ? modex_fetch(job, rnk, key, qualifiers, nqual, kvs)
                     : job_fetch(job, rnk, key, qualifiers, nqual, kvs);
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            if (PMIX_SUCCESS == rc && NULL != key) {
                return rc;
            }
        }
        // Job-level data is filed under no rank at all - it sits in the
        // local table under PMIX_RANK_WILDCARD - so the per-rank sweep
        // above cannot reach it, and PMIX_RANK_UNDEF means "anything in
        // this nspace" rather than "any rank in this nspace". Without
        // this, PMIx_Get(NULL, key, ...) could not read job-level data
        // at all; it worked only because get_data() in
        // src/client/pmix_client_get.c retries at PMIX_RANK_WILDCARD on
        // the client's behalf.
        //
        // Only on the pass that reads that table, though. The doover
        // below returns here for the modex, where job data never is, and
        // repeating the fetch would collect the same value twice - the
        // client's process_values() reads a count above one as "an
        // aggregate of everything this proc put", so the application
        // would get a PMIX_DATA_ARRAY of duplicates where it asked for a
        // scalar. gds/hash had exactly that defect for exactly this
        // reason.
        if (!useremote) {
            if (NULL == key) {
                rc = job_fetch(job, PMIX_RANK_WILDCARD, NULL, NULL, 0, kvs);
            }
            else {
                rc = job_fetch(job, PMIX_RANK_WILDCARD, key, qualifiers, nqual,
                    kvs);
                if (PMIX_SUCCESS == rc) {
                    return rc;
                }
            }
        }
        else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    }
    else {
        if (useremote ? have_modex : have_job) {
            rc = useremote
                     ? modex_fetch(job, proc->rank, key, qualifiers, nqual, kvs)
                     : job_fetch(job, proc->rank, key, qualifiers, nqual,
                                 kvs);
        }
        else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    }
    if (PMIX_SUCCESS == rc) {
        if (NULL != key && PMIX_CHECK_RESERVED_KEY(key)) {
            // there is no need to check other scopes for
            // reserved keys
            return PMIX_SUCCESS;
        }
        if (PMIX_GLOBAL == scope) {
            if (!useremote) {
                // We need to do this again for the remote data.
                useremote = true;
                goto doover;
            }
        }
    }
    else {
        if (PMIX_GLOBAL == scope || PMIX_SCOPE_UNDEF == scope) {
            if (!useremote) {
                // We need to also try the remote data.
                /* Say which store the retry is against, exactly as the
                 * success path above does. Everything past "doover"
                 * dispatches on this flag rather than on ht, because the
                 * modex is a chain of generations rather than one table;
                 * arriving there with it still false ran the JOB fetch
                 * over the MODEX table, paired with the job segment's key
                 * index. The indices are minted per segment, so that
                 * pairing does not resolve the key and the lookup misses
                 * whatever is there.
                 *
                 * It missed on the ordinary path, not a corner: an
                 * unqualified PMIx_Get arrives as PMIX_SCOPE_UNDEF, finds
                 * nothing local, and reaches the modex only through here.
                 * Every remote key therefore went up to the server, and a
                 * request for a rank that had already finished never came
                 * back - a dmdx tracker is created with no timeout - so
                 * the caller blocked forever. That is an 8-rank job over
                 * 8 nodes hanging better than half the time. */
                useremote = true;
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
            pmix_kval_t *kv;
            if (PMIX_LOCAL == scope) {
                if (have_modex) {
                    /* check the remote scope */
                    rc = modex_fetch(job, proc->rank, key, qualifiers, nqual, kvs);
                    if (PMIX_SUCCESS == rc || 0 < pmix_list_get_size(kvs)) {
                        while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(kvs))) {
                            PMIX_RELEASE(kv);
                        }
                        rc = PMIX_ERR_EXISTS_OUTSIDE_SCOPE;
                    } else {
                        rc = PMIX_ERR_NOT_FOUND;
                    }
                } else {
                    rc = PMIX_ERR_NOT_FOUND;
                }
            } else if (PMIX_REMOTE == scope) {
                /* check the local scope */
                rc = job_fetch(job, proc->rank, key, qualifiers, nqual, kvs);
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
    } else {
        // since we found something, the fetch is a success.
        // We need to set the status here because the fetch on the
        // last scope we tried might not have succeeded, but
        // we found things in an earlier scope we tried
        rc = PMIX_SUCCESS;
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
    pmix_list_t *kvs
) {
    pmix_gds_shmem3_job_t *job = NULL;
    pmix_status_t rc;

    rc = pmix_gds_shmem3_acquire_job_tracker(proc->nspace, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        return rc;
    }
    rc = shmem3_fetch_from_job(job, pr, proc, scope, copy, key,
                               qualifiers, nqual, kvs);
    PMIX_RELEASE(job);
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
