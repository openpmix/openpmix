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

#include "gds_shmem_utils.h"

#include "src/mca/pcompress/base/base.h"

pmix_status_t
pmix_gds_shmem_get_job_tracker(
    const pmix_nspace_t nspace,
    bool create,
    pmix_gds_shmem_job_t **job
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_job_t *target_tracker = NULL, *tracker = NULL;
    pmix_gds_shmem_component_t *component = &pmix_mca_gds_shmem_component;
    // Try to find the tracker for this job.
    // TODO(skg) Consider using a hash table here for lookup.
    PMIX_LIST_FOREACH (tracker, &component->jobs, pmix_gds_shmem_job_t) {
        if (0 == strcmp(nspace, tracker->nspace_id)) {
            target_tracker = tracker;
            break;
        }
    }
    // If we didn't find the requested target and we aren't asked
    // to create a new one, then the request cannot be fulfilled.
    if (!target_tracker && !create) {
        rc = PMIX_ERR_NOT_FOUND;
        goto out;
    }
    // Create one if not found and asked to create one.
    if (!target_tracker && create) {
        pmix_namespace_t *ns = NULL, *inspace = NULL;
        target_tracker = PMIX_NEW(pmix_gds_shmem_job_t);
        if (!target_tracker) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        target_tracker->nspace_id = strdup(nspace);
        if (!target_tracker->nspace_id) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        // See if we already have this nspace in global namespaces.
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(ns->nspace, nspace)) {
                inspace = ns;
                break;
            }
        }
        // If not, create one and update global namespace list.
        if (!inspace) {
            inspace = PMIX_NEW(pmix_namespace_t);
            if (!inspace) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            inspace->nspace = strdup(nspace);
            if (!inspace->nspace) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            pmix_list_append(&pmix_globals.nspaces, &inspace->super);
        }
        PMIX_RETAIN(inspace);
        target_tracker->nspace = inspace;
        // Add it to the list of jobs I'm supporting.
        pmix_list_append(&component->jobs, &target_tracker->super);
    }
out:
    if (PMIX_SUCCESS != rc) {
        if (target_tracker) {
            PMIX_RELEASE(target_tracker);
            target_tracker = NULL;
        }
    }
    *job = target_tracker;
    return rc;
}

pmix_gds_shmem_nodeinfo_t *
pmix_gds_shmem_get_nodeinfo_by_nodename(
    pmix_list_t *nodes,
    const char *hostname
) {
    pmix_gds_shmem_nodeinfo_t *ni;
    bool aliases_exist = false;

    if (NULL == hostname) {
        return NULL;
    }
    // First, just check all the node names as this is the most likely match.
    PMIX_LIST_FOREACH (ni, nodes, pmix_gds_shmem_nodeinfo_t) {
        if (0 == strcmp(ni->hostname, hostname)) {
            return ni;
        }
        if (NULL != ni->aliases) {
            aliases_exist = true;
        }
    }
    // We didn't find it by name and name aliases do not exists.
    if (!aliases_exist) {
        return NULL;
    }
    // If a match wasn't found, then we have to try the aliases.
    PMIX_LIST_FOREACH (ni, nodes, pmix_gds_shmem_nodeinfo_t) {
        if (NULL != ni->aliases) {
            for (int i = 0; NULL != ni->aliases[i]; i++) {
                if (0 == strcmp(ni->aliases[i], hostname)) {
                    return ni;
                }
            }
        }
    }
    // No match found.
    return NULL;
}
