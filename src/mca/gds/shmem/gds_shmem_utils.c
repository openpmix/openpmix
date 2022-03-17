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

#include "src/include/pmix_config.h"

#include "gds_shmem_utils.h"
#include "gds_shmem.h"

pmix_status_t
pmix_gds_shmem_get_job_tracker(
    const pmix_nspace_t nspace,
    pmix_gds_shmem_job_t **job
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_job_t *target_tracker = NULL, *tracker = NULL;
    pmix_gds_shmem_component_t *component = &pmix_mca_gds_shmem_component;
    // Try to find the tracker for this job.
    // TODO(skg) Consider using a hash table here for lookup.
    PMIX_LIST_FOREACH (tracker, &component->myjobs, pmix_gds_shmem_job_t) {
        if (0 == strcmp(nspace, tracker->ns)) {
            target_tracker = tracker;
            break;
        }
    }
    // Create one if not found.
    if (!target_tracker) {
        pmix_namespace_t *ns = NULL, *nptr = NULL;
        target_tracker = PMIX_NEW(pmix_gds_shmem_job_t);
        if (!target_tracker) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        target_tracker->ns = strdup(nspace);
        if (!target_tracker->ns) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        // See if we already have this nspace in global namespaces.
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(ns->nspace, nspace)) {
                nptr = ns;
                break;
            }
        }
        // If not, create one and update global namespace list.
        if (!nptr) {
            nptr = PMIX_NEW(pmix_namespace_t);
            if (!nptr) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            nptr->nspace = strdup(nspace);
            if (!nptr->nspace) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            pmix_list_append(&pmix_globals.nspaces, &nptr->super);
        }
        PMIX_RETAIN(nptr);
        target_tracker->nptr = nptr;
        // Add it to the list of jobs I'm supporting.
        pmix_list_append(&component->myjobs, &target_tracker->super);
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
