/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
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

#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"
#include "src/util/pmix_vmem.h"

#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"

//
// Notes for developers:
// We cannot use PMIX_CONSTRUCT for data that are stored in shared memory
// because their address is on the stack of the process in which they are
// constructed.
//

// Some items for future consideration:
// * Address FT case at some point. We need to have a broader conversion about
//   how we go about doing this. Ralph has some ideas.

/**
 * Key names used to find shared-memory segment info.
 */
#define SHMEM_SEG_BLOB_KEY "PMIX_GDS_SHMEM_SEG_BLOB"
#define SHMEM_SEG_NSID_KEY "PMIX_GDS_SHMEM_NSPACEID"
#define SHMEM_SEG_SMID_KEY "PMIX_GDS_SHMEM_SMSEGID"
#define SHMEM_SEG_PATH_KEY "PMIX_GDS_SHMEM_SEG_PATH"
#define SHMEM_SEG_SIZE_KEY "PMIX_GDS_SHMEM_SEG_SIZE"
#define SHMEM_SEG_HADR_KEY "PMIX_GDS_SHMEM_SEG_HADR"

/**
 * Stores packed job information.
 */
typedef struct {
    pmix_object_t super;
    /** Session ID associated with this job. */
    uint32_t session_id;
    /** Size of packed data. */
    size_t packed_size;
    /** Number of hash elements found. */
    size_t hash_table_size;
} pmix_gds_shmem_packed_local_job_info_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_packed_local_job_info_t);

/**
 * Stores modex sizing information.
 */
typedef struct {
    size_t size;
    size_t num_ht_elements;
} pmix_gds_shmem_modex_info_t;

/**
 * Stores modex context information.
 */
typedef struct {
    size_t buff_size;
    size_t nprocs;
} pmix_gds_shmem_modex_ctx_t;

static void
packed_job_info_construct(
    pmix_gds_shmem_packed_local_job_info_t *pji
) {
    pji->session_id = UINT32_MAX;
    pji->packed_size = 0;
    pji->hash_table_size = 0;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_packed_local_job_info_t,
    pmix_object_t,
    packed_job_info_construct,
    // Destruct is the same as above because we just invalidate the data.
    packed_job_info_construct
);

/**
 * Store unpacked shared-memory segment information.
 */
typedef struct {
    pmix_object_t super;
    char *nsid;
    pmix_gds_shmem_job_shmem_id_t smid;
    char *seg_path;
    size_t seg_size;
    size_t seg_hadr;
} pmix_gds_shmem_unpacked_seg_blob_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_unpacked_seg_blob_t);

static void
unpacked_seg_blob_construct(
    pmix_gds_shmem_unpacked_seg_blob_t *ub
) {
    ub->nsid = NULL;
    ub->smid = PMIX_GDS_SHMEM_INVALID_ID;
    ub->seg_path = NULL;
    ub->seg_size = 0;
    ub->seg_hadr = 0;
}

static void
unpacked_seg_blob_destruct(
    pmix_gds_shmem_unpacked_seg_blob_t *ub
) {
    free(ub->nsid);
    free(ub->seg_path);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_unpacked_seg_blob_t,
    pmix_object_t,
    unpacked_seg_blob_construct,
    unpacked_seg_blob_destruct
);

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
    const long long val = strtoll(str, &end, base);
    const int err = errno;

    if ((err == ERANGE && val == LLONG_MAX) ||
        (err == ERANGE && val == LLONG_MIN) ||
        *end != '\0') {
        return PMIX_ERROR;
    }
    *maybe_val = (size_t)val;
    return PMIX_SUCCESS;
}

/**
 * Architecture-specific address alignment.
 */
static inline void *
addr_align(
    void *base,
    size_t size
) {
#if 0 // Helpful debug
    PMIX_GDS_SHMEM_VVOUT("------------------------ADDRINN=%p,%zd", base, size);
#endif
    void *const res = (void *)(((uintptr_t)base + size + 7) & ~(uintptr_t)0x07);
#if 0 // Helpful debug
    // Make sure that it's 8-byte aligned.
    assert ((uintptr_t)res % 8 == 0);
    PMIX_GDS_SHMEM_VVOUT("------------------------ADDROUT=%p,%zd", res, size);
#endif
    return res;
}

static inline bool
tma_alloc_request_will_overflow(
    pmix_tma_t *tma,
    size_t alloc_size
) {
    const pmix_shmem_t *const backing_store = (pmix_shmem_t *)tma->data_context;
    const uintptr_t hdr_baseptr = (uintptr_t)backing_store->hdr_address;
    const uintptr_t data_baseptr = (uintptr_t)backing_store->data_address;
    const uintptr_t data_ptr_pos = (uintptr_t)*(tma->data_ptr);
    // Size of 'lost capacity` because of segment header.
    const size_t lost_capacity = (size_t)(data_baseptr - hdr_baseptr);
    const size_t bytes_used = (size_t)(data_ptr_pos - data_baseptr);

    return (bytes_used + alloc_size) > (backing_store->size - lost_capacity);
}

static inline void *
tma_malloc(
    pmix_tma_t *tma,
    size_t size
) {
    if (PMIX_UNLIKELY(tma_alloc_request_will_overflow(tma, size))) {
        return NULL;
    }

    void *const current = *(tma->data_ptr);
#if PMIX_ENABLE_DEBUG
    memset(current, 0, size);
#endif
    *(tma->data_ptr) = addr_align(current, size);
    return current;
}

static inline void *
tma_calloc(
    struct pmix_tma *tma,
    size_t nmemb,
    size_t size
) {
    const size_t real_size = nmemb * size;

    if (PMIX_UNLIKELY(tma_alloc_request_will_overflow(tma, real_size))) {
        return NULL;
    }

    void *const current = *(tma->data_ptr);
    memset(current, 0, real_size);
    *(tma->data_ptr) = addr_align(current, real_size);
    return current;
}

static inline void *
tma_realloc(
    pmix_tma_t *tma,
    void *ptr,
    size_t size
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr, size);
    // We don't support realloc.
    PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
    abort();
    return NULL;
}

static inline char *
tma_strdup(
    pmix_tma_t *tma,
    const char *s
) {
    const size_t size = strlen(s) + 1;

    if (PMIX_UNLIKELY(tma_alloc_request_will_overflow(tma, size))) {
        return NULL;
    }

    void *const current = *(tma->data_ptr);
    *(tma->data_ptr) = addr_align(current, size);
    return (char *)memmove(current, s, size);
}

static inline void *
tma_memmove(
    struct pmix_tma *tma,
    const void *src,
    size_t size
) {
    void *const current = *(tma->data_ptr);
    *(tma->data_ptr) = addr_align(current, size);
    return memmove(current, src, size);
}

static inline void
tma_free(
    struct pmix_tma *tma,
    void *ptr
) {
    // We don't currently reclaim freed space in our TMA.
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
    pmix_shmem_t *shmem_backing_store,
    pmix_tma_t *tma,
    void *data_ptr
) {
    tma_init_function_pointers(tma);
    tma->data_context = (void *)shmem_backing_store;
    tma->data_ptr = data_ptr;
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
    pmix_tma_t *const tma = pmix_obj_get_tma(&a->super.super);
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
    pmix_tma_t *const tma = pmix_obj_get_tma(&n->super.super);

    n->nodeid = UINT32_MAX;
    n->hostname = NULL;
    n->aliases = PMIX_NEW(pmix_list_t, tma);
    n->info = PMIX_NEW(pmix_list_t, tma);
}

static void
nodeinfo_destruct(
    pmix_gds_shmem_nodeinfo_t *n
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&n->super.super);

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
    PMIX_GDS_SHMEM_VVOUT_HERE();
    // Backing store ownership
    job->uid = geteuid();
    job->gid = getegid();
    job->chown = false;
    job->chgrp = false;
    // Namespace identification
    job->nspace_id = NULL;
    job->nspace = NULL;
    // Session
    job->session = PMIX_NEW(pmix_gds_shmem_session_t);
    // Job
    job->shmem_status = 0;
    job->shmem = PMIX_NEW(pmix_shmem_t);
    job->smdata = NULL;
    // Modex
    job->modex_shmem_status = 0;
    job->modex_shmem = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
    // Connection info
    job->conni = NULL;
}

static void
emit_shmem_usage_stats(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, shmem_id, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    pmix_tma_t *tma = NULL;
    const char *smname = NULL;
    switch (shmem_id) {
        case PMIX_GDS_SHMEM_JOB_ID:
            tma = &job->smdata->tma;
            smname = "smdata";
            break;
        case PMIX_GDS_SHMEM_MODEX_ID:
            tma = &job->smmodex->tma;
            smname = "smmodex";
            break;
        case PMIX_GDS_SHMEM_SESSION_ID:
            tma = &job->session->smdata->tma;
            smname = "smsession";
            break;
        case PMIX_GDS_SHMEM_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
            // This is an internal error.
            abort();
            return;
    }

    const size_t shmem_size = shmem->size;
    const size_t bytes_used = (size_t)((uintptr_t)*(tma->data_ptr)
                            - (uintptr_t)shmem->data_address);
    const float utilization = (bytes_used / (float)shmem_size) * 100.0;

    PMIX_GDS_SHMEM_VOUT(
        "%s memory statistics: "
        "segment size=%zd, bytes used=%zd, utilization=%.2f %%",
        smname, shmem_size, bytes_used, utilization
    );
}

static void
job_destruct(
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    if (job->nspace_id) {
        free(job->nspace_id);
    }
    if (job->nspace) {
        PMIX_RELEASE(job->nspace);
    }

    if (job->conni) {
        PMIX_RELEASE(job->conni);
    }

    static const pmix_gds_shmem_job_shmem_id_t shmem_ids[] = {
        PMIX_GDS_SHMEM_JOB_ID,
        PMIX_GDS_SHMEM_MODEX_ID,
        PMIX_GDS_SHMEM_SESSION_ID,
        PMIX_GDS_SHMEM_INVALID_ID
    };
    for (int i = 0; shmem_ids[i] != PMIX_GDS_SHMEM_INVALID_ID; ++i) {
        const pmix_gds_shmem_job_shmem_id_t sid = shmem_ids[i];

        pmix_shmem_t *shmem;
        rc = pmix_gds_shmem_get_job_shmem_by_id(job, sid, &shmem);
        if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        if (pmix_gds_shmem_has_status(job, sid, PMIX_GDS_SHMEM_MINE)) {
            // Emit usage status before we potentially destroy the segment.
            emit_shmem_usage_stats(job, sid);
        }
        // Releases memory for the structures located in shared-memory. This
        // will also unmap in case we need to later remap something in the
        // address space covered by this.
        PMIX_RELEASE(shmem);
        // Invalidate the shmem flags.
        pmix_gds_shmem_clearall_status(job, sid);
    }

    if (job->session) {
        PMIX_RELEASE(job->session);
    }
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
    pmix_tma_t *const tma = pmix_obj_get_tma(&a->super.super);

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

static void
session_construct(
    pmix_gds_shmem_session_t *s
) {
    s->shmem = PMIX_NEW(pmix_shmem_t);
    s->shmem_status = 0;
    s->smdata = NULL;
}

static void
session_destruct(
    pmix_gds_shmem_session_t *s
) {
    // job_destruct took care of our shmem.
    s->shmem = NULL;
    // Invalidate the shmem flags.
    s->shmem_status = 0;
    s->smdata = NULL;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_session_t,
    pmix_list_item_t,
    session_construct,
    session_destruct
);

static pmix_status_t
session_smdata_construct(
    pmix_gds_shmem_job_t *job,
    uint32_t sid
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // session know about its data located at the base of the segment.
    const size_t smdata_size = sizeof(*job->session->smdata);
    void *const baseaddr = job->session->shmem->data_address;

    job->session->smdata = baseaddr;
    memset(job->session->smdata, 0, smdata_size);
    // Save the starting address for TMA memory allocations.
    job->session->smdata->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(
        job->session->shmem,
        &job->session->smdata->tma,
        &job->session->smdata->current_addr
    );
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->session->smdata->tma.data_ptr) = addr_align(baseaddr, smdata_size);
    // We can now safely get our TMA.
    pmix_tma_t *const tma = &job->session->smdata->tma;
    // Now that we know the TMA, initialize smdata structures using it.
    job->session->smdata->id = sid;

    job->session->smdata->sessioninfo = PMIX_NEW(pmix_list_t, tma);
    if (!job->session->smdata->sessioninfo) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }

    job->session->smdata->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    if (!job->session->smdata->nodeinfo) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    pmix_gds_shmem_vout_smsession(job->session);
out:
    if (PMIX_SUCCESS != rc) {
        if (job->session->smdata->sessioninfo) {
            PMIX_RELEASE(job->session->smdata->sessioninfo);
        }
        if (job->session->smdata->nodeinfo) {
            PMIX_RELEASE(job->session->smdata->nodeinfo);
        }
    }
    return rc;
}

static pmix_status_t
job_smdata_construct(
    pmix_gds_shmem_job_t *job,
    size_t htsize
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its data located at the base of the segment.
    const size_t smdata_size = sizeof(*job->smdata);
    void *const baseaddr = job->shmem->data_address;

    job->smdata = baseaddr;
    memset(job->smdata, 0, smdata_size);
    // Save the starting address for TMA memory allocations.
    job->smdata->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(job->shmem, &job->smdata->tma, &job->smdata->current_addr);
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->smdata->tma.data_ptr) = addr_align(baseaddr, smdata_size);
    // We can now safely get our TMA.
    pmix_tma_t *const tma = &job->smdata->tma;
    // Now that we know the TMA, initialize smdata structures using it.
    job->smdata->jobinfo = PMIX_NEW(pmix_list_t, tma);
    if (!job->smdata->jobinfo) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }

    job->smdata->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    if (!job->smdata->nodeinfo) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }

    job->smdata->appinfo = PMIX_NEW(pmix_list_t, tma);
    if (!job->smdata->appinfo) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Will always have local data, so set it up.
    job->smdata->local_hashtab = PMIX_NEW(pmix_hash_table_t, tma);
    if (!job->smdata->local_hashtab) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    pmix_hash_table_init(job->smdata->local_hashtab, htsize);

    pmix_gds_shmem_vout_smdata(job);
out:
    if (PMIX_SUCCESS != rc) {
        if (job->smdata->jobinfo) {
            PMIX_RELEASE(job->smdata->jobinfo);
        }
        if (job->smdata->nodeinfo) {
            PMIX_RELEASE(job->smdata->nodeinfo);
        }
        if (job->smdata->appinfo) {
            PMIX_RELEASE(job->smdata->appinfo);
        }
        if (job->smdata->local_hashtab) {
            PMIX_RELEASE(job->smdata->local_hashtab);
        }
    }
    return rc;
}

static pmix_status_t
modex_smdata_construct(
    pmix_gds_shmem_job_t *job,
    size_t htsize
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its data located at the base of the segment.
    const size_t smmodex_size = sizeof(*job->smmodex);
    void *const baseaddr = job->modex_shmem->data_address;

    job->smmodex = baseaddr;
    memset(job->smmodex, 0, smmodex_size);
    // Save the starting address for TMA memory allocations.
    job->smmodex->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(job->modex_shmem, &job->smmodex->tma, &job->smmodex->current_addr);
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->smmodex->tma.data_ptr) = addr_align(baseaddr, smmodex_size);
    // We can now safely get our TMA.
    pmix_tma_t *const tma = &job->smmodex->tma;
    // Now that we know the TMA, initialize smdata structures using it.
    job->smmodex->hashtab = PMIX_NEW(pmix_hash_table_t, tma);
    if (!job->smmodex->hashtab) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_hash_table_init(job->smmodex->hashtab, htsize);

    pmix_gds_shmem_vout_smmodex(job);

    return PMIX_SUCCESS;
}

/**
 * Returns the base temp directory.
 */
static inline const char *
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
static inline const char *
get_shmem_backing_path(
    pmix_gds_shmem_job_t *job,
    const char *id
) {
    static char path[PMIX_PATH_MAX] = {'\0'};
    const char *basedir = fetch_base_tmpdir(job);
    // Now that we have the base path, append unique name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/%s-gds-%s.%s-%s.%s.%d",
        basedir, PACKAGE_NAME, PMIX_GDS_SHMEM_NAME,
        pmix_globals.hostname, job->nspace_id, id, getpid()
    );
    if (nw >= PMIX_PATH_MAX) {
        return NULL;
    }
    return path;
}

/**
 * Returns a valid shared-memory session name or NULL on error.
 */
static inline const char *
get_shmem_session_name(
    uint32_t session_id
) {
    static char name[64] = {'\0'};
    // Now that we have the base path, append unique name.
    size_t nw = snprintf(
        name, sizeof(name), "session.%zx", (size_t)session_id
    );
    if (nw >= sizeof(name)) {
        return NULL;
    }
    return name;
}

/**
 * Attaches to the given shared-memory segment.
 */
static pmix_status_t
shmem_attach(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    uintptr_t req_addr
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, shmem_id, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = pmix_shmem_segment_attach(
        shmem, req_addr, PMIX_SHMEM_MUST_MAP_AT_RADDR
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        // This type of error occurs when we
        // didn't map to the requested address.
        if (PMIX_ERR_NOT_AVAILABLE == rc) {
            pmix_show_help(
                "help-gds-shmem.txt",
                "shmem-segment-attach:address-mismatch",
                true, (size_t)req_addr, (size_t)shmem->hdr_address
            );
            rc = PMIX_ERROR;
        }
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)shmem->hdr_address
    );
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(shmem);
    }
    else {
        pmix_gds_shmem_set_status(
            job, shmem_id, PMIX_GDS_SHMEM_ATTACHED
        );
    }
    return rc;
}

static inline pmix_status_t
init_client_side_sm_data(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
) {
    switch (shmem_id) {
        case PMIX_GDS_SHMEM_JOB_ID:
            job->smdata = job->shmem->data_address;
            pmix_gds_shmem_vout_smdata(job);
            break;
        case PMIX_GDS_SHMEM_SESSION_ID:
            job->session->smdata = job->session->shmem->data_address;
            pmix_gds_shmem_vout_smsession(job->session);
            break;
        case PMIX_GDS_SHMEM_MODEX_ID:
            job->smmodex = job->modex_shmem->data_address;
            pmix_gds_shmem_vout_smmodex(job);
            break;
        case PMIX_GDS_SHMEM_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERROR);
            abort();
            return PMIX_ERROR;
    }
    // Segment is ready for use by the client.
    pmix_gds_shmem_set_status(job, shmem_id, PMIX_GDS_SHMEM_READY_FOR_USE);
    // Note: don't update the TMA to point to its local function pointers
    // because clients should only be reading from the shared-memory segment.
    return PMIX_SUCCESS;
}

static pmix_status_t
shmem_segment_attach_and_init(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_unpacked_seg_blob_t *seginfo
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, seginfo->smid, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Initialize the segment path.
    const size_t buffmax = sizeof(shmem->backing_path);
    pmix_string_copy(shmem->backing_path, seginfo->seg_path, buffmax);
    // Initialize the segment size.
    shmem->size = seginfo->seg_size;

    const uintptr_t req_addr = (uintptr_t)seginfo->seg_hadr;
    rc = shmem_attach(job, seginfo->smid, req_addr);
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Now we can safely initialize our shared data structures.
    rc = init_client_side_sm_data(job, seginfo->smid);
#if 0
    // Protect memory: clients can only read from here.
    mprotect(
        shmem->data_address, shmem->size, PROT_READ
    );
#endif
    return rc;
}

/**
 * Updates backing file permissions based on PMIx directives.
 */
static pmix_status_t
shmem_segment_fix_perms(
    pmix_gds_shmem_job_t *job,
    pmix_shmem_t *shmem
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Update segment ownership and permissions?
    if (job->chown || job->chgrp) {
        const uid_t uid = job->chown ? job->uid : (uid_t)-1;
        const gid_t gid = job->chgrp ? job->gid : (gid_t)-1;

        rc = pmix_shmem_segment_chown(shmem, uid, gid);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }

        rc = pmix_shmem_segment_chmod(
            shmem, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return rc;
}

/**
 * Create and attach to a shared-memory segment.
 */
static pmix_status_t
shmem_segment_create_and_attach(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    const char *segment_name,
    size_t segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Pad given size to fill remaining space on the last page.
    const size_t real_segsize = pmix_shmem_utils_pad_to_page(segment_size);
    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &base_addr, real_segsize
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: %s found vmhole at address=0x%zx",
        __func__, segment_name, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    const char *segment_path = get_shmem_backing_path(job, segment_name);
    if (PMIX_UNLIKELY(!segment_path)) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s (size=%zd B)",
        __func__, segment_path, real_segsize
    );
    // Get a handle to the appropriate shmem.
    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(job, shmem_id, &shmem);
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(
        shmem, real_segsize, segment_path
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Attach to the shared-memory segment.
    rc = shmem_attach(job, shmem_id, (uintptr_t)base_addr);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Fix-up backing file permission.
    rc = shmem_segment_fix_perms(job, shmem);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
out:
    if (PMIX_SUCCESS == rc) {
        // I created it, so note that it is mine.
        pmix_gds_shmem_set_status(
            job, shmem_id, PMIX_GDS_SHMEM_MINE
        );
    }
    return rc;
}

static pmix_status_t
module_init(
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);
    PMIX_GDS_SHMEM_VVOUT_HERE();

    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.jobs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.sessions, pmix_list_t);
    return PMIX_SUCCESS;
}

static void
module_finalize(void)
{
    PMIX_GDS_SHMEM_VVOUT_HERE();
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem_component.sessions);
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem_component.jobs);
}

static pmix_status_t
assign_module(
    pmix_info_t *info,
    size_t ninfo,
    int *priority
) {
    static const int max_priority = 100;
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    // The incoming info always overrides anything in the
    // environment as it is set by the application itself.
    bool specified = false;
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GDS_MODULE)) {
            char **options = NULL;
            specified = true; // They specified who they want.
            options = PMIx_Argv_split(info[n].value.data.string, ',');
            for (size_t m = 0; NULL != options[m]; m++) {
                if (0 == strcmp(options[m], PMIX_GDS_SHMEM_NAME)) {
                    // They specifically asked for us.
                    *priority = max_priority;
                    break;
                }
            }
            PMIx_Argv_free(options);
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
    PMIX_GDS_SHMEM_VVOUT_HERE();
    // We don't support this operation.
    return PMIX_ERR_NOT_SUPPORTED;
}

/**
 *
 */
static pmix_status_t
prepare_shmem_stores_for_local_job_data(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_packed_local_job_info_t *pji
) {
    pmix_status_t rc = PMIX_SUCCESS;
    static const float fluff = 3.0;
    const size_t kvsize = (sizeof(pmix_kval_t) + sizeof(pmix_value_t));
    // Initial hash table size.
    const size_t htsize = pji->hash_table_size;
    // Calculate a rough estimate on the amount of storage required to store the
    // values associated with the pmix_gds_shmem_shared_job_data_t. Err on the
    // side of overestimation.
    size_t seg_size = sizeof(*job->smdata);
    // We need to store a hash table in the shared-memory segment, so calculate
    // a rough estimate on the memory required for its storage.
    seg_size += sizeof(pmix_hash_table_t);
    seg_size += htsize * pmix_hash_table_sizeof_hash_element();
    // Add a little extra to compensate for the value storage requirements. Here
    // we add an additional storage space for each entry.
    seg_size += htsize * kvsize;
    // Finally add the data size contribution, plus a little extra.
    seg_size += pji->packed_size;
    // Include some extra fluff that empirically seems reasonable.
    seg_size *= fluff;
    // Adjust (increase or decrease) segment size by the given parameter size.
    seg_size *= pmix_gds_shmem_segment_size_multiplier;
    // Create and attach to the shared-memory segment associated with this job.
    // This will be the backing store for data associated with static, read-only
    // data shared between the server and its clients.
    rc = shmem_segment_create_and_attach(
        job, PMIX_GDS_SHMEM_JOB_ID, "jobdata", seg_size
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Do the same for the job's session information. Note that we recycle the
    // segment size calculated above because we know that it will be at least as
    // big as we need for this session information.
    const char *session_name = get_shmem_session_name(pji->session_id);
    if (PMIX_UNLIKELY(!session_name)) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = shmem_segment_create_and_attach(
        job, PMIX_GDS_SHMEM_SESSION_ID, session_name, seg_size
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Construct shared-memory data structures for job and session.
    rc = job_smdata_construct(job, htsize);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = session_smdata_construct(job, pji->session_id);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static inline pmix_status_t
pack_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VVOUT(
        "%s:%s for peer (ID=%d) namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        peer->info->peerid, job->nspace_id
    );

    pmix_shmem_t *shmem;
    rc = pmix_gds_shmem_get_job_shmem_by_id(
        job, shmem_id, &shmem
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_kval_t kv;
    do {
        // Pack the namespace name.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_NSID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(job->nspace_id);
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the shmem ID as string.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_SMID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        int nw = asprintf(&kv.value->data.string, "%zd", (size_t)shmem_id);
        if (PMIX_UNLIKELY(nw == -1)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the backing file path.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_PATH_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(shmem->backing_path);
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack attach size to shared-memory segment.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_SIZE_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        nw = asprintf(&kv.value->data.string, "%zx", shmem->size);
        if (PMIX_UNLIKELY(nw == -1)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the addresses used to attach to the shared-memory segment.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM_SEG_HADR_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        kv.value->type = PMIX_STRING;
        nw = asprintf(
            &kv.value->data.string, "%zx", (size_t)shmem->hdr_address
        );
        if (PMIX_UNLIKELY(nw == -1)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    } while (false);
    PMIX_DESTRUCT(&kv);

    return rc;
}

/**
 * Emits the contents of an pmix_gds_shmem_unpacked_seg_blob_t.
 */
static inline void
vout_unpacked_seg_blob(
    pmix_gds_shmem_unpacked_seg_blob_t *usb,
    const char *called_by
) {
#if (PMIX_ENABLE_DEBUG == 0)
    PMIX_HIDE_UNUSED_PARAMS(usb, called_by);
#endif
    PMIX_GDS_SHMEM_VVOUT(
        "%s: "
        SHMEM_SEG_NSID_KEY "=%s "
        SHMEM_SEG_SMID_KEY "=%u "
        SHMEM_SEG_PATH_KEY "=%s "
        SHMEM_SEG_SIZE_KEY "=%zd "
        SHMEM_SEG_HADR_KEY "=0x%zx",
        called_by, usb->nsid, (unsigned)usb->smid,
        usb->seg_path, usb->seg_size, usb->seg_hadr
    );
}

/**
 * Sets shared-memory connection information from a pmix_kval_t by unpacking the
 * blob and saving the values for the caller. If successful, returns relevant
 * data associated with the unpacked data.
 */
static inline pmix_status_t
unpack_shmem_connection_info(
    pmix_kval_t *kvbo,
    pmix_gds_shmem_unpacked_seg_blob_t *usb
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Make sure this is the expected type.
    if (PMIX_UNLIKELY(PMIX_BYTE_OBJECT != kvbo->value->type)) {
        rc = PMIX_ERR_TYPE_MISMATCH;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_buffer_t buffer;
    PMIX_CONSTRUCT(&buffer, pmix_buffer_t);

    PMIX_LOAD_BUFFER(
        pmix_client_globals.myserver,
        &buffer,
        kvbo->value->data.bo.bytes,
        kvbo->value->data.bo.size
    );

    pmix_kval_t kv;
    while (true) {
        PMIX_CONSTRUCT(&kv, pmix_kval_t);

        int32_t count = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            &buffer, &kv, &count, PMIX_KVAL
        );
        if (PMIX_SUCCESS != rc) {
            break;
        }

        const char *const val = kv.value->data.string;
        if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_NSID_KEY)) {
            int nw = asprintf(&usb->nsid, "%s", val);
            if (PMIX_UNLIKELY(nw == -1)) {
                rc = PMIX_ERR_NOMEM;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_SMID_KEY)) {
            size_t st_shmem_id;
            rc = strtost(val, 10, &st_shmem_id);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            usb->smid = (pmix_gds_shmem_job_shmem_id_t)st_shmem_id;
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_PATH_KEY)) {
            int nw = asprintf(&usb->seg_path, "%s", val);
            if (PMIX_UNLIKELY(nw == -1)) {
                rc = PMIX_ERR_NOMEM;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_SIZE_KEY)) {
            rc = strtost(val, 16, &usb->seg_size);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM_SEG_HADR_KEY)) {
            rc = strtost(val, 16, &usb->seg_hadr);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else {
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Done with this one.
        PMIX_DESTRUCT(&kv);
    }
    // Catch last kval.
    PMIX_DESTRUCT(&kv);
    PMIX_DESTRUCT(&buffer);

    if (PMIX_UNLIKELY(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        PMIX_ERROR_LOG(rc);
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
    }
    else {
        vout_unpacked_seg_blob(usb, __func__);
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/**
 * Fetches a complete copy of the job-level information.
 */
static pmix_status_t
fetch_local_job_data(
    const char *nspace,
    pmix_cb_t *job_cb
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_proc_t wildcard;
    PMIX_LOAD_PROCID(&wildcard, nspace, PMIX_RANK_WILDCARD);

    job_cb->key = NULL;
    job_cb->proc = &wildcard;
    job_cb->copy = true;

    job_cb->scope = PMIX_LOCAL;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, job_cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

/**
 * Internally the hash table can do some interesting sizing calculations, so we
 * just construct a temporary one with the number of expected elements, then
 * query it for its actual capacity.
 */
static inline size_t
get_actual_hashtab_capacity(
    size_t num_elements
) {
    pmix_hash_table_t tmp;
    PMIX_CONSTRUCT(&tmp, pmix_hash_table_t);
    pmix_hash_table_init(&tmp, num_elements);
    // Grab the actual capacity.
    const size_t result = tmp.ht_capacity;
    PMIX_DESTRUCT(&tmp);

    return result;
}

static inline pmix_status_t
get_local_job_data_info(
    pmix_cb_t *job_cb,
    pmix_gds_shmem_packed_local_job_info_t *pji
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t nhtentries = 0;
    uint32_t sid = UINT32_MAX;

    pmix_buffer_t data;
    PMIX_CONSTRUCT(&data, pmix_buffer_t);

    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &job_cb->kvs, pmix_kval_t) {
        // Calculate some statistics so we can make an educated estimate on the
        // size of structures we need for our backing store.
        if (PMIX_DATA_ARRAY == kvi->value->type) {
            // PMIX_PROC_DATA is stored in the hash table.
            if (PMIX_CHECK_KEY(kvi, PMIX_PROC_DATA)) {
                nhtentries += kvi->value->data.darray->size;
            }
            // See if this is the job's session ID. If so, capture it.
            pmix_info_t *info = (pmix_info_t *)kvi->value->data.darray->array;
            if (PMIX_CHECK_KEY(&info[0], PMIX_SESSION_ID)) {
                PMIX_VALUE_GET_NUMBER(rc, &info[0].value, sid, uint32_t);
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    PMIX_ERROR_LOG(rc);
                    goto out;
                }
            }
        }
        // Just a key/value pair, so they will likely go into the hash table.
        else {
            nhtentries += 1;
        }

        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &data, kvi, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto out;
        }
    }
    pji->session_id = sid;
    pji->packed_size = data.bytes_used;
    pji->hash_table_size = get_actual_hashtab_capacity(nhtentries);
out:
    PMIX_DESTRUCT(&data);
    return rc;
}

static inline pmix_status_t
pack_shmem_seg_blob(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    struct pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Only pack connection info that is ready for use. Otherwise,
    // it's bogus data that we shouldn't be sharing it with our clients.
    const bool ready_for_use = pmix_gds_shmem_has_status(
        job, shmem_id, PMIX_GDS_SHMEM_READY_FOR_USE
    );
    if (!ready_for_use) {
        return rc;
    }

    pmix_buffer_t buff;
    do {
        PMIX_CONSTRUCT(&buff, pmix_buffer_t);

        rc = pack_shmem_connection_info(
            job, shmem_id, peer, &buff
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }

        pmix_value_t blob = {
            .type = PMIX_BYTE_OBJECT
        };
        pmix_kval_t kv = {
            .key = SHMEM_SEG_BLOB_KEY,
            .value = &blob
        };

        PMIX_UNLOAD_BUFFER(&buff, blob.data.bo.bytes, blob.data.bo.size);
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_VALUE_DESTRUCT(&blob);
    } while (false);
    PMIX_DESTRUCT(&buff);

    return rc;
}

static pmix_status_t
cache_connection_info_for_job_shmem(
    pmix_gds_shmem_job_t *job
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_peer_t *const me = pmix_globals.mypeer;

    // Create a new buffer that will store the
    // job's shared-memory connection info.
    job->conni = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(!job->conni)) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Pack the payload for delivery. Note that the message we are going to send
    // is simply the shared memory connection information that is shared among
    // clients on a single node.
    // Start with the namespace name.
    PMIX_BFROPS_PACK(rc, me, job->conni, &job->nspace_id, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Pack the shared-memory segment information.
    // First for the job.
    rc = pack_shmem_seg_blob(
        job, PMIX_GDS_SHMEM_JOB_ID, me, job->conni
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Then for the session info.
    rc = pack_shmem_seg_blob(
        job, PMIX_GDS_SHMEM_SESSION_ID, me, job->conni
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(job->conni);
    }
    return rc;
}

static pmix_status_t
server_register_new_job_info(
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    // Ask for a complete copy of the job-level information.
    pmix_cb_t job_cb;
    PMIX_CONSTRUCT(&job_cb, pmix_cb_t);

    pmix_gds_shmem_packed_local_job_info_t pji;
    PMIX_CONSTRUCT(&pji, pmix_gds_shmem_packed_local_job_info_t);

    rc = fetch_local_job_data(job->nspace_id, &job_cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Pack the data so we can see how large it is. This will help inform how
    // large to make the shared-memory segments associated with these data.
    rc = get_local_job_data_info(&job_cb, &pji);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Get the shared-memory segments ready for job data.
    rc = prepare_shmem_stores_for_local_job_data(job, &pji);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Store fetched data into a shared-memory segment.
    rc = pmix_gds_shmem_store_local_job_data_in_shmem(job, &job_cb.kvs);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
out:
    PMIX_DESTRUCT(&job_cb);
    PMIX_DESTRUCT(&pji);
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
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_peer_t *const peer = (pmix_peer_t *)peer_struct;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        // This function is only available on servers.
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    // Create the job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(peer->nptr->nspace, true, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    do {
        // First see if we already have processed this
        // data so we don't waste time doing it again.
        if (job->conni) {
            break;
        }
        // We don't, so register the new job info.
        PMIX_GDS_SHMEM_VVOUT(
            "%s: %s registering new job info for namespace=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), job->nspace_id
        );

        rc = server_register_new_job_info(job);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }

        rc = cache_connection_info_for_job_shmem(job);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    } while (false);

    if (PMIX_LIKELY(PMIX_SUCCESS == rc)) {
        // Copy reply over to send the connection info to the given peer.
        PMIX_BFROPS_COPY_PAYLOAD(rc, peer, reply, job->conni);
    }
    else {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
unpack_shmem_seg_blob_and_attach_if_necessary(
    pmix_kval_t *kvbo
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem_unpacked_seg_blob_t usb;
    PMIX_CONSTRUCT(&usb, pmix_gds_shmem_unpacked_seg_blob_t);
    do {
        rc = unpack_shmem_connection_info(kvbo, &usb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Get the associated job tracker.
        pmix_gds_shmem_job_t *job;
        rc = pmix_gds_shmem_get_job_tracker(usb.nsid, true, &job);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Make sure we aren't already attached to the given shmem.
        if (pmix_gds_shmem_has_status(job, usb.smid, PMIX_GDS_SHMEM_ATTACHED)) {
            break;
        }
        // Looks like we have to attach and initialize it.
        rc = shmem_segment_attach_and_init(job, &usb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    } while (false);
    PMIX_DESTRUCT(&usb);

    return rc;
}

static pmix_status_t
client_connect_to_shmem_from_buffi(
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_kval_t kval;
    while (true) {
        PMIX_CONSTRUCT(&kval, pmix_kval_t);

        int32_t nvals = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            buff, &kval, &nvals , PMIX_KVAL
        );
        if (PMIX_SUCCESS != rc) {
            break;
        }

        if (PMIX_CHECK_KEY(&kval, SHMEM_SEG_BLOB_KEY)) {
            rc = unpack_shmem_seg_blob_and_attach_if_necessary(&kval);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else {
            PMIX_GDS_SHMEM_VOUT(
                "%s:ERROR unexpected key=%s", __func__, kval.key
            );
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kval);
    };
    // Release the leftover kval.
    PMIX_DESTRUCT(&kval);

    if (PMIX_UNLIKELY(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        PMIX_ERROR_LOG(rc);
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), nspace
    );
    // Done. Before this point the server should have populated the
    // shared-memory segment with the relevant data.
    return client_connect_to_shmem_from_buffi(buff);
}

/**
 * Returns size required to store modex data.
 */
static pmix_gds_shmem_modex_info_t
get_modex_sizing_data(
    const pmix_gds_shmem_modex_ctx_t *mctx
) {
    const size_t kval_size = sizeof(pmix_kval_t);
    // The default values if not provided with modex size info. More fluff than
    // in other places because this calculation is more imprecise. In many ways
    // this is okay because mmap() implements demand paging.
    float fluff = 5.0;
    // Multiplier to fudge compression factor. zlib max compression is 5:1.
    size_t segment_size = mctx->buff_size * 5;
    // Get an estimate on the number of kvals we need to store.
    const size_t nkvals = (segment_size / (float)kval_size) + kval_size;
    // Get the required hash table size based on number of kvals.
    const size_t nhtelems = get_actual_hashtab_capacity(nkvals);
    // We also need storage space for the hash table and its elements.
    segment_size += sizeof(pmix_hash_table_t);
    segment_size += nhtelems * pmix_hash_table_sizeof_hash_element();
    // Include some extra fluff that empirically seems reasonable.
    segment_size *= fluff;
    // Adjust (increase or decrease) segment size by the given parameter size.
    segment_size *= pmix_gds_shmem_segment_size_multiplier;

    pmix_gds_shmem_modex_info_t result = {
        .size = segment_size,
        .num_ht_elements = nhtelems
    };
    return result;
}

/**
 * This gets called for each process participating in the modex.
 */
static pmix_status_t
server_store_modex_cb(
    pmix_gds_base_ctx_t ctx,
    pmix_proc_t *proc,
    pmix_gds_modex_key_fmt_t key_fmt,
    char **kmap,
    pmix_buffer_t *pbkt
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        proc->nspace
    );

    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_gds_shmem_modex_ctx_t *mctx = (pmix_gds_shmem_modex_ctx_t *)ctx;

    const bool attached = pmix_gds_shmem_has_status(
        job, PMIX_GDS_SHMEM_MODEX_ID, PMIX_GDS_SHMEM_ATTACHED
    );
    if (!attached) {
        // Get the global packed buffer size from ctx.
        pmix_gds_shmem_modex_info_t minfo = get_modex_sizing_data(mctx);
        // Create and attach to the shared-memory
        // segment that will back these data.
        rc = shmem_segment_create_and_attach(
            job, PMIX_GDS_SHMEM_MODEX_ID, "modexdata", minfo.size
        );
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }

        rc = modex_smdata_construct(job, minfo.num_ht_elements);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    pmix_hash_table_t *const ht = job->smmodex->hashtab;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);
    // This is data returned via the PMIx_Fence call when data collection was
    // requested, so it only contains REMOTE/GLOBAL data. The byte object
    // contains the rank followed by pmix_kval_ts.
    pmix_kval_t *kv;
    // Unpack the values until we hit the end of the buffer.
    while (true) {
        kv = PMIX_NEW(pmix_kval_t, tma);
        if (PMIX_UNLIKELY(NULL == kv)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }

        rc = pmix_gds_base_modex_unpack_kval(key_fmt, pbkt, kmap, kv);
        if (PMIX_SUCCESS != rc) {
            break;
        }

        const pmix_rank_t rank = proc->rank;
        // If the rank is undefined, then we store it on the remote table of
        // rank=0 as we know that rank must always exist.
        if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_shmem_store_qualified(
                ht, (PMIX_RANK_UNDEF == rank) ? 0 : rank, kv->value
            );
        }
        else {
            rc = pmix_hash_store(
                ht, (PMIX_RANK_UNDEF == rank) ? 0 : rank, kv, NULL, 0
            );
        }
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(kv);
    }
    PMIX_DESTRUCT(kv);

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }
    else {
        // Last process in modex?
        if (--mctx->nprocs == 0) {
            // Segment is ready for use.
            pmix_gds_shmem_set_status(
                job, PMIX_GDS_SHMEM_MODEX_ID, PMIX_GDS_SHMEM_READY_FOR_USE
            );
        }
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/**
 * This function is only called by the PMIx server when its host has received
 * data from some other peer. It therefore always contains data solely from
 * remote procs, and we shall store it accordingly.
 */
static pmix_status_t
server_store_modex(
    struct pmix_namespace_t *ns,
    pmix_buffer_t *buff,
    void *cbdata
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_namespace_t *const namespace = (pmix_namespace_t *)ns;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for namespace=%s (nprocs=%zd, buff_size=%zd)", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), namespace->nspace,
        (size_t)namespace->nprocs, buff->bytes_used
    );
    // Cache modex info for the callbacks.
    pmix_gds_shmem_modex_ctx_t mctx = {
        .buff_size = buff->bytes_used,
        .nprocs = namespace->nprocs
    };
    pmix_gds_base_ctx_t ctx = (pmix_gds_base_ctx_t)&mctx;

    return pmix_gds_base_store_modex(
        ns, buff, ctx, server_store_modex_cb, cbdata
    );
}

static pmix_status_t
server_setup_fork(
    const pmix_proc_t *peer,
    char ***env
) {
    PMIX_HIDE_UNUSED_PARAMS(peer, env);
    PMIX_GDS_SHMEM_VVOUT_HERE();
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
    PMIX_HIDE_UNUSED_PARAMS(nlocalprocs);
    PMIX_GDS_SHMEM_VVOUT_HERE();

    // Create a job tracker for this nspace.
    pmix_gds_shmem_job_t *job;
    pmix_status_t rc = pmix_gds_shmem_get_job_tracker(nspace, true, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    for (size_t i = 0; i < ninfo; ++i) {
        if (PMIX_CHECK_KEY(&info[i], PMIX_USERID)) {
            const uid_t nuid = (uid_t)info[i].value.data.uint32;
            PMIX_GDS_SHMEM_VOUT(
                "%s: updating nspace=%s UID from %zd to %zd",
                __func__, nspace, (size_t)job->uid, (size_t)nuid
            );
            job->uid = nuid;
            job->chown = true;
        }
        else if (PMIX_CHECK_KEY(&info[i], PMIX_GRPID)) {
            const gid_t ngid = (gid_t)info[i].value.data.uint32;
            PMIX_GDS_SHMEM_VOUT(
                "%s: updating nspace=%s GID from %zd to %zd",
                __func__, nspace, (size_t)job->gid, (size_t)ngid
            );
            job->gid = ngid;
            job->chgrp = true;
        }
    }
    return rc;
}

static pmix_status_t
del_nspace(
    const char *nspace
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();

    PMIX_GDS_SHMEM_VOUT(
        "%s: %s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), nspace
    );

    pmix_gds_shmem_job_t *ji;
    pmix_gds_shmem_component_t *const component = &pmix_mca_gds_shmem_component;
    PMIX_LIST_FOREACH (ji, &component->jobs, pmix_gds_shmem_job_t) {
        if (0 == strcmp(nspace, ji->nspace_id)) {
            pmix_list_remove_item(&component->jobs, &ji->super);
            PMIX_RELEASE(ji);
            break;
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
server_mark_modex_complete(
    struct pmix_peer_t *peer,
    pmix_list_t *nslist,
    pmix_buffer_t *reply
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    // Pack connection info for each ns in nslist.
    pmix_nspace_caddy_t *nsi;
    PMIX_LIST_FOREACH (nsi, nslist, pmix_nspace_caddy_t) {
        // false here because we should already know about the nspace.
        pmix_gds_shmem_job_t *job;
        rc = pmix_gds_shmem_get_job_tracker(
            nsi->ns->nspace, false, &job
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Pack modex info, if it is ready to be shared.
        rc = pack_shmem_seg_blob(
            job, PMIX_GDS_SHMEM_MODEX_ID, peer, reply
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    return rc;
}

static pmix_status_t
client_recv_modex_complete(
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM_VVOUT_HERE();
    return client_connect_to_shmem_from_buffi(buff);
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
    .store = NULL,
    .store_modex = server_store_modex,
    .fetch = pmix_gds_shmem_fetch,
    .setup_fork = server_setup_fork,
    .add_nspace = server_add_nspace,
    .del_nspace = del_nspace,
    .assemb_kvs_req = NULL,
    .accept_kvs_resp = NULL,
    .mark_modex_complete = server_mark_modex_complete,
    .recv_modex_complete = client_recv_modex_complete
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
