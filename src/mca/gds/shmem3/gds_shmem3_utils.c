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
                // Nothing else has seen it: it is not on the global list
                // and the tracker is not pointing at it yet, so this is
                // the only chance to give it back.
                PMIX_RELEASE(inspace);
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

/* Note what is NOT here: a lock on the sessions list.
 *
 * The jobs list needs one because a reader can walk it from the
 * application's thread (see the note on joblock). This list is walked
 * from the progress thread only, and that is what stands in for the
 * lock.
 *
 * Not because a session-realm get is served somewhere else - it is
 * served right here, out of the session segments, by fetch_sessioninfo()
 * in gds_shmem3_fetch.c. What differs is the THREAD it arrives on. A
 * plain keyed get short-circuits onto the caller's own thread in
 * try_local_fetch() (src/client/pmix_client_get.c); that short circuit
 * is declined for anything carrying a realm - session, node or app -
 * because resolving one is not a plain lookup. get_data() may first have
 * to fetch the session id of some other proc and rebuild the info array
 * around it, which is progress-thread work. Declining the short circuit
 * means the ordinary thread-shifted path runs instead, so the very same
 * fetch reaches here - from the progress thread.
 *
 * Both ways into fetch_sessioninfo() are covered, and neither depends on
 * the two sides classifying a key in the same order (they do not:
 * process_request() tries node, app, session; shmem3_fetch_from_job()
 * tries session, node, app). It does not matter, because ALL THREE realm
 * flags are in try_local_fetch()'s decline list, so a key that lands in
 * any realm declines whichever one it landed in. A PMIX_SESSION_INFO
 * qualifier sets lg->sessioninfo and lg->sessiondirective directly. The
 * whole-job read that also calls fetch_sessioninfo() has a NULL key,
 * which is declined too.
 *
 * Note as well that pmix_gds_shmem3_get_session_tracker() WRITES
 * job->session on its way through here, so an application thread
 * arriving would be a race on the tracker, not only on the list spine.
 *
 * If that gating is ever relaxed to let a session lookup run on the
 * caller's thread, this list needs the same treatment joblock gives the
 * other one.
 */
pmix_gds_shmem3_session_t *
pmix_gds_shmem3_find_session(
    uint32_t sid,
    bool create
) {
    pmix_gds_shmem3_component_t *const comp = &pmix_mca_gds_shmem3_component;
    pmix_gds_shmem3_session_t *si;

    /* UINT32_MAX is how a job spells "no session named", never a session
     * of its own - see the note on pmix_gds_shmem3_session_t.id. */
    if (PMIX_UNLIKELY(UINT32_MAX == sid)) {
        return NULL;
    }

    PMIX_LIST_FOREACH(si, &comp->sessions, pmix_gds_shmem3_session_t) {
        if (si->id == sid) {
            return si;
        }
    }

    if (!create) {
        return NULL;
    }

    si = PMIX_NEW(pmix_gds_shmem3_session_t);
    if (PMIX_UNLIKELY(NULL == si)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return NULL;
    }
    si->id = sid;
    /* The list takes the reference PMIX_NEW just gave us, and holds it
     * until del_session() - so a session outlives the jobs in it, which
     * is the whole point of a session. Jobs take their own. */
    pmix_list_append(&comp->sessions, &si->super);
    return si;
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

    /* A wildcard request asks for whatever this job already has, which is
     * the private object job_construct() gave it until something names a
     * session for it. */
    if (UINT32_MAX == sid) {
        return job->session;
    }

    /* Already pointing at the session being asked for. */
    if (NULL != job->session && sid == job->session->id) {
        return job->session;
    }

    /* Pointing at a *different* named session. This is an error: a job
     * belongs to one session. */
    if (PMIX_UNLIKELY(NULL != job->session &&
                      UINT32_MAX != job->session->id)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    /* So the job either has nothing or is still holding the unnamed
     * default, and this id decides which object it should point at.
     *
     * The list is searched on the tracker's own id rather than on
     * smdata->id, which is what makes this reachable at all: smdata is
     * inside the segment, so a search that reads it can only find
     * sessions whose segment already exists - and the caller that has to
     * be answered first is the one deciding whether to build that
     * segment. */
    pmix_gds_shmem3_session_t *si = pmix_gds_shmem3_find_session(sid, create);
    if (NULL == si) {
        return NULL;
    }

    PMIX_RETAIN(si);
    if (NULL != job->session) {
        // Give back the unnamed default this replaces.
        PMIX_RELEASE(job->session);
    }
    job->session = si;
    return si;
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
