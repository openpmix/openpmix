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

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_CREATE() for our needs.
#define PMIX_GDS_SHMEM_INFO_CREATE(m, n, tma)                                  \
do {                                                                           \
    pmix_info_t *i;                                                            \
    (m) = (pmix_info_t *)pmix_tma_calloc((tma), (n), sizeof(pmix_info_t));     \
    if (NULL != (m)) {                                                         \
        i = (pmix_info_t *)(m);                                                \
        i[(n) - 1].flags = PMIX_INFO_ARRAY_END;                                \
    }                                                                          \
} while (0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_XFER() for our needs.
#define PMIX_GDS_SHMEM_INFO_XFER(d, s, tma)                                    \
    (void)pmix_gds_shmem_info_xfer(d, s, tma)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_PROC_CREATE() for our needs.
#define PMIX_GDS_SHMEM_PROC_CREATE(m, n, tma)                                  \
do {                                                                           \
    (m) = (pmix_proc_t *)pmix_tma_calloc((tma), (n), sizeof(pmix_proc_t));     \
} while (0)

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
    if (0 == strcmp(h1, h2)) {
        return true;
    }
    return false;
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

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_copy_value() for our needs.
pmix_status_t
pmix_gds_shmem_bfrops_base_copy_value(
    pmix_value_t **dest,
    pmix_value_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    pmix_value_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    *dest = (pmix_value_t *)pmix_tma_malloc(tma, sizeof(pmix_value_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type */
    p->type = src->type;
    /* copy the data */
    return pmix_gds_shmem_value_xfer(p, src, tma);
}

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_XFER() for our needs.
static pmix_status_t
pmix_gds_shmem_info_xfer(
    pmix_info_t *dest,
    const pmix_info_t *src,
    pmix_tma_t *tma
) {
    if (NULL == dest || NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }
    PMIX_LOAD_KEY(dest->key, src->key);
    return pmix_gds_shmem_value_xfer(&dest->value, &src->value, tma);
}

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_copy_darray() for our needs.
pmix_status_t
pmix_gds_shmem_copy_darray(
    pmix_data_array_t **dest,
    pmix_data_array_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_data_array_t *p = NULL;

    PMIX_HIDE_UNUSED_PARAMS(type);

    p = (pmix_data_array_t *)pmix_tma_calloc(tma, 1, sizeof(pmix_data_array_t));
    if (NULL == p) {
        rc = PMIX_ERR_NOMEM;
        goto out;
    }

    p->type = src->type;
    p->size = src->size;

    if (0 == p->size || NULL == src->array) {
        // Done!
        goto out;
    }

    // Process based on type of array element.
    switch (src->type) {
        case PMIX_UINT8:
        case PMIX_INT8:
        case PMIX_BYTE:
            p->array = (char *)pmix_tma_malloc(tma, src->size);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size);
            break;
        case PMIX_UINT16:
        case PMIX_INT16:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(uint16_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint16_t));
            break;
        case PMIX_UINT32:
        case PMIX_INT32:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(uint32_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint32_t));
            break;
        case PMIX_UINT64:
        case PMIX_INT64:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(uint64_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint64_t));
            break;
        case PMIX_BOOL:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(bool));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(bool));
            break;
        case PMIX_SIZE:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(size_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(size_t));
            break;
        case PMIX_PID:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(pid_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pid_t));
            break;
        case PMIX_STRING: {
            char **prarray, **strarray;
            p->array = (char **)pmix_tma_malloc(tma, src->size * sizeof(char *));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            prarray = (char **)p->array;
            strarray = (char **)src->array;
            for (size_t n = 0; n < src->size; n++) {
                if (NULL != strarray[n]) {
                    prarray[n] = pmix_tma_strdup(tma, strarray[n]);
                    if (NULL == prarray[n]) {
                        rc = PMIX_ERR_NOMEM;
                        goto out;
                    }
                }
            }
            break;
        }
        case PMIX_INT:
        case PMIX_UINT:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(int));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(int));
            break;
        case PMIX_FLOAT:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(float));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(float));
            break;
        case PMIX_DOUBLE:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(double));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(double));
            break;
        case PMIX_TIMEVAL:
            p->array = (struct timeval *)pmix_tma_malloc(
                tma, src->size * sizeof(struct timeval)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(struct timeval));
            break;
        case PMIX_TIME:
            p->array = (time_t *)pmix_tma_malloc(tma, src->size * sizeof(time_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(time_t));
            break;
        case PMIX_STATUS:
            p->array = (pmix_status_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_status_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_status_t));
            break;
        case PMIX_PROC:
            PMIX_GDS_SHMEM_PROC_CREATE(p->array, src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_proc_t));
            break;
        case PMIX_PROC_RANK:
            p->array = (pmix_rank_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_rank_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_rank_t));
            break;
        case PMIX_INFO: {
            pmix_info_t *p1, *s1;
            PMIX_GDS_SHMEM_INFO_CREATE(p->array, src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            p1 = (pmix_info_t *)p->array;
            s1 = (pmix_info_t *)src->array;
            for (size_t n = 0; n < src->size; n++) {
                PMIX_GDS_SHMEM_INFO_XFER(&p1[n], &s1[n], tma);
            }
            break;
        }
        case PMIX_BYTE_OBJECT:
        case PMIX_COMPRESSED_STRING: {
            pmix_byte_object_t *pbo, *sbo;
            p->array = (pmix_byte_object_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_byte_object_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            pbo = (pmix_byte_object_t *)p->array;
            sbo = (pmix_byte_object_t *)src->array;
            for (size_t n = 0; n < src->size; n++) {
                if (NULL != sbo[n].bytes && 0 < sbo[n].size) {
                    pbo[n].size = sbo[n].size;
                    pbo[n].bytes = (char *)pmix_tma_malloc(tma, pbo[n].size);
                    memcpy(pbo[n].bytes, sbo[n].bytes, pbo[n].size);
                } else {
                    pbo[n].bytes = NULL;
                    pbo[n].size = 0;
                }
            }
            break;
        }
        case PMIX_PERSIST:
            p->array = (pmix_persistence_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_persistence_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_persistence_t));
            break;
        case PMIX_SCOPE:
            p->array = (pmix_scope_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_scope_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_scope_t));
            break;
        case PMIX_DATA_RANGE:
            p->array = (pmix_data_range_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_data_range_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_data_range_t));
            break;
        case PMIX_COMMAND:
            p->array = (pmix_cmd_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_cmd_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_cmd_t));
            break;
        case PMIX_INFO_DIRECTIVES:
            p->array = (pmix_info_directives_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_info_directives_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_info_directives_t));
            break;
        case PMIX_DATA_ARRAY:
            // We don't support nested arrays.
            rc = PMIX_ERR_NOT_SUPPORTED;
            break;
        case PMIX_VALUE:
        case PMIX_APP:
        case PMIX_PDATA:
        case PMIX_BUFFER:
        case PMIX_KVAL:
        // TODO(skg) How are we going to handle this case? My guess is that
        // since these are pointers then the assumption is that they are only
        // valid within the process that asked to store them.
        case PMIX_POINTER:
        case PMIX_PROC_INFO:
        case PMIX_QUERY:
        case PMIX_ENVAR:
        case PMIX_COORD:
        case PMIX_REGATTR:
        case PMIX_PROC_CPUSET:
        case PMIX_GEOMETRY:
        case PMIX_DEVICE_DIST:
        case PMIX_ENDPOINT:
        case PMIX_PROC_NSPACE:
        case PMIX_PROC_STATS:
        case PMIX_DISK_STATS:
        case PMIX_NET_STATS:
        case PMIX_NODE_STATS:
        default:
            pmix_output(
                0, "%s: UNSUPPORTED TYPE: %s",
                __func__, PMIx_Data_type_string(src->type)
            );
            rc = PMIX_ERR_UNKNOWN_DATA_TYPE;
    }
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        pmix_tma_free(tma, p);
        p = NULL;
    }
    *dest = p;
    return rc;
}


// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_value_xfer() for our needs.
pmix_status_t
pmix_gds_shmem_value_xfer(
    pmix_value_t *p,
    const pmix_value_t *src,
    pmix_tma_t *tma
) {
    p->type = src->type;

    switch (src->type) {
        case PMIX_UNDEF:
            break;
        case PMIX_BOOL:
            p->data.flag = src->data.flag;
            break;
        case PMIX_BYTE:
            p->data.byte = src->data.byte;
            break;
        case PMIX_STRING:
            if (NULL != src->data.string) {
                p->data.string = pmix_tma_strdup(tma, src->data.string);
            } else {
                p->data.string = NULL;
            }
            break;
        case PMIX_SIZE:
            p->data.size = src->data.size;
            break;
        case PMIX_PID:
            p->data.pid = src->data.pid;
            break;
        case PMIX_INT:
            /* to avoid alignment issues */
            memcpy(&p->data.integer, &src->data.integer, sizeof(int));
            break;
        case PMIX_INT8:
            p->data.int8 = src->data.int8;
            break;
        case PMIX_INT16:
            /* to avoid alignment issues */
            memcpy(&p->data.int16, &src->data.int16, 2);
            break;
        case PMIX_INT32:
            /* to avoid alignment issues */
            memcpy(&p->data.int32, &src->data.int32, 4);
            break;
        case PMIX_INT64:
            /* to avoid alignment issues */
            memcpy(&p->data.int64, &src->data.int64, 8);
            break;
        case PMIX_UINT:
            /* to avoid alignment issues */
            memcpy(&p->data.uint, &src->data.uint, sizeof(unsigned int));
            break;
        case PMIX_UINT8:
            p->data.uint8 = src->data.uint8;
            break;
        case PMIX_UINT16:
        case PMIX_STOR_ACCESS_TYPE:
            /* to avoid alignment issues */
            memcpy(&p->data.uint16, &src->data.uint16, 2);
            break;
        case PMIX_UINT32:
            /* to avoid alignment issues */
            memcpy(&p->data.uint32, &src->data.uint32, 4);
            break;
        case PMIX_UINT64:
        case PMIX_STOR_MEDIUM:
        case PMIX_STOR_ACCESS:
        case PMIX_STOR_PERSIST:
           /* to avoid alignment issues */
            memcpy(&p->data.uint64, &src->data.uint64, 8);
            break;
        case PMIX_FLOAT:
            p->data.fval = src->data.fval;
            break;
        case PMIX_DOUBLE:
            p->data.dval = src->data.dval;
            break;
        case PMIX_TIMEVAL:
            memcpy(&p->data.tv, &src->data.tv, sizeof(struct timeval));
            break;
        case PMIX_TIME:
            memcpy(&p->data.time, &src->data.time, sizeof(time_t));
            break;
        case PMIX_STATUS:
            memcpy(&p->data.status, &src->data.status, sizeof(pmix_status_t));
            break;
        case PMIX_PROC_RANK:
            memcpy(&p->data.rank, &src->data.rank, sizeof(pmix_rank_t));
            break;
        case PMIX_PROC:
            PMIX_GDS_SHMEM_PROC_CREATE(p->data.proc, 1, tma);
            if (NULL == p->data.proc) {
                return PMIX_ERR_NOMEM;
            }
            memcpy(p->data.proc, src->data.proc, sizeof(pmix_proc_t));
            break;
        case PMIX_BYTE_OBJECT:
        case PMIX_COMPRESSED_STRING:
        case PMIX_REGEX:
        case PMIX_COMPRESSED_BYTE_OBJECT:
            memset(&p->data.bo, 0, sizeof(pmix_byte_object_t));
            if (NULL != src->data.bo.bytes && 0 < src->data.bo.size) {
                p->data.bo.bytes = pmix_tma_malloc(tma, src->data.bo.size);
                memcpy(p->data.bo.bytes, src->data.bo.bytes, src->data.bo.size);
                p->data.bo.size = src->data.bo.size;
            } else {
                p->data.bo.bytes = NULL;
                p->data.bo.size = 0;
            }
            break;
        case PMIX_PERSIST:
            memcpy(
                &p->data.persist,
                &src->data.persist,
                sizeof(pmix_persistence_t)
            );
            break;
        case PMIX_SCOPE:
            memcpy(&p->data.scope, &src->data.scope, sizeof(pmix_scope_t));
            break;
        case PMIX_DATA_RANGE:
            memcpy(&p->data.range, &src->data.range, sizeof(pmix_data_range_t));
            break;
        case PMIX_PROC_STATE:
            memcpy(&p->data.state, &src->data.state, sizeof(pmix_proc_state_t));
            break;
        case PMIX_DATA_ARRAY:
            return pmix_gds_shmem_copy_darray(
                &p->data.darray,
                src->data.darray,
                PMIX_DATA_ARRAY,
                tma
            );
        case PMIX_POINTER:
            p->data.ptr = src->data.ptr;
            break;
        case PMIX_ALLOC_DIRECTIVE:
            memcpy(
                &p->data.adir,
                &src->data.adir,
                sizeof(pmix_alloc_directive_t)
            );
            break;
        case PMIX_ENVAR:
            // NOTE(skg) This one is okay because all it does is set members.
            PMIX_ENVAR_CONSTRUCT(&p->data.envar);
            if (NULL != src->data.envar.envar) {
                p->data.envar.envar = pmix_tma_strdup(tma, src->data.envar.envar);
            }
            if (NULL != src->data.envar.value) {
                p->data.envar.value = pmix_tma_strdup(tma, src->data.envar.value);
            }
            p->data.envar.separator = src->data.envar.separator;
            break;
        case PMIX_LINK_STATE:
            memcpy(
                &p->data.linkstate,
                &src->data.linkstate,
                sizeof(pmix_link_state_t)
            );
            break;
        case PMIX_JOB_STATE:
            memcpy(
                &p->data.jstate,
                &src->data.jstate,
                sizeof(pmix_job_state_t)
            );
            break;
        case PMIX_LOCTYPE:
            memcpy(
                &p->data.locality,
                &src->data.locality,
                sizeof(pmix_locality_t)
            );
            break;
        case PMIX_DEVTYPE:
            memcpy(
                &p->data.devtype,
                &src->data.devtype,
                sizeof(pmix_device_type_t)
            );
            break;
        // TODO(skg) Implement currently unsupported types.
        case PMIX_PROC_NSPACE:
        case PMIX_PROC_INFO:
        case PMIX_COORD:
        case PMIX_TOPO:
        case PMIX_PROC_CPUSET:
        case PMIX_GEOMETRY:
        case PMIX_DEVICE_DIST:
        case PMIX_ENDPOINT:
        case PMIX_REGATTR:
        case PMIX_DATA_BUFFER:
        case PMIX_PROC_STATS:
        case PMIX_DISK_STATS:
        case PMIX_NET_STATS:
        case PMIX_NODE_STATS:
        default:
            pmix_output(
                0, "%s: UNSUPPORTED TYPE: %s",
                __func__, PMIx_Data_type_string(src->type)
            );
            return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
