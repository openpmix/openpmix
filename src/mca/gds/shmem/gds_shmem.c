/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem.h"
#include "gds_shmem_utils.h"
#include "gds_shmem_store.h"
#include "gds_shmem_fetch.h"
// TODO(skg) This will eventually go away.
#include "pmix_hash2.h"

#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"

#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_shmem.h"

#include "src/mca/pmdl/pmdl.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/pcompress/base/base.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif

// Some notes:
// We cannot use PMIX_CONSTRUCT for data that are stored in shared memory
// because their address is on the stack of the process in which they are
// constructed.

// TODO(skg) Address FT case at some point. Need to have a broader conversion
// about how we go about doing this. Ralph has some ideas.

// TODO(skg) Consider using mprotect.
// TODO(skg) We way need to implement a different hash table.

/**
 * Key names used to find shared-memory segment info.
 */
#define PMIX_GDS_SHMEM_KEY_SEG_PATH "PMIX_GDS_SHMEM_SEG_PATH"
#define PMIX_GDS_SHMEM_KEY_SEG_SIZE "PMIX_GDS_SHMEM_SEG_SIZE"
#define PMIX_GDS_SHMEM_KEY_SEG_ADDR "PMIX_GDS_SHMEM_SEG_ADDR"

/**
 * String to size_t.
 */
static inline pmix_status_t
strtost(
    const char *str,
    int base,
    size_t *maybe_val
) {
    *maybe_val = 0;

    errno = 0;
    char *end = NULL;
    long long val = strtoll(str, &end, base);
    int err = errno;
    // In valid range?
    if (err == ERANGE && val == LLONG_MAX) {
        return PMIX_ERROR;
    }
    if (err == ERANGE && val == LLONG_MIN) {
        return PMIX_ERROR;
    }
    if (*end != '\0') {
        return PMIX_ERROR;
    }
    *maybe_val = (size_t)val;
    return PMIX_SUCCESS;
}

/**
 * 8-byte address alignment.
 */
static inline void *
addr_align_8(
    void *base,
    size_t size
) {
#if 0 // Helpful debug
    PMIX_GDS_SHMEM_VOUT("------------------------ADDRINN=%p,%zd", base, size);
#endif
    void *result = (void *)(((uintptr_t)base + size + 7) & ~(uintptr_t)0x07);
#if 0 // Helpful debug
    // Make sure that it's 8-byte aligned.
    assert ((uintptr_t)result % 8 == 0);
    PMIX_GDS_SHMEM_VOUT("------------------------ADDROUT=%p,%zd", result, size);
#endif
    return result;
}

// TODO(skg) Add memory arena boundary checks.
static inline void *
tma_malloc(
    pmix_tma_t *tma,
    size_t size
) {
    void *current = *(tma->data_ptr);
    memset(current, 0, size);
    *(tma->data_ptr) = addr_align_8(current, size);
    return current;
}

// TODO(skg) Add memory arena boundary checks.
static inline void *
tma_calloc(
    struct pmix_tma *tma,
    size_t nmemb,
    size_t size
) {
    const size_t real_size = nmemb * size;
    void *current = *(tma->data_ptr);
    memset(current, 0, real_size);
    *(tma->data_ptr) = addr_align_8(current, real_size);
    return current;
}

// TODO(skg)
static inline void *
tma_realloc(
    pmix_tma_t *tma,
    void *ptr,
    size_t size
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr, size);
    assert(false);
    return NULL;
}

// TODO(skg) Add memory arena boundary checks.
static inline char *
tma_strdup(
    pmix_tma_t *tma,
    const char *s
) {
    void *current = *(tma->data_ptr);
    const size_t size = strlen(s) + 1;
    *(tma->data_ptr) = addr_align_8(current, size);
    return (char *)memmove(current, s, size);
}

// TODO(skg) Add memory arena boundary checks.
static inline void *
tma_memmove(
    struct pmix_tma *tma,
    const void *src,
    size_t size
) {
    void *current = *(tma->data_ptr);
    *(tma->data_ptr) = addr_align_8(current, size);
    return memmove(current, src, size);
}

// TODO(skg)
static inline void
tma_free(
    struct pmix_tma *tma,
    void *ptr
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr);
}

static void
tma_init_function_pointers(
    pmix_tma_t *tma
) {
    tma->tma_malloc = tma_malloc;
    tma->tma_calloc = tma_calloc;
    tma->tma_realloc = tma_realloc;
    tma->tma_strdup = tma_strdup;
    tma->tma_memmove = tma_memmove;
    tma->tma_free = tma_free;
}

static void
tma_init(
    pmix_tma_t *tma
) {
    tma_init_function_pointers(tma);
    tma->data_ptr = NULL;
}

static void
host_alias_construct(
    pmix_gds_shmem_host_alias_t *a
) {
    a->name = NULL;
}

static void
host_alias_destruct(
    pmix_gds_shmem_host_alias_t *a
) {
    pmix_tma_t *tma = pmix_obj_get_tma(&a->super.super);
    if (a->name) {
        pmix_tma_free(tma, a->name);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_host_alias_t,
    pmix_list_item_t,
    host_alias_construct,
    host_alias_destruct
);

static void
nodeinfo_construct(
    pmix_gds_shmem_nodeinfo_t *n
) {
    pmix_tma_t *tma = pmix_obj_get_tma(&n->super.super);

    n->nodeid = UINT32_MAX;
    n->hostname = NULL;
    n->aliases = PMIX_NEW(pmix_list_t, tma);
    n->info = PMIX_NEW(pmix_list_t, tma);
}

static void
nodeinfo_destruct(
    pmix_gds_shmem_nodeinfo_t *n
) {
    pmix_tma_t *tma = pmix_obj_get_tma(&n->super.super);

    pmix_tma_free(tma, n->hostname);
    if (n->aliases) {
        PMIX_LIST_DESTRUCT(n->aliases);
    }
    if (n->info) {
        PMIX_LIST_DESTRUCT(n->info);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_nodeinfo_t,
    pmix_list_item_t,
    nodeinfo_construct,
    nodeinfo_destruct
);

static void
job_construct(
    pmix_gds_shmem_job_t *job
) {
    job->nspace_id = NULL;
    job->nspace = NULL;
    job->ffgds = NULL;
    job->shmem = PMIX_NEW(pmix_shmem_t);
    job->smdata = NULL;
}

static void
job_destruct(
    pmix_gds_shmem_job_t *job
) {
    if (job->nspace_id) {
        free(job->nspace_id);
    }
    if (job->nspace) {
        PMIX_RELEASE(job->nspace);
    }
    job->ffgds = NULL;
    // This will release the memory for the structures located in the
    // shared-memory segment.
    if (job->shmem) {
        PMIX_RELEASE(job->shmem);
    }
    job->shmem = NULL;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_job_t,
    pmix_list_item_t,
    job_construct,
    job_destruct
);

static void
app_construct(
    pmix_gds_shmem_app_t *a
) {
    pmix_tma_t *tma = pmix_obj_get_tma(&a->super.super);

    a->appnum = 0;
    a->appinfo = PMIX_NEW(pmix_list_t, tma);
    a->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    a->job = NULL;
}

static void
app_destruct(
    pmix_gds_shmem_app_t *a
) {
    if (a->appinfo) {
        PMIX_LIST_DESTRUCT(a->appinfo);
    }
    if (a->nodeinfo) {
        PMIX_LIST_DESTRUCT(a->nodeinfo);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_app_t,
    pmix_list_item_t,
    app_construct,
    app_destruct
);

static pmix_status_t
module_init(
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.jobs, pmix_list_t);
    return PMIX_SUCCESS;
}

static void
module_finalize(void)
{
    PMIX_GDS_SHMEM_VOUT_HERE();
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem_component.jobs);
}

static pmix_status_t
assign_module(
    pmix_info_t *info,
    size_t ninfo,
    int *priority
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    static const int max_priority = 100;
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    // The incoming info always overrides anything in the
    // environment as it is set by the application itself.
    bool specified = false;
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GDS_MODULE)) {
            char **options = NULL;
            specified = true; // They specified who they want.
            options = pmix_argv_split(info[n].value.data.string, ',');
            for (size_t m = 0; NULL != options[m]; m++) {
                if (0 == strcmp(options[m], PMIX_GDS_SHMEM_NAME)) {
                    // They specifically asked for us.
                    *priority = max_priority;
                    break;
                }
            }
            pmix_argv_free(options);
            break;
        }
    }
#if (PMIX_GDS_SHMEM_DISABLE == 1)
    if (true) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
#endif
    // If they don't want us, then disqualify ourselves.
    if (specified && *priority != max_priority) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
server_cache_job_info(
    struct pmix_namespace_t *ns,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(ns, info, ninfo);
    PMIX_GDS_SHMEM_VOUT_HERE();
    // We don't support this operation.
    return PMIX_ERR_NOT_SUPPORTED;
}

/**
 *
 */
static pmix_status_t
prepare_backing_store_for_local_job_data(
    pmix_gds_shmem_job_t *job,
    size_t data_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Initial hash table size.
    const size_t ihtsize = 256;
    // Keep in sync with the number of lists in pmix_gds_shmem_app_t.
    const size_t nlists = 3;
    // Used as a multiplier to size some structures.
    const size_t nranks = (size_t)job->nspace->nprocs;
    // First, see if the given job already has initialized its shared info.
    // If so, we are done.
    if (job->smdata) {
        return PMIX_SUCCESS;
    }
    // Else we have to setup the memory allocator.
    // Calculate a rough estimate on the amount of storage required to store the
    // values associated with the pmix_gds_shmem_shared_data_t. Err on the side
    // of overestimation.
    // Start with the size of the segment header.
    size_t seg_size = sizeof(pmix_gds_shmem_shared_data_t);
    // We need to store some lists in the shared-memory segment, so calculate
    // a rough estimate on the memory required for their storage.
    seg_size += nlists * sizeof(pmix_list_t) * nranks;
    // We need to store a hash table in the shared-memory segment, so calculate
    // a rough estimate on the memory required for its storage.
    seg_size += sizeof(pmix_hash_table2_t);
    seg_size += ihtsize * pmix_hash_table2_t_sizeof_hash_element();
    // Add a little extra to compensate for the value storage requirements. Here
    // we add an additional storage space for each entry.
    seg_size += ihtsize * (sizeof(pmix_kval_t) + sizeof(pmix_value_t));
    // Finally add the data size contribution.
    seg_size += data_size;
    // Add some extra fluff in case we weren't precise enough.
    seg_size *= 1.5;
    // Pad to fill an entire page.
    seg_size += pmix_gds_shmem_pad_amount_to_page(seg_size);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Create and attach to the shared-memory segment associated with this job.
    // This will be the backing store for metadata associated with static,
    // read-only data shared between the server and its clients.
    char segment_name[PMIX_PATH_MAX] = {'\0'};
    size_t nw = snprintf(
        segment_name, PMIX_PATH_MAX, "%s", job->nspace_id
    );
    if (nw >= PMIX_PATH_MAX) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Create and attach to the shared-memory segment.
    rc = pmix_gds_shmem_segment_create_and_attach(
        job, segment_name, seg_size
    );
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its smdata located at the base of the segment.
    void *baseaddr = job->shmem->base_address;
    job->smdata = baseaddr;
    memset(job->smdata, 0, sizeof(*job->smdata));
    // Save the starting address for TMA memory allocations.
    job->smdata->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(&job->smdata->tma);
    job->smdata->tma.data_ptr = &job->smdata->current_addr;
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->smdata->tma.data_ptr) = addr_align_8(baseaddr, sizeof(*job->smdata));
    // We can now safely get the job's TMA.
    pmix_tma_t *tma = pmix_gds_shmem_get_job_tma(job);
    // Now that we know the TMA, allocate its structures.
    job->smdata->jobinfo = PMIX_NEW(pmix_list_t, tma);
    job->smdata->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    job->smdata->apps = PMIX_NEW(pmix_list_t, tma);
    job->smdata->local_hashtab = PMIX_NEW(pmix_hash_table2_t, tma);
    pmix_hash_table2_init(job->smdata->local_hashtab, ihtsize);

    pmix_gds_shmem_vout_smdata(job);

    return rc;
}

static inline pmix_status_t
pack_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for peer (ID=%d) namespace=%s",
        __func__, PMIX_NAME_PRINT(&pmix_globals.myid),
        peer->info->peerid, job->nspace_id
    );
    // Pack the backing file path.
    pmix_kval_t kv;
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = strdup(PMIX_GDS_SHMEM_KEY_SEG_PATH);
    kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
    kv.value->type = PMIX_STRING;
    kv.value->data.string = strdup(job->shmem->backing_path);
    PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
    PMIX_DESTRUCT(&kv);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Pack attach size to shared-memory segment.
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = strdup(PMIX_GDS_SHMEM_KEY_SEG_SIZE);
    kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
    kv.value->type = PMIX_STRING;
    int nw = asprintf(&kv.value->data.string, "%zx", job->shmem->size);
    if (nw == -1) {
        PMIX_DESTRUCT(&kv);
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
    PMIX_DESTRUCT(&kv);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Pack the base address for attaching to shared-memory segment.
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = strdup(PMIX_GDS_SHMEM_KEY_SEG_ADDR);
    kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
    kv.value->type = PMIX_STRING;
    nw = asprintf(
        &kv.value->data.string, "%zx",
        (size_t)job->shmem->base_address
    );
    if (nw == -1) {
        PMIX_DESTRUCT(&kv);
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
    PMIX_DESTRUCT(&kv);
    return rc;
}

/**
 * Sets shared-memory connection information from a pmix_buffer_t by
 * unpacking the blob and saving the values for the appropriate rank.
 */
static pmix_status_t
unpack_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;
    int32_t count = 1;

    pmix_kval_t kval;
    PMIX_CONSTRUCT(&kval, pmix_kval_t);
    PMIX_BFROPS_UNPACK(
        rc, pmix_client_globals.myserver,
        buffer, &kval, &count, PMIX_KVAL
    );
    while (PMIX_SUCCESS == rc) {
        // We only pack string, so make sure this is the right kind of data.
        if (kval.value->type != PMIX_STRING) {
            rc = PMIX_ERR_BAD_PARAM;
            break;
        }
        const char *val = kval.value->data.string;
        if (pmix_gds_shmem_keys_eq(kval.key, PMIX_GDS_SHMEM_KEY_SEG_PATH)) {
            // Set job segment path.
            int nw = snprintf(
                job->shmem->backing_path, PMIX_PATH_MAX, "%s", val
            );
            if (nw >= PMIX_PATH_MAX) {
                rc = PMIX_ERROR;
                break;
            }
        }
        else if (pmix_gds_shmem_keys_eq(kval.key, PMIX_GDS_SHMEM_KEY_SEG_SIZE)) {
            // Set job shared-memory segment size.
            rc = strtost(val, 16, &job->shmem->size);
            if (PMIX_SUCCESS != rc) {
                break;
            }
        }
        else if (pmix_gds_shmem_keys_eq(kval.key, PMIX_GDS_SHMEM_KEY_SEG_ADDR)) {
            size_t base_addr = 0;
            // Convert string base address to something we can use.
            rc = strtost(val, 16, &base_addr);
            if (PMIX_SUCCESS != rc) {
                break;
            }
            // Set job segment base address.
            job->shmem->base_address = (void *)base_addr;
        }
        else {
            rc = PMIX_ERR_BAD_PARAM;
            break;
        }
        count = 1;
        PMIX_DESTRUCT(&kval);
        PMIX_CONSTRUCT(&kval, pmix_kval_t);
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            buffer, &kval, &count, PMIX_KVAL
        );
    }
    // Catch last one.
    PMIX_DESTRUCT(&kval);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
    }
    else {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

static pmix_status_t
register_job_info(
    struct pmix_peer_t *peer_struct,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_peer_t *peer = (pmix_peer_t *)peer_struct;
    pmix_namespace_t *ns = peer->nptr;
    size_t data_size = 0;
    // Setup a job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(ns->nspace, true, &job);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Get the job's TMA.
    pmix_tma_t *tma = pmix_gds_shmem_get_job_tma(job);

    pmix_proc_t wildcard;
    PMIX_LOAD_PROCID(&wildcard, ns->nspace, PMIX_RANK_WILDCARD);
    // Ask for a complete copy of the job-level information.
    pmix_cb_t job_cb;
    PMIX_CONSTRUCT(&job_cb, pmix_cb_t);
    job_cb.key = NULL;
    job_cb.proc = &wildcard;
    job_cb.copy = true;
    job_cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &job_cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&job_cb);
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Pack the data so we can see how large it is. This will help inform how
    // large to make the shared-memory segment associated with these data.
    pmix_buffer_t data;
    PMIX_CONSTRUCT(&data, pmix_buffer_t);

    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &job_cb.kvs, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, peer, &data, kvi, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&job_cb);
            PMIX_DESTRUCT(&data);
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    data_size += data.bytes_allocated;
    // No longer needed since we captured its size.
    PMIX_DESTRUCT(&data);

    rc = prepare_backing_store_for_local_job_data(job, data_size);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_hash_table2_t *ht = job->smdata->local_hashtab;
    PMIX_LIST_FOREACH (kvi, &job_cb.kvs, pmix_kval_t) {
        if (PMIX_DATA_ARRAY == kvi->value->type) {
            // We support the following keys of this type.
            if (pmix_gds_shmem_keys_eq(kvi->key, PMIX_APP_INFO_ARRAY)) {
                PMIX_GDS_SHMEM_VOUT("storing app info array ---------------");
                rc = pmix_gds_shmem_store_app_array(job, kvi->value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else if (pmix_gds_shmem_keys_eq(kvi->key, PMIX_NODE_INFO_ARRAY)) {
                PMIX_GDS_SHMEM_VOUT("storing node info array --------------");
                rc = pmix_gds_shmem_store_node_array(
                    job, kvi->value, job->smdata->nodeinfo
                );
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else if (pmix_gds_shmem_keys_eq(kvi->key, PMIX_PROC_INFO_ARRAY)) {
                PMIX_GDS_SHMEM_VOUT("storing proc info array --------------");
                rc = pmix_gds_shmem_store_proc_data(job, kvi);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    break;
                }
            }
            else {
                rc = PMIX_ERR_NOT_SUPPORTED;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else {
            PMIX_GDS_SHMEM_VOUT("storing key=%s--------------", kvi->key);
            pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
            kv->key = pmix_tma_strdup(tma, kvi->key);
            PMIX_GDS_SHMEM_VALUE_XFER(rc, kv->value, kvi->value, tma);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_ERROR_LOG(rc);
                break;
            }
            rc = pmix_hash2_store(ht, PMIX_RANK_WILDCARD, kvi, NULL, 0);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
    }
    // No longer needed, as we already saved its data.
    PMIX_DESTRUCT(&job_cb);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Now pack the payload for delivery.
    const char *msg = ns->nspace;
    PMIX_BFROPS_PACK(rc, peer, reply, &msg, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    for (pmix_rank_t rank = 0; rank < ns->nprocs; rank++) {
        pmix_buffer_t buff;
        PMIX_CONSTRUCT(&buff, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, peer, &buff, &rank, 1, PMIX_PROC_RANK);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&buff);
            break;
        }

        rc = pack_shmem_connection_info(job, peer, &buff);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&buff);
            PMIX_ERROR_LOG(rc);
            break;
        }

        pmix_value_t blob;
        pmix_kval_t kv = {
            .key = PMIX_PROC_BLOB,
            .value = &blob
        };
        blob.type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(&buff, blob.data.bo.bytes, blob.data.bo.size);
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
        PMIX_VALUE_DESTRUCT(&blob);
        PMIX_DESTRUCT(&buff);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // If we have more than one local client for this nspace,
    // save this packed object so we don't do this again.
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) || 1 < ns->nlocalprocs) {
        PMIX_RETAIN(reply);
        ns->jobbkt = reply;
    }

    return rc;
}

/**
 *
 */
static pmix_status_t
server_register_job_info(
    struct pmix_peer_t *peer_struct,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_peer_t *peer = (pmix_peer_t *)peer_struct;
    pmix_namespace_t *ns = peer->nptr;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        // This function is only available on servers.
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    PMIX_GDS_SHMEM_VOUT(
        "%s: %s for peer %s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        PMIX_PEER_PRINT(peer)
    );
    // First see if we already have processed this data for another
    // peer in this nspace so we don't waste time doing it again.
    if (NULL != ns->jobbkt) {
        PMIX_GDS_SHMEM_VOUT(
            "[%s:%d] copying prepacked payload",
            pmix_globals.myid.nspace,
            pmix_globals.myid.rank
        );
        // We have packed this before, so we can just deliver it.
        PMIX_BFROPS_COPY_PAYLOAD(rc, peer, reply, ns->jobbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        // Now see if we have delivered it to
        // all our local clients for this nspace.
        if (!PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
            ns->ndelivered == ns->nlocalprocs) {
            // We have, so let's get rid of the packed copy of the data.
            PMIX_RELEASE(ns->jobbkt);
            ns->jobbkt = NULL;
        }
        return rc;
    }
    // Else we need to actually register the job info.
    return register_job_info(peer_struct, reply);
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buff
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), nspace
    );
    // Create a job tracker for the given namespace.
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(nspace, true, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_kval_t kval;
    PMIX_CONSTRUCT(&kval, pmix_kval_t);

    int32_t cnt = 1;
    PMIX_BFROPS_UNPACK(
        rc, pmix_client_globals.myserver,
        buff, &kval, &cnt, PMIX_KVAL
    );
    while (PMIX_SUCCESS == rc) {
        if (PMIX_CHECK_KEY(&kval, PMIX_PROC_BLOB)) {
            pmix_rank_t rank;
            pmix_byte_object_t *bo = &(kval.value->data.bo);

            pmix_buffer_t bbuff;
            PMIX_CONSTRUCT(&bbuff, pmix_buffer_t);

            PMIX_LOAD_BUFFER(
                pmix_client_globals.myserver,
                &bbuff, bo->bytes, bo->size
            );
            // Start by unpacking the rank.
            cnt = 1;
            PMIX_BFROPS_UNPACK(
                rc, pmix_client_globals.myserver,
                &bbuff, &rank, &cnt, PMIX_PROC_RANK
            );
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kval);
                PMIX_DESTRUCT(&bbuff);
                return rc;
            }
            rc = unpack_shmem_connection_info(job, &bbuff);
            PMIX_DESTRUCT(&bbuff);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kval);
                return rc;
            }
        }
        else {
            rc = PMIX_ERR_BAD_PARAM;
            break;
        }
        PMIX_DESTRUCT(&kval);
        PMIX_CONSTRUCT(&kval, pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            buff, &kval, &cnt, PMIX_KVAL
        );
    };
    // Release the leftover kval.
    PMIX_DESTRUCT(&kval);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Attach to the given shared-memory segment.
    uintptr_t mmap_addr = 0;
    rc = pmix_shmem_segment_attach(
        job->shmem,
        // The base address was populated above.
        job->shmem->base_address,
        &mmap_addr
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Make sure that we mapped to the requested address.
    if (mmap_addr != (uintptr_t)job->shmem->base_address) {
        // TODO(skg) We should add a nice error message here.
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)mmap_addr
    );
    // Now we need to initialize our data
    // structures from the shared-memory segment.
    job->smdata = job->shmem->base_address;
    // Update the TMA to point to its local function pointers.
    tma_init_function_pointers(&job->smdata->tma);
    pmix_gds_shmem_vout_smdata(job);
    // Protect memory: clients can only read from here.
#if 0
    mprotect(
        job->shmem->base_address,
        job->shmem->size, PROT_READ
    );
#endif
    // Done. Before this point the server should have populated the
    // shared-memory segment with the relevant data.
    return rc;
}

static pmix_status_t
store(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    pmix_kval_t *kval
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    // Setup a job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Let a full-featured gds module handle this.
    return job->ffgds->store(proc, scope, kval);
}

/**
 * This function is only called by the PMIx server when its host has received
 * data from some other peer. It therefore always contains data solely from
 * remote procs, and we shall store it accordingly.
 */
static pmix_status_t
store_modex(
    struct pmix_namespace_t *nspace_struct,
    pmix_buffer_t *buff,
    void *cbdata
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_namespace_t *nspace = (pmix_namespace_t *)nspace_struct;
    // Setup a job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(nspace->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Let a full-featured gds module handle this.
    return job->ffgds->store_modex(nspace_struct, buff, cbdata);
}

static pmix_status_t
server_setup_fork(
    const pmix_proc_t *peer,
    char ***env
) {
    PMIX_HIDE_UNUSED_PARAMS(peer, env);
    PMIX_GDS_SHMEM_VOUT_HERE();
    // Nothing to do here.
    return PMIX_SUCCESS;
}

static pmix_status_t
server_add_nspace(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(nspace, nlocalprocs, info, ninfo);
    PMIX_GDS_SHMEM_VOUT_HERE();
    // Nothing to do here.
    return PMIX_SUCCESS;
}

// TODO(skg) Implement.
static pmix_status_t
del_nspace(
    const char *nspace
) {
    PMIX_HIDE_UNUSED_PARAMS(nspace);
    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
assemb_kvs_req(
    const pmix_proc_t *proc,
    pmix_list_t *kvs,
    pmix_buffer_t *buff,
    void *cbdata
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    // Setup a job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Let a full-featured gds module handle this.
    return job->ffgds->assemb_kvs_req(proc, kvs, buff, cbdata);
}

static pmix_status_t
accept_kvs_resp(
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    pmix_status_t rc = PMIX_SUCCESS;
    const char *nspace = pmix_globals.mypeer->nptr->nspace;
    // Setup a job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Let a full-featured gds module handle this.
    return job->ffgds->accept_kvs_resp(buff);
}

pmix_gds_base_module_t pmix_shmem_module = {
    .name = PMIX_GDS_SHMEM_NAME,
    .is_tsafe = false,
    .init = module_init,
    .finalize = module_finalize,
    .assign_module = assign_module,
    .cache_job_info = server_cache_job_info,
    .register_job_info = server_register_job_info,
    .store_job_info = store_job_info,
    .store = store,
    .store_modex = store_modex,
    .fetch = pmix_gds_shmem_fetch,
    .setup_fork = server_setup_fork,
    .add_nspace = server_add_nspace,
    .del_nspace = del_nspace,
    .assemb_kvs_req = assemb_kvs_req,
    .accept_kvs_resp = accept_kvs_resp
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
