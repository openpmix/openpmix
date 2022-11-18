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

    // Try to find the tracker for this job.
    pmix_gds_shmem_component_t *component = &pmix_mca_gds_shmem_component;
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
        // Cache a handle to a full-featured gds module.
        target_tracker->ffgds = pmix_globals.mypeer->nptr->compat.gds;
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

pmix_gds_shmem_session_t *
pmix_gds_shmem_check_session(
    pmix_gds_shmem_job_t *job,
    uint32_t sid
) {
    pmix_gds_shmem_session_t *sptr = NULL;

    /* if this is an invalid session ID, we don't look for it
     * on the list - someone is trying to register a new
     * session for a job prior to actually getting a
     * session ID. We simply add it to the end of the
     * list and return a pointer that they can later
     * use to assign an actual SID */
    if (UINT32_MAX != sid) {
        /* if the tracker is NULL, then they are asking for the
         * session tracker for a specific sid */
        if (NULL == job) {
            PMIX_LIST_FOREACH(sptr, &pmix_mca_gds_shmem_component.sessions, pmix_gds_shmem_session_t) {
                if (sptr->session == sid) {
                    return sptr;
                }
            }
        } else {
            /* if the job tracker already has a session object, then
             * check that the IDs match */
            if (NULL != job->session) {
                sptr = job->session;
                if (sptr->session != sid) {
                    /* this is an error */
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    return NULL;
                }
                return sptr;
            }
        }
        /* get here because the job tracker doesn't have
         * a session tracker, so create the session tracker */
        sptr = PMIX_NEW(pmix_gds_shmem_session_t);
        sptr->session = sid;
        /* add to the global list */
        pmix_list_append(&pmix_mca_gds_shmem_component.sessions, &sptr->super);
        if (NULL != job) {
            /* also add it to the job */
            PMIX_RETAIN(sptr);
            job->session = sptr;
        }
        return sptr;
    }

    if (NULL == job) {
        /* this is an error */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    if (NULL != job->session) {
        return job->session;
    }

    /* create the session tracker */
    sptr = PMIX_NEW(pmix_gds_shmem_session_t);
    /* we don't add it to the global list because it doesn't have
     * a specific ID, but we do add it to the job */
    job->session = sptr;
    return sptr;
}

pmix_gds_shmem_nodeinfo_t *
pmix_gds_shmem_get_nodeinfo_by_nodename(
    pmix_list_t *nodes,
    const char *hostname
) {
    bool aliases_exist = false;

    if (NULL == hostname) {
        return NULL;
    }
    // First, just check all the node names as this is the most likely match.
    pmix_gds_shmem_nodeinfo_t *ni;
    PMIX_LIST_FOREACH (ni, nodes, pmix_gds_shmem_nodeinfo_t) {
        if (0 == strcmp(ni->hostname, hostname)) {
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
    PMIX_LIST_FOREACH (ni, nodes, pmix_gds_shmem_nodeinfo_t) {
        pmix_gds_shmem_host_alias_t *nai = NULL;
        PMIX_LIST_FOREACH (nai, ni->aliases, pmix_gds_shmem_host_alias_t) {
            if (0 == strcmp(nai->name, hostname)) {
                return ni;
            }
        }
    }
    // No match found.
    return NULL;
}

bool
pmix_gds_shmem_check_hostname(
    const char *h1,
    const char *h2
) {
    return (0 == strcmp(h1, h2));
}

/**
 * Error function for catching unhandled data types.
 */
static inline pmix_status_t
unhandled_type(
    pmix_data_type_t val_type
) {
    static const pmix_status_t rc = PMIX_ERR_FATAL;
    PMIX_GDS_SHMEM_VOUT(
        "%s: %s", __func__, PMIx_Data_type_string(val_type)
    );
    PMIX_ERROR_LOG(rc);
    return rc;
}

/**
 * Returns the size required to store the given type.
 */
// TODO(skg) This probably shouldn't live here. Figure out a way to get this in
// an appropriate place.
pmix_status_t
pmix_gds_shmem_get_value_size(
    const pmix_value_t *value,
    size_t *result
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t total = 0;

    const pmix_data_type_t type = value->type;
    // NOTE(skg) This follows the size conventions set in include/pmix_common.h.
    switch (type) {
        case PMIX_ALLOC_DIRECTIVE:
        case PMIX_BYTE:
        case PMIX_DATA_RANGE:
        case PMIX_INT8:
        case PMIX_PERSIST:
        case PMIX_POINTER:
        case PMIX_PROC_STATE:
        case PMIX_SCOPE:
        case PMIX_UINT8:
            total += sizeof(int8_t);
            break;
        case PMIX_BOOL:
            total += sizeof(bool);
            break;
        // TODO(skg) This needs more work. For example, we can potentially miss
        // the storage requirements of scaffolding around things like
        // PMIX_STRINGs, for example. See pmix_gds_shmem_copy_darray().
        case PMIX_DATA_ARRAY: {
            total += sizeof(pmix_data_array_t);
            const size_t n = value->data.darray->size;
            PMIX_GDS_SHMEM_VOUT(
                "%s: %s of type=%s has size=%zd",
                __func__, PMIx_Data_type_string(type),
                PMIx_Data_type_string(value->data.darray->type), n
            );
            // We don't construct or destruct tmp_value because we are only
            // interested in inspecting its values for the purposes of
            // estimating its size using this function.
            pmix_value_t tmp_value = {
                .type = value->data.darray->type
            };
            size_t sizeofn = 0;
            rc = pmix_gds_shmem_get_value_size(&tmp_value, &sizeofn);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            total += n * sizeofn;
            break;
        }
        case PMIX_DOUBLE:
            total += sizeof(double);
            break;
        case PMIX_ENVAR: {
            const pmix_envar_t *val = &value->data.envar;
            total += sizeof(*val);
            total += strlen(val->envar) + 1;
            total += strlen(val->value) + 1;
            break;
        }
        case PMIX_FLOAT:
            total += sizeof(float);
            break;
        case PMIX_INFO:
            total += sizeof(pmix_info_t);
            break;
        case PMIX_INT:
        case PMIX_UINT:
        case PMIX_STATUS:
            total += sizeof(int);
            break;
        case PMIX_INT16:
            total += sizeof(int16_t);
            break;
        case PMIX_INT32:
            total += sizeof(int32_t);
            break;
        case PMIX_INT64:
            total += sizeof(int64_t);
            break;
        case PMIX_PID:
            total += sizeof(pid_t);
            break;
        case PMIX_PROC:
            total += sizeof(pmix_proc_t);
            break;
        case PMIX_PROC_RANK:
            total += sizeof(pmix_rank_t);
            break;
        case PMIX_REGEX:
            total += sizeof(pmix_byte_object_t);
            total += value->data.bo.size;
            break;
        case PMIX_STRING:
            total += strlen(value->data.string) + 1;
            break;
        case PMIX_SIZE:
            total += sizeof(size_t);
            break;
        case PMIX_UINT16:
            total += sizeof(uint16_t);
            break;
        case PMIX_UINT32:
            total += sizeof(uint32_t);
            break;
        case PMIX_UINT64:
            total += sizeof(uint64_t);
            break;
        case PMIX_APP:
        case PMIX_BUFFER:
        case PMIX_BYTE_OBJECT:
        case PMIX_COMMAND:
        case PMIX_COMPRESSED_STRING:
        case PMIX_COORD:
        case PMIX_DATA_BUFFER:
        case PMIX_DATA_TYPE:
        case PMIX_DEVICE_DIST:
        case PMIX_DEVTYPE:
        case PMIX_DISK_STATS:
        case PMIX_ENDPOINT:
        case PMIX_GEOMETRY:
        case PMIX_INFO_DIRECTIVES:
        case PMIX_IOF_CHANNEL:
        case PMIX_JOB_STATE:
        case PMIX_KVAL:
        case PMIX_LINK_STATE:
        case PMIX_NET_STATS:
        case PMIX_NODE_STATS:
        case PMIX_PDATA:
        case PMIX_PROC_CPUSET:
        case PMIX_PROC_INFO:
        case PMIX_PROC_NSPACE:
        case PMIX_PROC_STATS:
        case PMIX_QUERY:
        case PMIX_REGATTR:
        case PMIX_TIME:
        case PMIX_TIMEVAL:
        case PMIX_TOPO:
        case PMIX_VALUE:
        default:
            return unhandled_type(type);
    }
    *result = total;
    return rc;
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
 * Returns amount needed to pad to page boundary.
 */
size_t
pmix_gds_shmem_pad_amount_to_page(
    size_t size
) {
    const size_t page_size = get_page_size();
    return ((~size) + page_size + 1) & (page_size - 1);
}

/**
 * Returns a valid path or NULL on error.
 */
static const char *
get_shmem_backing_path(
    const char *id
) {
    static char path[PMIX_PATH_MAX] = {'\0'};
    const char *basedir = NULL;
    // TODO(skg) Fix
#if 0
    for (size_t i = 0; i < ninfo; ++i) {
        if (PMIX_CHECK_KEY(&info[i], PMIX_NSDIR)) {
            basedir = info[i].value.data.string;
            break;
        }
        if (PMIX_CHECK_KEY(&info[i], PMIX_TMPDIR)) {
            basedir = info[i].value.data.string;
            break;
        }
    }
#endif
    if (!basedir) {
        if (NULL == (basedir = getenv("TMPDIR"))) {
            basedir = "/tmp";
        }
    }
    // Now that we have the base dir, append file name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/gds-%s.%s.%d",
        basedir, PMIX_GDS_SHMEM_NAME, id, getpid()
    );
    if (nw >= PMIX_PATH_MAX) {
        return NULL;
    }
    return path;
}

// TODO(skg) Add pmix_vmem_hole_kind_t parameter. We may not want to use the
// largest available hole all the time.
/**
 * Create and attach to a shared-memory segment.
 */
pmix_status_t
pmix_gds_shmem_segment_create_and_attach(
    pmix_gds_shmem_job_t *job,
    const char *segment_id,
    size_t segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    const char *segment_path = NULL;
    uintptr_t mmap_addr = 0;
    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &base_addr, segment_size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: found vmhole at address=0x%zx",
        __func__, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    segment_path = get_shmem_backing_path(segment_id);
    if (!segment_path) {
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s (size=%zd B)",
        __func__, segment_path, segment_size
    );
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(
        job->shmem, segment_size, segment_path
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Attach to the shared-memory segment with the given address.
    rc = pmix_shmem_segment_attach(
        job->shmem, (void *)base_addr, &mmap_addr
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Make sure that we mapped to the requested address.
    if (mmap_addr != (uintptr_t)job->shmem->base_address) {
        // TODO(skg) Add a nice error message.
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)mmap_addr
    );
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(job->shmem);
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
