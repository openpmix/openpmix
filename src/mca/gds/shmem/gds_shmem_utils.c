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

#include "src/util/pmix_show_help.h"
#include "src/util/pmix_vmem.h"

pmix_status_t
pmix_gds_shmem_get_job_tracker(
    const pmix_nspace_t nspace,
    bool create,
    pmix_gds_shmem_job_t **job
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Try to find the tracker for this job.
    pmix_gds_shmem_job_t *ti = NULL, *target_tracker = NULL;
    pmix_gds_shmem_component_t *const component = &pmix_mca_gds_shmem_component;
    PMIX_LIST_FOREACH (ti, &component->jobs, pmix_gds_shmem_job_t) {
        if (0 == strcmp(nspace, ti->nspace_id)) {
            target_tracker = ti;
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
        target_tracker = PMIX_NEW(pmix_gds_shmem_job_t);
        if (PMIX_UNLIKELY(!target_tracker)) {
            rc = PMIX_ERR_NOMEM;
            goto out;
        }
        target_tracker->nspace_id = strdup(nspace);
        if (PMIX_UNLIKELY(!target_tracker->nspace_id)) {
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
        target_tracker->nspace = inspace;
        // Add it to the list of jobs I'm supporting.
        pmix_list_append(&component->jobs, &target_tracker->super);
    }
out:
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        if (target_tracker) {
            PMIX_RELEASE(target_tracker);
            target_tracker = NULL;
        }
    }
    *job = target_tracker;
    return rc;
}

pmix_gds_shmem_session_t *
pmix_gds_shmem_check_session(
    pmix_gds_shmem_job_t *job,
    uint32_t sid,
    bool create
) {
    // This is an error: we should always be given a job.
    if (!job) {
        return NULL;
    }

    pmix_tma_t *const tma = pmix_gds_shmem_get_job_tma(job);
    pmix_gds_shmem_component_t *const comp = &pmix_mca_gds_shmem_component;

    if (NULL == job->smdata->session) {
        bool found = false;
        // No session has been assigned to this job. See
        // if the given ID has already been registered.
        pmix_gds_shmem_session_t *si;
        PMIX_LIST_FOREACH(si, &comp->sessions, pmix_gds_shmem_session_t) {
            if (si->session == sid) {
                found = true;
                break;
            }
        }
        if (found) {
            // Point the job tracker at this session.
            PMIX_RETAIN(si);
            job->smdata->session = si;
            return si;
        }
        // If it wasn't found, then create it if permitted.
        if (create) {
            si = PMIX_NEW(pmix_gds_shmem_session_t, tma);
            si->session = sid;
            PMIX_RETAIN(si);
            job->smdata->session = si;
            pmix_list_append(&comp->sessions, &si->super);
            return si;
        }
        else {
            return NULL;
        }
    }
    // If the current session object is pointing to the default global session
    // and we were given a specific session ID, then update it.
    if (UINT32_MAX == job->smdata->session->session) {
        if (UINT32_MAX == sid) {
            // If the given SID is also UINT32_MAX, then we just add to it.
            return job->smdata->session;
        }
        // See if the given ID has already been registered.
        bool found = false;
        pmix_gds_shmem_session_t *si;
        PMIX_LIST_FOREACH(si, &comp->sessions, pmix_gds_shmem_session_t) {
            if (si->session == sid) {
                found = true;
                break;
            }
        }
        if (found) {
            // Update the refcount on the current session object.
            PMIX_RELEASE(job->smdata->session);
            // Point the job tracker at the new place.
            PMIX_RETAIN(si);
            job->smdata->session = si;
            return si;
        }
        // If it wasn't found, then create it.
        if (create) {
            si = PMIX_NEW(pmix_gds_shmem_session_t, tma);
            si->session = sid;
            PMIX_RETAIN(si);
            job->smdata->session = si;
            pmix_list_append(&comp->sessions, &si->super);
            return si;
        }
    }
    else if (UINT32_MAX == sid) {
        // It's a wildcard request, so return the job-tracker session.
        return job->smdata->session;
    }
    // The job tracker already was assigned a session ID.
    // Check if the new one matches.
    if (job->smdata->session->session != sid) {
        // This is an error: you cannot assign a given job to multiple sessions.
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }
    // The two must match, so return it.
    return job->smdata->session;
}

bool
pmix_gds_shmem_check_hostname(
    const char *h1,
    const char *h2
) {
    return (0 == strcmp(h1, h2));
}

pmix_status_t
pmix_gds_shmem_get_job_shmem_from_id(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_shmem_t **shmem
) {
    switch (shmem_id) {
        case PMIX_GDS_SHMEM_JOB_SHMEM_JOB:
            *shmem = job->shmem;
            break;
        case PMIX_GDS_SHMEM_JOB_SHMEM_MODEX:
            *shmem = job->modex_shmem;
            break;
        case PMIX_GDS_SHMEM_JOB_SHMEM_INVALID:
        default:
            *shmem = NULL;
            return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

/**
 * Returns page size.
 */
static inline size_t
get_page_size(void)
{
    const long i = sysconf(_SC_PAGE_SIZE);
    if (-1 == i) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return 0;
    }
    return i;
}

/**
 * Returns amount needed to pad provided size to page boundary.
 */
size_t
pmix_gds_shmem_pad_amount_to_page(
    size_t size
) {
    const size_t page_size = get_page_size();
    return ((~size) + page_size + 1) & (page_size - 1);
}

/**
 * Returns the base temp directory.
 */
static const char *
fetch_base_tmpdir(
    pmix_gds_shmem_job_t *job
) {
    pmix_status_t rc = PMIX_SUCCESS;

    static char fetched_path[PMIX_PATH_MAX] = {'\0'};
    // Keys we may fetch, in priority order.
    char *fetch_keys[] = {
        PMIX_NSDIR,
        PMIX_TMPDIR,
        NULL
    };
    // Did we get a usable fetched key/value?
    bool fetched_kv = false;

    for (int i = 0; NULL != fetch_keys[i]; ++i) {
        pmix_cb_t cb;
        PMIX_CONSTRUCT(&cb, pmix_cb_t);

        pmix_proc_t wildcard;
        PMIX_LOAD_PROCID(
            &wildcard,
            job->nspace->nspace,
            PMIX_RANK_WILDCARD
        );

        cb.key = fetch_keys[i];
        cb.proc = &wildcard;
        cb.copy = true;
        cb.scope = PMIX_LOCAL;

        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (rc != PMIX_SUCCESS) {
            PMIX_DESTRUCT(&cb);
            break;
        }
        // We should only have one item here.
        assert(1 == pmix_list_get_size(&cb.kvs));
        // Get a pointer to the only item in the list.
        pmix_kval_t *kv = (pmix_kval_t *)pmix_list_get_first(&cb.kvs);
        // Make sure we are dealing with the right stuff.
        assert(PMIX_CHECK_KEY(kv, fetch_keys[i]));
        assert(kv->value->type == PMIX_STRING);
        // Copy the value over.
        size_t nw = snprintf(
            fetched_path, PMIX_PATH_MAX, "%s",
            kv->value->data.string
        );
        PMIX_DESTRUCT(&cb);
        if (nw >= PMIX_PATH_MAX) {
            // Try another.
            continue;
        }
        else {
            // We got a usable fetched key.
            fetched_kv = true;
            break;
        }
    }
    // Didn't find a specific temp basedir, so just use a general one.
    if (!fetched_kv) {
        static const char *tmpdir = NULL;
        if (NULL == (tmpdir = getenv("TMPDIR"))) {
            tmpdir = "/tmp";
        }
        return tmpdir;
    }
    else {
        return fetched_path;
    }
}

/**
 * Returns a valid path or NULL on error.
 */
static const char *
get_shmem_backing_path(
    pmix_gds_shmem_job_t *job,
    const char *id
) {
    static char path[PMIX_PATH_MAX] = {'\0'};
    const char *basedir = fetch_base_tmpdir(job);
    // Now that we have the base path, append unique name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/%s-gds-%s.%s.%s.%d",
        basedir, PACKAGE_NAME, PMIX_GDS_SHMEM_NAME,
        job->nspace_id, id, getpid()
    );
    if (nw >= PMIX_PATH_MAX) {
        return NULL;
    }
    return path;
}

/**
 * Attaches to the given shared-memory segment.
 */
static pmix_status_t
shmem_attach(
    pmix_shmem_t *shmem,
    uintptr_t req_addr
) {
    pmix_status_t rc = PMIX_SUCCESS;

    uintptr_t mmap_addr = 0;
    rc = pmix_shmem_segment_attach(
        shmem, (void *)req_addr, &mmap_addr
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    // Make sure that we mapped to the requested address.
    // TODO(skg) Why doesn't this display sometimes?
    if (mmap_addr != req_addr) {
        pmix_show_help(
            "help-gds-shmem.txt",
            "shmem-segment-attach:address-mismatch",
            true, (size_t)req_addr, (size_t)mmap_addr
        );
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        goto out;
    }

    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)mmap_addr
    );
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(shmem);
    }
    return rc;
}

/**
 * Create and attach to a shared-memory segment.
 */
pmix_status_t
pmix_gds_shmem_segment_create_and_attach(
    pmix_gds_shmem_job_t *job,
    pmix_shmem_t *shmem,
    const char *segment_id,
    size_t segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &base_addr, segment_size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s:%s found vmhole at address=0x%zx",
        __func__, segment_id, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    const char *segment_path = get_shmem_backing_path(job, segment_id);
    if (!segment_path) {
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s (size=%zd B)",
        __func__, segment_path, segment_size
    );
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(shmem, segment_size, segment_path);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Attach to the shared-memory segment.
    rc = shmem_attach(shmem, (uintptr_t)base_addr);
out:
    if (PMIX_SUCCESS == rc) {
        job->release_shmem = true;
    }
    return rc;
}

static inline pmix_status_t
init_sm_data(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
) {
    switch (shmem_id) {
        case PMIX_GDS_SHMEM_JOB_SHMEM_JOB:
            job->smdata = job->shmem->base_address;
            break;
        case PMIX_GDS_SHMEM_JOB_SHMEM_MODEX:
            job->smmodex = job->modex_shmem->base_address;
            break;
        case PMIX_GDS_SHMEM_JOB_SHMEM_INVALID:
        default:
            return PMIX_ERROR;
    }
    // Note: don't update the TMA to point to its local function pointers
    // because clients should only be reading from the shared-memory segment.
    pmix_gds_shmem_vout_smdata(job);
    return PMIX_SUCCESS;
}

pmix_status_t
pmix_gds_shmem_segment_attach_and_init(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_from_id(
        job, shmem_id, &shmem
    );
    if (rc != PMIX_SUCCESS) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    // Note that shmem->base_address should have already been populated.
    const uintptr_t req_addr = (uintptr_t)shmem->base_address;
    rc = shmem_attach(shmem, req_addr);
    if (rc != PMIX_SUCCESS) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    // Now we can safely initialize our shared data structures.
    rc = init_sm_data(job, shmem_id);
#if 0
    // Protect memory: clients can only read from here.
    mprotect(
        shmem->base_address, shmem->size, PROT_READ
    );
#endif
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
