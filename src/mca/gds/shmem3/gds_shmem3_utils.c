/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem3_utils.h"

/**
 * Find a job tracker and take a reference to it, for a caller that is
 * not on the progress thread.
 *
 * The lock is held only across the search and the retain. What it
 * protects is the list spine - del_nspace() can splice a tracker out at
 * any moment - and the window between finding an entry and claiming it.
 * After that the reference does the work: the tracker owns the mappings
 * of its shared segments and cannot be destructed while anyone holds
 * one, so the caller can read those segments with no lock at all.
 */
pmix_status_t
pmix_gds_shmem3_acquire_job_tracker(
    const pmix_nspace_t nspace,
    pmix_gds_shmem3_job_t **job
) {
    pmix_status_t rc = PMIX_ERR_INVALID_NAMESPACE;
    pmix_gds_shmem3_job_t *ti = NULL;
    pmix_gds_shmem3_component_t *const component = &pmix_mca_gds_shmem3_component;

    *job = NULL;
    pmix_mutex_lock(&component->joblock);
    PMIX_LIST_FOREACH (ti, &component->jobs, pmix_gds_shmem3_job_t) {
        if (0 == strcmp(nspace, ti->nspace_id)) {
            PMIX_RETAIN(ti);
            *job = ti;
            rc = PMIX_SUCCESS;
            break;
        }
    }
    pmix_mutex_unlock(&component->joblock);
    return rc;
}

pmix_status_t
pmix_gds_shmem3_get_job_tracker(
    const pmix_nspace_t nspace,
    bool create,
    pmix_gds_shmem3_job_t **job
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Try to find the tracker for this job.
    pmix_gds_shmem3_job_t *ti = NULL, *ijob = NULL;
    pmix_gds_shmem3_component_t *const component = &pmix_mca_gds_shmem3_component;
    /* This runs on the progress thread, which is the only writer, so the
     * lock is not here to order it against del_nspace(). It is here to
     * keep the spine still while a reader on another thread may be
     * walking it - see the note on joblock. */
    pmix_mutex_lock(&component->joblock);
    PMIX_LIST_FOREACH (ti, &component->jobs, pmix_gds_shmem3_job_t) {
        if (0 == strcmp(nspace, ti->nspace_id)) {
            ijob = ti;
            break;
        }
    }
    pmix_mutex_unlock(&component->joblock);
    // If we didn't find the requested target and we aren't asked
    // to create a new one, then the request cannot be fulfilled.
    if (!ijob && !create) {
        rc = PMIX_ERR_INVALID_NAMESPACE;
        goto out;
    }
    // Create one if not found and asked to create one.
    if (!ijob && create) {
        ijob = PMIX_NEW(pmix_gds_shmem3_job_t);
        if (PMIX_UNLIKELY(!ijob)) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        ijob->nspace_id = strdup(nspace);
        if (PMIX_UNLIKELY(!ijob->nspace_id)) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        // See if we already have this nspace in global namespaces.
        pmix_namespace_t *nsi = NULL, *inspace = NULL;
        PMIX_LIST_FOREACH (nsi, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(nsi->nspace, nspace)) {
                inspace = nsi;
                break;
            }
        }
        // If not, create one and update global namespace list.
        if (!inspace) {
            inspace = PMIX_NEW(pmix_namespace_t);
            if (PMIX_UNLIKELY(!inspace)) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            inspace->nspace = strdup(nspace);
            if (PMIX_UNLIKELY(!inspace->nspace)) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            pmix_list_append(&pmix_globals.nspaces, &inspace->super);
        }
        PMIX_RETAIN(inspace);
        ijob->nspace = inspace;
        // Add it to the list of jobs I'm supporting.
        pmix_mutex_lock(&component->joblock);
        pmix_list_append(&component->jobs, &ijob->super);
        pmix_mutex_unlock(&component->joblock);
    }
out:
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        if (ijob) {
            PMIX_RELEASE(ijob);
            ijob = NULL;
        }
    }
    *job = ijob;
    return rc;
}

pmix_gds_shmem3_session_t *
pmix_gds_shmem3_get_session_tracker(
    pmix_gds_shmem3_job_t *job,
    uint32_t sid,
    bool create
) {
    // This is an error: we should always be given a job.
    if (PMIX_UNLIKELY(!job)) {
        return NULL;
    }

    pmix_gds_shmem3_component_t *const comp = &pmix_mca_gds_shmem3_component;

    // A session's identity lives in smdata, which sits in the session's
    // shared-memory segment. That segment is constructed by the server in
    // session_smdata_construct() and mapped by a client in
    // init_client_side_sm_data(); until then smdata is NULL. Creating a
    // session here cannot work, because there would be nowhere to record
    // its ID - no caller asks us to, and one that did would have to
    // arrange the segment first.
    if (PMIX_UNLIKELY(create)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return NULL;
    }

    if (NULL == job->session) {
        // No session has been assigned to this job. See
        // if the given ID has already been registered.
        pmix_gds_shmem3_session_t *si;
        PMIX_LIST_FOREACH(si, &comp->sessions, pmix_gds_shmem3_session_t) {
            if (NULL == si->smdata) {
                // Its segment is not in place yet, so it has no ID to match.
                continue;
            }
            if (si->smdata->id == sid) {
                // Found it. Point the job tracker at this session.
                PMIX_RETAIN(si);
                job->session = si;
                return si;
            }
        }
        return NULL;
    }
    if (NULL == job->session->smdata) {
        // The session segment has not been constructed or attached, so we
        // have no session data to answer with.
        return NULL;
    }
    // If the current session object is pointing to the default global session
    // and we were given a specific session ID, then update it.
    if (UINT32_MAX == job->session->smdata->id) {
        if (UINT32_MAX == sid) {
            // If the given SID is also UINT32_MAX, then we just add to it.
            return job->session;
        }
        // See if the given ID has already been registered.
        pmix_gds_shmem3_session_t *si;
        PMIX_LIST_FOREACH(si, &comp->sessions, pmix_gds_shmem3_session_t) {
            if (NULL == si->smdata) {
                continue;
            }
            if (si->smdata->id == sid) {
                // Found it. Update the refcount on the current session object.
                PMIX_RELEASE(job->session);
                // Point the job tracker at the new place.
                PMIX_RETAIN(si);
                job->session = si;
                return si;
            }
        }
        // Not found, and we cannot create one (see above).
        return NULL;
    }
    else if (UINT32_MAX == sid) {
        // It's a wildcard request, so return the job-tracker session.
        return job->session;
    }
    // The job tracker already was assigned a session ID.
    // Check if the new one matches.
    if (PMIX_UNLIKELY(job->session->smdata->id != sid)) {
        // This is an error: you cannot assign a given job to multiple sessions.
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }
    // The two must match, so return it.
    return job->session;
}

bool
pmix_gds_shmem3_hostnames_eq(
    const char *h1,
    const char *h2
) {
    // A node array may identify its node by nodeid alone, so a tracked
    // node can legitimately carry no hostname. Treat a missing name as
    // "does not match" rather than handing NULL to strcmp().
    if (NULL == h1 || NULL == h2) {
        return false;
    }
    return (0 == strcmp(h1, h2));
}

pmix_status_t
pmix_gds_shmem3_get_job_shmem3_by_id(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_shmem_t **shmem3
) {
    switch (shmem3_id) {
        case PMIX_GDS_SHMEM3_JOB_ID:
            *shmem3 = job->shmem3;
            break;
        case PMIX_GDS_SHMEM3_SESSION_ID:
            // job_construct() allocates the session object, so this is
            // normally set - but job_destruct() walks every segment id,
            // including on a job whose construction did not complete.
            if (NULL == job->session) {
                return PMIX_ERR_NOT_FOUND;
            }
            *shmem3 = job->session->shmem3;
            break;
        case PMIX_GDS_SHMEM3_MODEX_ID:
            *shmem3 = job->modex_shmem3;
            break;
        case PMIX_GDS_SHMEM3_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            abort();
            return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

static inline pmix_gds_shmem3_status_t *
get_job_shmem3_status_flagp(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id
) {
    switch (shmem3_id) {
        case PMIX_GDS_SHMEM3_JOB_ID:
            return &job->shmem3_status;
        case PMIX_GDS_SHMEM3_SESSION_ID:
            return &job->session->shmem3_status;
        case PMIX_GDS_SHMEM3_MODEX_ID:
            return &job->modex_shmem3_status;
        case PMIX_GDS_SHMEM3_INVALID_ID:
        default:
            // This is a fatal internal error.
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            abort();
            return NULL;
    }
}

void
pmix_gds_shmem3_set_status(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_gds_shmem3_status_flag_t flag
) {
    *get_job_shmem3_status_flagp(job, shmem3_id) |= flag;
}

void
pmix_gds_shmem3_clear_status(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_gds_shmem3_status_flag_t flag
) {
    *get_job_shmem3_status_flagp(job, shmem3_id) &= ~flag;
}

void
pmix_gds_shmem3_clearall_status(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id
) {
    *get_job_shmem3_status_flagp(job, shmem3_id) = 0;
}

bool
pmix_gds_shmem3_has_status(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_gds_shmem3_status_flag_t flag
) {
    return (*get_job_shmem3_status_flagp(job, shmem3_id) & flag);
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
