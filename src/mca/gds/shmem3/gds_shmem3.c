/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2024 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "gds_shmem3.h"
#include "gds_shmem3_utils.h"
#include "gds_shmem3_store.h"
#include "gds_shmem3_fetch.h"

#include "src/include/pmix_dictionary.h"

#include "src/util/pmix_hash.h"
#include "src/util/pmix_printf.h"
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
#define SHMEM3_SEG_BLOB_KEY "PMIX_GDS_SHMEM3_SEG_BLOB"
#define SHMEM3_SEG_NSID_KEY "PMIX_GDS_SHMEM3_NSPACEID"
#define SHMEM3_SEG_SMID_KEY "PMIX_GDS_SHMEM3_SMSEGID"
#define SHMEM3_SEG_PATH_KEY "PMIX_GDS_SHMEM3_SEG_PATH"
#define SHMEM3_SEG_SIZE_KEY "PMIX_GDS_SHMEM3_SEG_SIZE"
#define SHMEM3_SEG_HADR_KEY "PMIX_GDS_SHMEM3_SEG_HADR"
/* "1" when this modex generation holds only what changed, so the client
 * must keep the generations before it; "0" when it stands on its own and
 * supersedes them. Only ever packed for the modex segment. */
#define SHMEM3_SEG_DELTA_KEY "PMIX_GDS_SHMEM3_SEG_DELTA"


#define EMSG_SHMEM3_IS_BROKEN "\n***\nAn unrecoverable error occurred in the " \
"gds/shmem3 component.\nResolve this issue by disabling it. Set in your "      \
"environment the following:\nPMIX_MCA_gds=hash\n***\n"

#define EMSG_SHMEM3_OOM "\n***\nA memory allocation backed by shared-memory "  \
"failed in the gds/shmem3 component.\nResolve this issue by either:"           \
"\n1.) Increasing the value of PMIX_MCA_gds_shmem3_segment_size_multiplier "   \
"\nor"                                                                         \
"\n2.) Disabling gds/shmem3 via PMIX_MCA_gds=hash\n***\n"

/**
 * Stores packed job information.
 */
typedef struct {
    pmix_object_t super;
    /** Session ID associated with this job. */
    uint32_t session_id;
    /** Size of packed data. */
    size_t packed_size;
    /** Number of elements the local hash table needs - one per rank. */
    size_t hash_table_size;
    /** Number of key/value pairs those elements will hold between them. */
    size_t nkvals;
} pmix_gds_shmem3_packed_local_job_info_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_packed_local_job_info_t);

/**
 * Stores modex sizing information.
 */
typedef struct {
    size_t size;
    size_t num_ht_elements;
} pmix_gds_shmem3_modex_info_t;

static void
packed_job_info_construct(
    pmix_gds_shmem3_packed_local_job_info_t *pji
) {
    pji->session_id = UINT32_MAX;
    pji->packed_size = 0;
    pji->hash_table_size = 0;
    pji->nkvals = 0;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_packed_local_job_info_t,
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
    pmix_gds_shmem3_job_shmem3_id_t smid;
    char *seg_path;
    size_t seg_size;
    size_t seg_hadr;
    bool is_delta;
} pmix_gds_shmem3_unpacked_seg_blob_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_unpacked_seg_blob_t);

static void
unpacked_seg_blob_construct(
    pmix_gds_shmem3_unpacked_seg_blob_t *ub
) {
    ub->nsid = NULL;
    ub->smid = PMIX_GDS_SHMEM3_INVALID_ID;
    ub->seg_path = NULL;
    ub->seg_size = 0;
    ub->seg_hadr = 0;
    ub->is_delta = false;
}

static void
unpacked_seg_blob_destruct(
    pmix_gds_shmem3_unpacked_seg_blob_t *ub
) {
    free(ub->nsid);
    free(ub->seg_path);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_unpacked_seg_blob_t,
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
        end == str || *end != '\0') {
        return PMIX_ERROR;
    }
    // Every caller wants a size or an address. A negative reaches them as
    // a very large size_t - SIZE_MAX for "-1" - which then fails as an
    // absurd segment size or an unmappable address, some distance from
    // the malformed string that produced it. Refuse it here instead.
    if (val < 0) {
        return PMIX_ERROR;
    }
    *maybe_val = (size_t)val;
    return PMIX_SUCCESS;
}

/**
 * Stores TMA memory allocation information.
 *
 * One of these sits immediately ahead of every block the TMA hands out, so
 * the block carries its own size. Two properties matter:
 *
 * - It must stay a multiple of 8 bytes. addr_align() keeps the bump pointer
 *   8-byte aligned, and the payload address is the header address plus this
 *   size, so a header that is not a multiple of 8 would misalign every
 *   object in the segment.
 * - It lives in the segment, but nothing outside this file reads it, so it
 *   is not part of the shared *layout*: clients reach in-segment objects
 *   through pointers, never by walking allocations. It therefore does not
 *   belong in PMIX_GDS_SHMEM3_LAYOUT_ID and does not require a rename.
 *
 * This replaced a side hash table (addr -> extent) that the allocator
 * maintained on the process heap. That table cost two heap allocations and
 * a ptr-keyed hash insert on *every* allocation - and, because free is a
 * no-op here, it only ever grew, then had to be walked entry by entry at
 * teardown. All of it existed to answer one question, asked only by
 * tma_realloc(): how big was this block? A header answers it for the price
 * of 16 bytes of segment space, which is noise against the sizing fluff
 * these segments already carry.
 */
typedef struct {
    /** Size of allocation. */
    size_t extent;
    /** Marks the header as one this allocator wrote; see tma_realloc(). */
    uint64_t magic;
} pmix_gds_shmem3_tma_alloc_t;

/** Distinguishes our header from whatever else a bad pointer points at. */
#define PMIX_GDS_SHMEM3_TMA_ALLOC_MAGIC 0x504d495833544d41ULL // "PMIX3TMA"

/**
 * Holds allocation context information.
 */
typedef struct {
    pmix_object_t super;
    /** Handle to shared-memory backing store. */
    pmix_shmem_t *shmem3;
    /** Points to a value that maintains the next available address. */
    void **data_ptr;
} pmix_gds_shmem3_alloc_ctx_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_alloc_ctx_t);

static void
shmem3_allocator_construct(
    pmix_gds_shmem3_alloc_ctx_t *a
) {
    a->shmem3 = NULL;
    a->data_ptr = NULL;
}

static void
shmem3_allocator_destruct(
    pmix_gds_shmem3_alloc_ctx_t *a
) {
    a->shmem3 = NULL;
    a->data_ptr = NULL;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_alloc_ctx_t,
    pmix_object_t,
    shmem3_allocator_construct,
    shmem3_allocator_destruct
);

/**
 * Architecture-specific address alignment.
 */
static inline void *
addr_align(
    void *base,
    size_t size
) {
#if 0 // Helpful debug
    PMIX_GDS_SHMEM3_VVOUT("------------------------ADDRINN=%p,%zd", base, size);
#endif
    void *const res = (void *)(((uintptr_t)base + size + 7) & ~(uintptr_t)0x07);
#if 0 // Helpful debug
    // Make sure that it's 8-byte aligned.
    assert ((uintptr_t)res % 8 == 0);
    PMIX_GDS_SHMEM3_VVOUT("------------------------ADDROUT=%p,%zd", res, size);
#endif
    return res;
}

static inline pmix_gds_shmem3_alloc_ctx_t *
tma_get_alloc_ctx(
    pmix_tma_t *tma
) {
    return tma->data_context;
}

static inline void *
tma_get_curraddr(
    pmix_tma_t *tma
) {
    return *(tma_get_alloc_ctx(tma)->data_ptr);
}

static inline void
tma_set_curraddr(
    pmix_tma_t *tma,
    void *newaddr
) {
    *(tma_get_alloc_ctx(tma)->data_ptr) = newaddr;
}

static inline bool
tma_alloc_request_will_overflow(
    pmix_tma_t *tma,
    size_t alloc_size
) {
    const pmix_gds_shmem3_alloc_ctx_t *const ctx = tma_get_alloc_ctx(tma);
    const pmix_shmem_t *const backing_store = ctx->shmem3;

    const uintptr_t hdr_baseptr = (uintptr_t)backing_store->hdr_address;
    const uintptr_t data_baseptr = (uintptr_t)backing_store->data_address;
    const uintptr_t data_ptr_pos = (uintptr_t)tma_get_curraddr(tma);
    // Size of 'lost capacity` because of segment header.
    const size_t lost_capacity = (size_t)(data_baseptr - hdr_baseptr);
    const size_t bytes_used = (size_t)(data_ptr_pos - data_baseptr);

    // The sum is what gets compared, so a request large enough to wrap it
    // would pass the test and the bump allocator would then hand out
    // addresses past the end of the segment - silently, into memory
    // clients have mapped. Treat a wrap as the overflow it is.
    bool wo = (bytes_used + alloc_size) < bytes_used ||
              (bytes_used + alloc_size) > (backing_store->size - lost_capacity);

    if (PMIX_UNLIKELY(wo)) {
        errno = ENOMEM;
        perror(EMSG_SHMEM3_OOM);
        abort();
    }
    return wo;
}

/**
 * Returns the bookkeeping header for a block this allocator handed out.
 */
static inline pmix_gds_shmem3_tma_alloc_t *
tma_alloc_header(
    void *ptr
) {
    return (pmix_gds_shmem3_tma_alloc_t *)
        ((char *)ptr - sizeof(pmix_gds_shmem3_tma_alloc_t));
}

/**
 * Carves a block of the requested size out of the segment, stamping its
 * header. The caller owns initializing the returned storage.
 *
 * Nothing zeroes the block, and nothing needs to: pmix_shmem_segment_create()
 * truncates its backing file from empty, so every page of a fresh segment
 * reads as zero and every block this hands out comes from space no caller has
 * yet touched. Writing zeros over zeros only forced the whole segment
 * resident up front - the largest avoidable cost in building one - instead of
 * letting demand paging bring in what is actually used. If that guarantee
 * ever weakens, restore the memset here rather than at the call sites.
 */
static inline void *
tma_carve(
    pmix_tma_t *tma,
    size_t size
) {
    const size_t hdrsize = sizeof(pmix_gds_shmem3_tma_alloc_t);
    // The header comes out of the segment too, so it is part of the request
    // as far as the capacity check is concerned. Adding it must not be what
    // wraps the sum.
    if (PMIX_UNLIKELY(SIZE_MAX - hdrsize < size)) {
        errno = ENOMEM;
        perror(EMSG_SHMEM3_OOM);
        abort();
    }
    if (PMIX_UNLIKELY(tma_alloc_request_will_overflow(tma, size + hdrsize))) {
        return NULL;
    }
    pmix_gds_shmem3_tma_alloc_t *const hdr = tma_get_curraddr(tma);
    hdr->extent = size;
    hdr->magic = PMIX_GDS_SHMEM3_TMA_ALLOC_MAGIC;

    void *const base = (char *)hdr + hdrsize;
    tma_set_curraddr(tma, addr_align(base, size));
    return base;
}

static inline void *
tma_malloc(
    pmix_tma_t *tma,
    size_t size
) {
    if (0 == size) {
        return NULL;
    }
    return tma_carve(tma, size);
}

static inline void *
tma_calloc(
    struct pmix_tma *tma,
    size_t nmemb,
    size_t size
) {
    // Same reasoning as tma_alloc_request_will_overflow(): a product that
    // wraps produces a small allocation for a large request.
    if (0 != nmemb && SIZE_MAX / nmemb < size) {
        errno = ENOMEM;
        perror(EMSG_SHMEM3_OOM);
        abort();
    }
    const size_t real_size = nmemb * size;
    if (0 == real_size) {
        return NULL;
    }
    // See tma_carve(): a fresh segment is already zero.
    return tma_carve(tma, real_size);
}

static inline void *
tma_realloc(
    pmix_tma_t *tma,
    void *ptr,
    size_t new_size
) {
    // Behave like malloc
    if (NULL == ptr) {
        return tma_malloc(tma, new_size);
    }
    // Behave like free
    if (0 == new_size) {
        pmix_tma_free(tma, ptr);
        return NULL;
    }
    // Every block we hand out carries its size just ahead of it. A pointer
    // that does not is one this allocator never produced, which means the
    // caller has crossed a heap block with a segment block - keep saying so
    // rather than copying from an extent we invented.
    pmix_gds_shmem3_tma_alloc_t *const hdr = tma_alloc_header(ptr);
    if (PMIX_UNLIKELY(PMIX_GDS_SHMEM3_TMA_ALLOC_MAGIC != hdr->magic)) {
        errno = EFAULT;
        perror(EMSG_SHMEM3_IS_BROKEN);
        abort();
    }
    const size_t old_size = hdr->extent;
    if (new_size != old_size) {
        void *new_base = pmix_tma_malloc(tma, new_size);
        if (NULL == new_base) {
            return ptr;
        }
        // Move min(new_size, old_size) into new space.
        memmove(new_base, ptr, new_size < old_size ? new_size : old_size);
        pmix_tma_free(tma, ptr);
        return new_base;
    }
    return ptr;
}

static inline char *
tma_strdup(
    pmix_tma_t *tma,
    const char *s
) {
    const size_t size = strlen(s) + 1;

    void *const base = tma_carve(tma, size);
    if (PMIX_UNLIKELY(NULL == base)) {
        return NULL;
    }
    return (char *)memmove(base, s, size);
}

static inline void
tma_free(
    struct pmix_tma *tma,
    void *ptr
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr);
    // We don't do anything for free.
}

static void
tma_init_function_pointers(
    pmix_tma_t *tma
) {
    tma->tma_malloc = tma_malloc;
    tma->tma_calloc = tma_calloc;
    tma->tma_realloc = tma_realloc;
    tma->tma_strdup = tma_strdup;
    tma->tma_free = tma_free;
}

static void
tma_init(
    pmix_shmem_t *shmem3_backing_store,
    pmix_tma_t *tma,
    void *data_ptr
) {
    // Only available in the allocator's address space.
    pmix_gds_shmem3_alloc_ctx_t *ctx = PMIX_NEW(pmix_gds_shmem3_alloc_ctx_t);

    tma_init_function_pointers(tma);
    tma->data_context = (void *)ctx;

    ctx->shmem3 = shmem3_backing_store;
    ctx->data_ptr = data_ptr;
}

static void
host_alias_construct(
    pmix_gds_shmem3_host_alias_t *a
) {
    a->name = NULL;
}

static void
host_alias_destruct(
    pmix_gds_shmem3_host_alias_t *a
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&a->super.super);
    if (a->name) {
        pmix_tma_free(tma, a->name);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_host_alias_t,
    pmix_list_item_t,
    host_alias_construct,
    host_alias_destruct
);

static void
nodeinfo_construct(
    pmix_gds_shmem3_nodeinfo_t *n
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&n->super.super);

    n->nodeid = UINT32_MAX;
    n->hostname = NULL;
    n->aliases = PMIX_NEW(pmix_list_t, tma);
    n->info = PMIX_NEW(pmix_list_t, tma);
}

static void
nodeinfo_destruct(
    pmix_gds_shmem3_nodeinfo_t *n
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
    pmix_gds_shmem3_nodeinfo_t,
    pmix_list_item_t,
    nodeinfo_construct,
    nodeinfo_destruct
);

static void
job_construct(
    pmix_gds_shmem3_job_t *job
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    // Backing store ownership
    job->uid = geteuid();
    job->gid = getegid();
    job->chown = false;
    job->chgrp = false;
    // Namespace identification
    job->nspace_id = NULL;
    job->nspace = NULL;
    // Session
    job->session = PMIX_NEW(pmix_gds_shmem3_session_t);
    // Job
    job->shmem3_status = 0;
    job->shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smdata = NULL;
    // Modex
    job->modex_shmem3_status = 0;
    job->modex_generation = 0;
    job->modex_shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
    job->modex_is_delta = false;
    PMIX_CONSTRUCT(&job->modex_prior, pmix_list_t);
    // Connection info
    job->conni = NULL;
}

static pmix_tma_t *
get_tma_by_shmem3_id(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id
) {
    switch (shmem3_id) {
        case PMIX_GDS_SHMEM3_JOB_ID:
            return &job->smdata->tma;
        case PMIX_GDS_SHMEM3_MODEX_ID:
            return &job->smmodex->tma;
        case PMIX_GDS_SHMEM3_SESSION_ID:
            return &job->session->smdata->tma;
        case PMIX_GDS_SHMEM3_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
            // This is an internal error.
            abort();
            return NULL;
    }
}

static const char *
get_shmem3_id_name(
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id
) {
    switch (shmem3_id) {
        case PMIX_GDS_SHMEM3_JOB_ID:
            return "smdata";
        case PMIX_GDS_SHMEM3_MODEX_ID:
            return "smmodex";
        case PMIX_GDS_SHMEM3_SESSION_ID:
            return "smsession";
        case PMIX_GDS_SHMEM3_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
            // This is an internal error.
            abort();
            return NULL;
    }
}

static void
emit_shmem3_usage_stats(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem3;
    rc = pmix_gds_shmem3_get_job_shmem3_by_id(
        job, shmem3_id, &shmem3
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    pmix_tma_t *tma = get_tma_by_shmem3_id(job, shmem3_id);
    const char *smname = get_shmem3_id_name(shmem3_id);

    const size_t shmem3_size = shmem3->size;
    const size_t bytes_used = (size_t)((uintptr_t)tma_get_curraddr(tma)
                            - (uintptr_t)shmem3->data_address);
    const float utilization = (bytes_used / (float)shmem3_size) * 100.0;

    PMIX_GDS_SHMEM3_VOUT(
        "%s memory statistics: "
        "segment size=%zd, bytes used=%zd, utilization=%.2f %%",
        smname, shmem3_size, bytes_used, utilization
    );
}

static void
job_destruct(
    pmix_gds_shmem3_job_t *job
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
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

    /* Retired modex generations. Each holds its own segment handle, and
     * the class destructor gives it back the same way the loop below
     * does for the current one. */
    PMIX_LIST_DESTRUCT(&job->modex_prior);

    static const pmix_gds_shmem3_job_shmem3_id_t shmem3_ids[] = {
        PMIX_GDS_SHMEM3_JOB_ID,
        PMIX_GDS_SHMEM3_MODEX_ID,
        PMIX_GDS_SHMEM3_SESSION_ID,
        PMIX_GDS_SHMEM3_INVALID_ID
    };
    for (int i = 0; shmem3_ids[i] != PMIX_GDS_SHMEM3_INVALID_ID; ++i) {
        const pmix_gds_shmem3_job_shmem3_id_t sid = shmem3_ids[i];

        pmix_shmem_t *shmem3;
        rc = pmix_gds_shmem3_get_job_shmem3_by_id(job, sid, &shmem3);
        if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        if (pmix_gds_shmem3_has_status(job, sid, PMIX_GDS_SHMEM3_MINE)) {
            // Emit usage status before we potentially destroy the segment.
            emit_shmem3_usage_stats(job, sid);
            // Points to a pmix_gds_shmem3_alloc_ctx_t.
            PMIX_RELEASE(get_tma_by_shmem3_id(job, sid)->data_context);
        }
        // Releases memory for the structures located in shared-memory. This
        // will also unmap in case we need to later remap something in the
        // address space covered by this.
        PMIX_RELEASE(shmem3);
        // Invalidate the shmem3 flags.
        pmix_gds_shmem3_clearall_status(job, sid);
    }

    if (job->session) {
        PMIX_RELEASE(job->session);
    }
}

/**
 * Let go of this job's current modex segment.
 *
 * Releasing our handle does not pull the segment out from under anyone:
 * the backing file carries a reference count, so a client that has it
 * mapped keeps a valid mapping and the file survives until the last
 * holder lets go. That is what makes handing one generation off and
 * starting the next safe.
 *
 * Mirrors what job_destruct() does for this segment; keep the two in
 * step. Order matters - the allocator context is reached through
 * job->smmodex, so it has to go before that pointer is cleared.
 */
static void
modex_seg_construct(
    pmix_gds_shmem3_modex_seg_t *seg
) {
    seg->status = 0;
    seg->shmem3 = NULL;
    seg->smmodex = NULL;
}

static void
modex_seg_destruct(
    pmix_gds_shmem3_modex_seg_t *seg
) {
    if (NULL == seg->shmem3) {
        return;
    }
    /* Mirrors what release_modex_segment() does for the current
     * generation - keep the two in step. Order matters: the allocator
     * context is reached through smmodex, so it goes first. */
    if (PMIX_GDS_SHMEM3_MINE & seg->status) {
        if (NULL != seg->smmodex) {
            PMIX_RELEASE(seg->smmodex->tma.data_context);
        }
    }
    PMIX_RELEASE(seg->shmem3);
    seg->shmem3 = NULL;
    seg->smmodex = NULL;
    seg->status = 0;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_modex_seg_t,
    pmix_list_item_t,
    modex_seg_construct,
    modex_seg_destruct
);

/**
 * Set the current modex generation aside instead of letting it go.
 *
 * A delta contribution holds only what changed, so the generation before
 * it is still the only copy of everything the delta did not repeat. Move
 * it to the head of job->modex_prior - newest first, which is the order
 * a read walks - and leave this job ready to create the next one.
 *
 * The backing file is reference counted, so a client still reading the
 * retired generation is undisturbed either way; what this changes is
 * that *we* keep our handle, and therefore keep answering out of it.
 */
static pmix_status_t
retire_modex_segment(
    pmix_gds_shmem3_job_t *job
) {
    if (!pmix_gds_shmem3_has_status(job, PMIX_GDS_SHMEM3_MODEX_ID,
                                    PMIX_GDS_SHMEM3_ATTACHED)) {
        return PMIX_SUCCESS;
    }
    pmix_gds_shmem3_modex_seg_t *seg = PMIX_NEW(pmix_gds_shmem3_modex_seg_t);
    if (PMIX_UNLIKELY(NULL == seg)) {
        return PMIX_ERR_NOMEM;
    }
    seg->status = job->modex_shmem3_status;
    seg->shmem3 = job->modex_shmem3;
    seg->smmodex = job->smmodex;
    pmix_list_prepend(&job->modex_prior, &seg->super);

    job->modex_shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
    pmix_gds_shmem3_clearall_status(job, PMIX_GDS_SHMEM3_MODEX_ID);
    return PMIX_SUCCESS;
}

/**
 * Let go of every retired modex generation.
 *
 * A cumulative contribution repeats everything its process has published,
 * so the generation carrying it stands on its own and nothing older can
 * still be the only copy of anything.
 */
static void
drop_modex_priors(
    pmix_gds_shmem3_job_t *job
) {
    pmix_gds_shmem3_modex_seg_t *seg;

    while (NULL != (seg = (pmix_gds_shmem3_modex_seg_t *)
                          pmix_list_remove_first(&job->modex_prior))) {
        PMIX_RELEASE(seg);
    }
}

static void
release_modex_segment(
    pmix_gds_shmem3_job_t *job
) {
    if (!pmix_gds_shmem3_has_status(job, PMIX_GDS_SHMEM3_MODEX_ID,
                                    PMIX_GDS_SHMEM3_ATTACHED)) {
        return;
    }
    if (pmix_gds_shmem3_has_status(job, PMIX_GDS_SHMEM3_MODEX_ID,
                                   PMIX_GDS_SHMEM3_MINE)) {
        emit_shmem3_usage_stats(job, PMIX_GDS_SHMEM3_MODEX_ID);
        PMIX_RELEASE(get_tma_by_shmem3_id(job, PMIX_GDS_SHMEM3_MODEX_ID)->data_context);
    }
    PMIX_RELEASE(job->modex_shmem3);
    job->modex_shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
    pmix_gds_shmem3_clearall_status(job, PMIX_GDS_SHMEM3_MODEX_ID);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_job_t,
    pmix_list_item_t,
    job_construct,
    job_destruct
);

static void
app_construct(
    pmix_gds_shmem3_app_t *a
) {
    pmix_tma_t *const tma = pmix_obj_get_tma(&a->super.super);

    a->appnum = 0;
    a->appinfo = PMIX_NEW(pmix_list_t, tma);
    a->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    a->job = NULL;
}

static void
app_destruct(
    pmix_gds_shmem3_app_t *a
) {
    if (a->appinfo) {
        PMIX_LIST_DESTRUCT(a->appinfo);
    }
    if (a->nodeinfo) {
        PMIX_LIST_DESTRUCT(a->nodeinfo);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_app_t,
    pmix_list_item_t,
    app_construct,
    app_destruct
);

static void
session_construct(
    pmix_gds_shmem3_session_t *s
) {
    s->shmem3 = PMIX_NEW(pmix_shmem_t);
    s->shmem3_status = 0;
    s->smdata = NULL;
}

static void
session_destruct(
    pmix_gds_shmem3_session_t *s
) {
    // job_destruct took care of our shmem3.
    s->shmem3 = NULL;
    // Invalidate the shmem3 flags.
    s->shmem3_status = 0;
    s->smdata = NULL;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem3_session_t,
    pmix_list_item_t,
    session_construct,
    session_destruct
);

static pmix_status_t
session_smdata_construct(
    pmix_gds_shmem3_job_t *job,
    uint32_t sid
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // session know about its data located at the base of the segment.
    const size_t smdata_size = sizeof(*job->session->smdata);
    void *const baseaddr = job->session->shmem3->data_address;

    job->session->smdata = baseaddr;
    memset(job->session->smdata, 0, smdata_size);
    // Save the starting address for TMA memory allocations.
    job->session->smdata->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(
        job->session->shmem3,
        &job->session->smdata->tma,
        &job->session->smdata->current_addr
    );
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    tma_set_curraddr(&job->session->smdata->tma, addr_align(baseaddr, smdata_size));
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
    pmix_gds_shmem3_vout_smsession(job->session);
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
    pmix_gds_shmem3_job_t *job,
    size_t htsize
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its data located at the base of the segment.
    const size_t smdata_size = sizeof(*job->smdata);
    void *const baseaddr = job->shmem3->data_address;

    job->smdata = baseaddr;
    memset(job->smdata, 0, smdata_size);
    // Save the starting address for TMA memory allocations.
    job->smdata->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(job->shmem3, &job->smdata->tma, &job->smdata->current_addr);
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    tma_set_curraddr(&job->smdata->tma, addr_align(baseaddr, smdata_size));
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
    // The indices in that table are meaningless without the translation,
    // and the translation cannot be the process-global one because
    // clients do not share our numbering. Build it in the segment so it
    // travels with the data it describes.
    job->smdata->keyindex = PMIX_NEW(pmix_keyindex_t, tma);
    if (!job->smdata->keyindex) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        goto out;
    }

    pmix_gds_shmem3_vout_smdata(job);
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
        if (job->smdata->keyindex) {
            PMIX_RELEASE(job->smdata->keyindex);
        }
    }
    return rc;
}

static pmix_status_t
modex_smdata_construct(
    pmix_gds_shmem3_job_t *job,
    size_t htsize
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its data located at the base of the segment.
    const size_t smmodex_size = sizeof(*job->smmodex);
    void *const baseaddr = job->modex_shmem3->data_address;

    job->smmodex = baseaddr;
    memset(job->smmodex, 0, smmodex_size);
    // Save the starting address for TMA memory allocations.
    job->smmodex->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(job->modex_shmem3, &job->smmodex->tma, &job->smmodex->current_addr);
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    tma_set_curraddr(&job->smmodex->tma, addr_align(baseaddr, smmodex_size));
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

    // The indices stored in that table are meaningless without the
    // translation, and the translation cannot be the process-global one
    // because clients do not share our numbering. Build it here, in the
    // segment, so it travels with the data it describes.
    job->smmodex->keyindex = PMIX_NEW(pmix_keyindex_t, tma);
    if (!job->smmodex->keyindex) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_gds_shmem3_vout_smmodex(job);

    return rc;
}

/**
 * Returns the base temp directory.
 */
static inline const char *
fetch_base_tmpdir(
    pmix_gds_shmem3_job_t *job
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
        // Get a pointer to the first item in the list. These used to be
        // assert()s, which compile away in the builds anyone ships - so
        // the checks were absent exactly where a wrong answer would be
        // dereferenced rather than caught.
        pmix_kval_t *kv = (pmix_kval_t *)pmix_list_get_first(&cb.kvs);
        if (NULL == kv || NULL == kv->value ||
            PMIX_STRING != kv->value->type ||
            NULL == kv->value->data.string) {
            PMIX_DESTRUCT(&cb);
            continue;
        }
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
get_shmem3_backing_path(
    pmix_gds_shmem3_job_t *job,
    const char *id
) {
    static char path[PMIX_PATH_MAX] = {'\0'};
    const char *basedir = fetch_base_tmpdir(job);
    // Now that we have the base path, append unique name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/%s-gds-%s.%s-%s.%s.%d",
        basedir, PACKAGE_NAME, PMIX_GDS_SHMEM3_NAME,
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
get_shmem3_session_name(
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
shmem3_attach(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    uintptr_t req_addr
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem3;
    rc = pmix_gds_shmem3_get_job_shmem3_by_id(
        job, shmem3_id, &shmem3
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = pmix_shmem_segment_attach(
        shmem3, req_addr, PMIX_SHMEM_MUST_MAP_AT_RADDR,
        PMIX_GDS_SHMEM3_LAYOUT_ID
    );
    if (PMIX_UNLIKELY(pmix_gds_shmem3_force_client_attach_failure)) {
        // Testing only: pretend the fixed-address attach failed so the
        // client exercises the GDS fallback path. The shared "out" cleanup
        // below detaches the segment if the real attach actually succeeded.
        rc = PMIX_ERR_NOT_AVAILABLE;
    }
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        // We were given a fixed base address but could not map the segment
        // there in this process's address space -- its VM layout (ASLR,
        // what else is already mapped) differs from the process that chose
        // the address. This is not fatal: signal the framework to fall
        // back to the next GDS module (e.g. hash) for this client instead
        // of aborting PMIx_Init.
        if (PMIX_ERR_NOT_AVAILABLE == rc) {
            PMIX_GDS_SHMEM3_VOUT(
                "%s: could not attach segment at required address 0x%zx; "
                "falling back to the next GDS module",
                __func__, (size_t)req_addr
            );
            rc = PMIX_ERR_TAKE_NEXT_OPTION;
        }
        else if (PMIX_ERR_NOT_SUPPORTED == rc) {
            // The segment was written by a process that lays these
            // structures out differently than we do - most often a
            // --enable-debug build talking to a default one, since that
            // moves obj_magic_id into the front of pmix_object_t. Reading
            // it would put every field at the wrong offset, so decline the
            // segment and let this client use hash instead.
            PMIX_GDS_SHMEM3_VOUT(
                "%s: segment layout does not match ours (expected 0x%08x); "
                "falling back to the next GDS module",
                __func__, (unsigned)PMIX_GDS_SHMEM3_LAYOUT_ID
            );
            rc = PMIX_ERR_TAKE_NEXT_OPTION;
        }
        goto out;
    }
    PMIX_GDS_SHMEM3_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)shmem3->hdr_address
    );
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(shmem3);
        // remove the job from the tracker
        pmix_list_remove_item(&pmix_mca_gds_shmem3_component.jobs, &job->super);
        PMIX_RELEASE(job);
    }
    else {
        pmix_gds_shmem3_set_status(
            job, shmem3_id, PMIX_GDS_SHMEM3_ATTACHED
        );
    }
    return rc;
}

static inline pmix_status_t
init_client_side_sm_data(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id
) {
    switch (shmem3_id) {
        case PMIX_GDS_SHMEM3_JOB_ID:
            job->smdata = job->shmem3->data_address;
            pmix_gds_shmem3_vout_smdata(job);
            break;
        case PMIX_GDS_SHMEM3_SESSION_ID:
            job->session->smdata = job->session->shmem3->data_address;
            pmix_gds_shmem3_vout_smsession(job->session);
            break;
        case PMIX_GDS_SHMEM3_MODEX_ID:
            job->smmodex = job->modex_shmem3->data_address;
            pmix_gds_shmem3_vout_smmodex(job);
            break;
        case PMIX_GDS_SHMEM3_INVALID_ID:
        default:
            PMIX_ERROR_LOG(PMIX_ERROR);
            abort();
            return PMIX_ERROR;
    }
    // Segment is ready for use by the client.
    pmix_gds_shmem3_set_status(job, shmem3_id, PMIX_GDS_SHMEM3_READY_FOR_USE);
    // Note: don't update the TMA to point to its local function pointers
    // because clients should only be reading from the shared-memory segment.
    return PMIX_SUCCESS;
}

static pmix_status_t
shmem3_segment_attach_and_init(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_unpacked_seg_blob_t *seginfo
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_shmem_t *shmem3;
    rc = pmix_gds_shmem3_get_job_shmem3_by_id(
        job, seginfo->smid, &shmem3
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Initialize the segment path.
    const size_t buffmax = sizeof(shmem3->backing_path);
    pmix_string_copy(shmem3->backing_path, seginfo->seg_path, buffmax);
    // Initialize the segment size.
    shmem3->size = seginfo->seg_size;

    const uintptr_t req_addr = (uintptr_t)seginfo->seg_hadr;
    rc = shmem3_attach(job, seginfo->smid, req_addr);
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        return rc;
    }
    // Now we can safely initialize our shared data structures.
    rc = init_client_side_sm_data(job, seginfo->smid);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        return rc;
    }
    /* We are a reader of this segment, so take write access away and let
     * the MMU keep us honest. Everything here is either read in place or
     * copied out; the one thing that used to write was a hash-table
     * lookup recording the table's key type, which no longer does so for
     * a TMA table.
     *
     * This is worth more than the assertion it replaces. A stray write
     * from a reader does not hurt the reader - it corrupts a structure
     * some *other* process is walking, and surfaces there, later, as
     * something that looks nothing like its cause. Protected, it is a
     * SIGSEGV on the instruction responsible.
     *
     * A failure here is not fatal: the segment is perfectly readable
     * either way, we simply do not get the guard. Say so and carry on
     * rather than failing an attach that otherwise succeeded. */
    if (PMIX_SUCCESS != pmix_shmem_segment_protect_data(shmem3)) {
        PMIX_GDS_SHMEM3_VOUT(
            "%s: could not drop write access to %s (continuing read-write)",
            __func__, shmem3->backing_path
        );
    }
    return rc;
}

/**
 * Updates backing file permissions based on PMIx directives.
 */
static pmix_status_t
shmem3_segment_fix_perms(
    pmix_gds_shmem3_job_t *job,
    pmix_shmem_t *shmem3
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Update segment ownership and permissions?
    if (job->chown || job->chgrp) {
        const uid_t uid = job->chown ? job->uid : (uid_t)-1;
        const gid_t gid = job->chgrp ? job->gid : (gid_t)-1;

        rc = pmix_shmem_segment_chown(shmem3, uid, gid);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }

        rc = pmix_shmem_segment_chmod(
            shmem3, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
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
shmem3_segment_create_and_attach(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    const char *segment_name,
    size_t segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Pad given size to fill remaining space on the last page.
    const size_t real_segsize = pmix_shmem_utils_pad_to_page(segment_size);
    // Find a unique path for the shared-memory backing file.
    const char *segment_path = get_shmem3_backing_path(job, segment_name);
    if (PMIX_UNLIKELY(!segment_path)) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    PMIX_GDS_SHMEM3_VOUT(
        "%s: segment backing file path is %s (size=%zd B)",
        __func__, segment_path, real_segsize
    );
    // Get a handle to the appropriate shmem3.
    pmix_shmem_t *shmem3;
    rc = pmix_gds_shmem3_get_job_shmem3_by_id(job, shmem3_id, &shmem3);
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(
        shmem3, real_segsize, segment_path, PMIX_GDS_SHMEM3_LAYOUT_ID
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Find a hole in virtual memory and attach the segment there.
    //
    // The hole is located by scanning /proc/self/maps and is then claimed by a
    // separate mmap(); those two steps are not atomic. PMIx runs its internal
    // work on a single progress thread, so PMIx never races itself here -- but
    // the address space is process-wide, so other activity in the process can
    // take the located hole in between: another thread, the dynamic loader
    // (dlopen as components/namespaces come up), the C allocator growing an
    // arena via mmap, or AddressSanitizer's own mappings. This is rare in
    // general but considerably more likely under ASAN and during
    // MPI_Comm_spawn. Because the server chooses this address (clients are
    // later told whichever one we land on), recover from a lost hole by
    // selecting a new one and retrying rather than failing the spawned job's
    // PMIx_Init.
    //
    // Bypass shmem3_attach() here: on a failed map it reports a client-style
    // address mismatch and tears the job down, which is wrong for the server,
    // which chose the address and can simply try another hole.
    enum { MAX_ATTACH_ATTEMPTS = 16 };
    for (int attempt = 1; ; ++attempt) {
        size_t base_addr = 0;
        rc = pmix_vmem_find_hole(
            VMEM_HOLE_BIGGEST, &base_addr, real_segsize
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto out;
        }
        PMIX_GDS_SHMEM3_VOUT(
            "%s: %s found vmhole at address=0x%zx (attempt %d)",
            __func__, segment_name, base_addr, attempt
        );
        rc = pmix_shmem_segment_attach(
            shmem3, (uintptr_t)base_addr, PMIX_SHMEM_MUST_MAP_AT_RADDR,
            PMIX_GDS_SHMEM3_LAYOUT_ID
        );
        if (PMIX_LIKELY(PMIX_SUCCESS == rc)) {
            break;
        }
        // Only the "could not map at the requested address" case is retryable;
        // pmix_shmem_segment_attach() already detached its failed attempt.
        if (PMIX_ERR_NOT_AVAILABLE != rc || attempt >= MAX_ATTACH_ATTEMPTS) {
            PMIX_ERROR_LOG(rc);
            goto out_release;
        }
    }
    pmix_gds_shmem3_set_status(job, shmem3_id, PMIX_GDS_SHMEM3_ATTACHED);
    // Fix-up backing file permission.
    rc = shmem3_segment_fix_perms(job, shmem3);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
out:
    if (PMIX_SUCCESS == rc) {
        // I created it, so note that it is mine.
        pmix_gds_shmem3_set_status(
            job, shmem3_id, PMIX_GDS_SHMEM3_MINE
        );
    }
    return rc;
out_release:
    // Mirror shmem3_attach()'s failure handling: detach and drop the job
    // tracker entry.
    (void)pmix_shmem_segment_detach(shmem3);
    pmix_list_remove_item(&pmix_mca_gds_shmem3_component.jobs, &job->super);
    PMIX_RELEASE(job);
    return rc;
}

static pmix_status_t
module_init(
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    PMIX_CONSTRUCT(&pmix_mca_gds_shmem3_component.jobs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem3_component.joblock, pmix_mutex_t);
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem3_component.sessions, pmix_list_t);
    return PMIX_SUCCESS;
}

static void
module_finalize(void)
{
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem3_component.sessions);
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem3_component.jobs);
}

static pmix_status_t
assign_module(
    pmix_info_t *info,
    size_t ninfo,
    int *priority
) {
    static const int max_priority = 100;
    *priority = PMIX_GDS_SHMEM3_DEFAULT_PRIORITY;
    // The incoming info always overrides anything in the
    // environment as it is set by the application itself.
    bool specified = false;
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GDS_MODULE)) {
            char **options = NULL;
            // The directive comes from the application or the environment,
            // so it may be malformed - a non-string value, or a string
            // that splits to nothing. Neither names us, but neither is a
            // request for somebody else either, so keep the default bid.
            if (PMIX_STRING != info[n].value.type ||
                NULL == info[n].value.data.string) {
                break;
            }
            options = PMIx_Argv_split(info[n].value.data.string, ',');
            if (NULL == options) {
                break;
            }
            specified = true; // They specified who they want.
            for (size_t m = 0; NULL != options[m]; m++) {
                if (0 == strcmp(options[m], PMIX_GDS_SHMEM3_NAME)) {
                    // They specifically asked for us.
                    *priority = max_priority;
                    break;
                }
            }
            PMIx_Argv_free(options);
            break;
        }
    }
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
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    // We don't support this operation.
    return PMIX_ERR_NOT_SUPPORTED;
}

/**
 *
 */
static pmix_status_t
prepare_shmem3_stores_for_local_job_data(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_packed_local_job_info_t *pji
) {
    pmix_status_t rc = PMIX_SUCCESS;
    static const float fluff = 3.0;
    const size_t kvsize = (sizeof(pmix_kval_t) + sizeof(pmix_value_t));
    // Elements the table needs - one per rank it will hold.
    const size_t htsize = pji->hash_table_size;
    // Key/value pairs those elements hold between them. The table is
    // sized by the first number and its contents by this one; they are
    // not interchangeable, and using htsize for both is what made the
    // table thousands of times larger than it had to be.
    const size_t nkvals = pji->nkvals;
    // Calculate a rough estimate on the amount of storage required to store the
    // values associated with the pmix_gds_shmem3_shared_job_data_t. Err on the
    // side of overestimation.
    size_t seg_size = sizeof(*job->smdata);
    // We need to store a hash table in the shared-memory segment, so calculate
    // a rough estimate on the memory required for its storage.
    seg_size += sizeof(pmix_hash_table_t);
    seg_size += pmix_hash_table_sizeof_storage(htsize);
    // Each element points at a per-rank structure with its own arrays.
    seg_size += htsize * pmix_hash_sizeof_proc_storage();
    // Add a little extra to compensate for the value storage requirements. Here
    // we add an additional storage space for each key/value pair.
    seg_size += nkvals * kvsize;
    /* The keyindex that translates the indices in that table lives in
     * this segment too, and an empty one is already ~170 KB. On top of
     * that each distinct key costs a record and three copies of its own
     * string. Bound the entry count by nkvals - the real number of
     * distinct keys is normally far smaller.
     *
     * The key *lengths* are bounded by the data, not by PMIX_MAX_KEYLEN:
     * every one of these keys arrived in the buffer we just packed, so
     * their strings cannot total more than it holds. Reserving the
     * 511-byte maximum for each instead made this term alone dwarf the
     * payload it was protecting.
     *
     * Leaving this out is not a slow leak, it is a hard failure: the
     * TMA behind the segment is a bump allocator with nowhere to grow,
     * so the server aborts partway through register_job_info() and
     * every client sits waiting for job data that will never arrive. */
    seg_size += pmix_keyindex_sizeof_fixed_storage();
    seg_size += nkvals * pmix_hash_sizeof_key_entry(0);
    /* Three copies per registered key, plus the copy newkval() strdups
     * for every value that lands on a list rather than in the table. */
    seg_size += 4 * pji->packed_size;
    // Finally add the data size contribution, plus a little extra.
    seg_size += pji->packed_size;
    // Include some extra fluff that empirically seems reasonable.
    seg_size *= fluff;
    // Adjust (increase or decrease) segment size by the given parameter size.
    seg_size *= pmix_gds_shmem3_segment_size_multiplier;
    // Create and attach to the shared-memory segment associated with this job.
    // This will be the backing store for data associated with static, read-only
    // data shared between the server and its clients.
    rc = shmem3_segment_create_and_attach(
        job, PMIX_GDS_SHMEM3_JOB_ID, "jobdata", seg_size
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Do the same for the job's session information. Note that we recycle the
    // segment size calculated above because we know that it will be at least as
    // big as we need for this session information.
    const char *session_name = get_shmem3_session_name(pji->session_id);
    if (PMIX_UNLIKELY(!session_name)) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = shmem3_segment_create_and_attach(
        job, PMIX_GDS_SHMEM3_SESSION_ID, session_name, seg_size
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
pack_shmem3_connection_info(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM3_VVOUT(
        "%s:%s for peer (ID=%d) namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        peer->info->peerid, job->nspace_id
    );

    pmix_shmem_t *shmem3;
    rc = pmix_gds_shmem3_get_job_shmem3_by_id(
        job, shmem3_id, &shmem3
    );
    if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_kval_t kv;
    do {
        // Pack the namespace name.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_NSID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(job->nspace_id);
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack the shmem3 ID as string.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_SMID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        int nw = pmix_asprintf(&kv.value->data.string, "%zd", (size_t)shmem3_id);
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
        kv.key = strdup(SHMEM3_SEG_PATH_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(shmem3->backing_path);
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        PMIX_DESTRUCT(&kv);
        // Pack attach size to shared-memory segment.
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_SIZE_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        nw = pmix_asprintf(&kv.value->data.string, "%zx", shmem3->size);
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
        kv.key = strdup(SHMEM3_SEG_HADR_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        nw = pmix_asprintf(
            &kv.value->data.string, "%zx", (size_t)shmem3->hdr_address
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
        if (PMIX_GDS_SHMEM3_MODEX_ID != shmem3_id) {
            break;
        }
        /* Say whether this modex generation stands on its own, so the
         * client makes the same keep-or-drop decision this server made.
         * Only the modex is ever republished, so only it carries this. */
        PMIX_DESTRUCT(&kv);
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_DELTA_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        kv.value->data.string = strdup(job->modex_is_delta ? "1" : "0");
        if (PMIX_UNLIKELY(NULL == kv.value->data.string)) {
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
 * Emits the contents of an pmix_gds_shmem3_unpacked_seg_blob_t.
 */
static inline void
vout_unpacked_seg_blob(
    pmix_gds_shmem3_unpacked_seg_blob_t *usb,
    const char *called_by
) {
#if (PMIX_ENABLE_DEBUG == 0)
    PMIX_HIDE_UNUSED_PARAMS(usb, called_by);
#endif
    PMIX_GDS_SHMEM3_VVOUT(
        "%s: "
        SHMEM3_SEG_NSID_KEY "=%s "
        SHMEM3_SEG_SMID_KEY "=%u "
        SHMEM3_SEG_PATH_KEY "=%s "
        SHMEM3_SEG_SIZE_KEY "=%zd "
        SHMEM3_SEG_HADR_KEY "=0x%zx",
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
unpack_shmem3_connection_info(
    pmix_kval_t *kvbo,
    pmix_gds_shmem3_unpacked_seg_blob_t *usb
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

        // Every element of a seg blob is packed as a string; anything
        // else means the blob is not one of ours, and val would not be
        // a pointer we can hand to pmix_asprintf()/strtost().
        if (PMIX_UNLIKELY(NULL == kv.value ||
                          PMIX_STRING != kv.value->type ||
                          NULL == kv.value->data.string)) {
            rc = PMIX_ERR_TYPE_MISMATCH;
            PMIX_ERROR_LOG(rc);
            break;
        }
        const char *const val = kv.value->data.string;
        if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_NSID_KEY)) {
            int nw = pmix_asprintf(&usb->nsid, "%s", val);
            if (PMIX_UNLIKELY(nw == -1)) {
                rc = PMIX_ERR_NOMEM;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_SMID_KEY)) {
            size_t st_shmem3_id;
            rc = strtost(val, 10, &st_shmem3_id);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            usb->smid = (pmix_gds_shmem3_job_shmem3_id_t)st_shmem3_id;
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_PATH_KEY)) {
            int nw = pmix_asprintf(&usb->seg_path, "%s", val);
            if (PMIX_UNLIKELY(nw == -1)) {
                rc = PMIX_ERR_NOMEM;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_SIZE_KEY)) {
            rc = strtost(val, 16, &usb->seg_size);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_HADR_KEY)) {
            rc = strtost(val, 16, &usb->seg_hadr);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_DELTA_KEY)) {
            /* only the modex carries this; absent means "stands alone" */
            usb->is_delta = ('0' != val[0]);
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

static inline pmix_status_t
get_local_job_data_info(
    pmix_cb_t *job_cb,
    pmix_gds_shmem3_packed_local_job_info_t *pji
) {
    pmix_status_t rc = PMIX_SUCCESS;
    /* Entries in job->smdata->local_hashtab. That table is keyed by RANK,
     * so what goes in it is one element per rank a PMIX_PROC_INFO_ARRAY
     * describes, plus a single element for PMIX_RANK_WILDCARD that every
     * plain job-level key shares - see
     * pmix_gds_shmem3_store_local_job_data_in_shmem3(). Counting the
     * *infos* inside each array, and one per plain key, described a table
     * that does not exist. */
    size_t nprocarrays = 0;
    bool have_job_level_kv = false;
    /* How many key/value pairs go into that table between them. This is a
     * different quantity from the element count, and the estimate below
     * needs both: the elements size the table, the pairs size the values
     * hanging off it and the key index describing them. Conflating the
     * two is what made the element count wrong in the first place. */
    size_t nkvals = 0;
    uint32_t sid = UINT32_MAX;

    pmix_buffer_t data;
    PMIX_CONSTRUCT(&data, pmix_buffer_t);

    pmix_kval_t *kvi;
    PMIX_LIST_FOREACH (kvi, &job_cb->kvs, pmix_kval_t) {
        // Calculate some statistics so we can make an educated estimate on the
        // size of structures we need for our backing store.
        if (PMIX_DATA_ARRAY == kvi->value->type &&
            NULL != kvi->value->data.darray &&
            NULL != kvi->value->data.darray->array &&
            0 != kvi->value->data.darray->size) {
            /* A PMIX_PROC_INFO_ARRAY becomes one hash-table entry: every
             * info in it is stored against the single rank the array
             * describes. */
            if (PMIX_CHECK_KEY(kvi, PMIX_PROC_INFO_ARRAY)) {
                nprocarrays += 1;
                nkvals += kvi->value->data.darray->size;
            }
            // See if this is the job's session ID. If so, capture it.
            pmix_info_t *info = (pmix_info_t *)kvi->value->data.darray->array;
            if (PMIX_CHECK_KEY(&info[0], PMIX_SESSION_ID)) {
                rc = PMIx_Value_get_number(&info[0].value, &sid, PMIX_UINT32);
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    PMIX_ERROR_LOG(rc);
                    goto out;
                }
            }
        }
        /* Just a key/value pair. They all go into the hash table under
         * PMIX_RANK_WILDCARD, so however many there are they share one
         * entry between them. */
        else {
            have_job_level_kv = true;
            nkvals += 1;
        }

        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &data, kvi, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto out;
        }
    }
    pji->session_id = sid;
    pji->packed_size = data.bytes_used;
    /* Entries, not capacity: pmix_hash_table_init() applies the density
     * ratio itself, so passing it a capacity double-applied it. */
    pji->hash_table_size = nprocarrays + (have_job_level_kv ? 1 : 0);
    pji->nkvals = nkvals;
out:
    PMIX_DESTRUCT(&data);
    return rc;
}

static inline pmix_status_t
pack_shmem3_seg_blob(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    struct pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Only pack connection info that is ready for use. Otherwise,
    // it's bogus data that we shouldn't be sharing it with our clients.
    const bool ready_for_use = pmix_gds_shmem3_has_status(
        job, shmem3_id, PMIX_GDS_SHMEM3_READY_FOR_USE
    );
    if (!ready_for_use) {
        return rc;
    }

    pmix_buffer_t buff;
    do {
        PMIX_CONSTRUCT(&buff, pmix_buffer_t);

        rc = pack_shmem3_connection_info(
            job, shmem3_id, peer, &buff
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }

        pmix_value_t blob = {
            .type = PMIX_BYTE_OBJECT
        };
        pmix_kval_t kv = {
            .key = SHMEM3_SEG_BLOB_KEY,
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
cache_connection_info_for_job_shmem3(
    pmix_gds_shmem3_job_t *job
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
    rc = pack_shmem3_seg_blob(
        job, PMIX_GDS_SHMEM3_JOB_ID, me, job->conni
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Then for the session info.
    rc = pack_shmem3_seg_blob(
        job, PMIX_GDS_SHMEM3_SESSION_ID, me, job->conni
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        // Falling through here let the keyindex pack below overwrite rc,
        // so a failure to describe the session segment was reported as
        // success and the client attached the job segment alone. Note
        // that "there is no session segment ready to share" is not this
        // case: pack_shmem3_seg_blob() returns success for that.
        goto out;
    }
    // Nothing else to describe. The key indices the client needs in
    // order to read either segment live inside that segment, so there is
    // no dictionary to reconcile between the two processes.
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(job->conni);
    }
    return rc;
}

static pmix_status_t
server_register_new_job_info(
    pmix_gds_shmem3_job_t *job
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    // Ask for a complete copy of the job-level information.
    pmix_cb_t job_cb;
    PMIX_CONSTRUCT(&job_cb, pmix_cb_t);

    pmix_gds_shmem3_packed_local_job_info_t pji;
    PMIX_CONSTRUCT(&pji, pmix_gds_shmem3_packed_local_job_info_t);

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
    rc = prepare_shmem3_stores_for_local_job_data(job, &pji);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Store fetched data into a shared-memory segment.
    rc = pmix_gds_shmem3_store_local_job_data_in_shmem3(job, &job_cb.kvs);
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
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_peer_t *const peer = (pmix_peer_t *)peer_struct;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        // This function is only available on servers.
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    // Create the job tracker for this peer's nspace.
    pmix_gds_shmem3_job_t *job;
    rc = pmix_gds_shmem3_get_job_tracker(peer->nptr->nspace, true, &job);
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
        PMIX_GDS_SHMEM3_VVOUT(
            "%s: %s registering new job info for namespace=%s", __func__,
            PMIX_NAME_PRINT(&pmix_globals.myid), job->nspace_id
        );

        rc = server_register_new_job_info(job);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }

        rc = cache_connection_info_for_job_shmem3(job);
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
unpack_shmem3_seg_blob_and_attach_if_necessary(
    pmix_kval_t *kvbo
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem3_unpacked_seg_blob_t usb;
    PMIX_CONSTRUCT(&usb, pmix_gds_shmem3_unpacked_seg_blob_t);
    do {
        rc = unpack_shmem3_connection_info(kvbo, &usb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Get the associated job tracker.
        pmix_gds_shmem3_job_t *job;
        rc = pmix_gds_shmem3_get_job_tracker(usb.nsid, true, &job);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Make sure we aren't already attached to the given shmem3.
        if (pmix_gds_shmem3_has_status(job, usb.smid, PMIX_GDS_SHMEM3_ATTACHED)) {
            pmix_shmem_t *cur = NULL;
            rc = pmix_gds_shmem3_get_job_shmem3_by_id(job, usb.smid, &cur);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            /* Same segment: nothing to do. The path is what tells the
             * generations apart - the server names each modex segment
             * after its generation, so a different path under the same
             * id means a newer modex we have not mapped yet. Comparing
             * paths rather than adding a counter to the blob keeps this
             * off the wire entirely. */
            if (NULL != cur && 0 == strcmp(cur->backing_path, usb.seg_path)) {
                break;
            }
            /* A newer one. Let go of ours before mapping it: the old
             * segment's backing file is reference counted, so dropping
             * our handle cannot disturb a peer that is still on it. */
            PMIX_GDS_SHMEM3_VOUT(
                "%s: replacing segment %s with %s for namespace=%s",
                __func__, (NULL == cur) ? "(none)" : cur->backing_path,
                usb.seg_path, usb.nsid
            );
            if (PMIX_GDS_SHMEM3_MODEX_ID == usb.smid) {
                if (usb.is_delta) {
                    /* the arriving generation holds only what changed,
                     * so ours is still the only copy of the rest - keep
                     * it and read back through it */
                    rc = retire_modex_segment(job);
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                        PMIX_ERROR_LOG(rc);
                        break;
                    }
                } else {
                    release_modex_segment(job);
                    drop_modex_priors(job);
                }
            } else {
                /* Only the modex is republished today. Anything else
                 * changing underneath us is not something this path
                 * knows how to do safely, so say so rather than map a
                 * second segment over the first. */
                rc = PMIX_ERR_NOT_SUPPORTED;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        // Looks like we have to attach and initialize it.
        rc = shmem3_segment_attach_and_init(job, &usb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            break;
        }
    } while (false);
    PMIX_DESTRUCT(&usb);

    return rc;
}

static pmix_status_t
client_connect_to_shmem3_from_buffi(
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
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

        if (PMIX_CHECK_KEY(&kval, SHMEM3_SEG_BLOB_KEY)) {
            rc = unpack_shmem3_seg_blob_and_attach_if_necessary(&kval);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                break;
            }
        }
        else {
            PMIX_GDS_SHMEM3_VOUT(
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

    // A segment could not be attached at its required fixed address;
    // propagate the signal unchanged so the client falls back to the next
    // GDS module rather than treating it as a fatal unpack failure.
    if (PMIX_ERR_TAKE_NEXT_OPTION == rc) {
        return rc;
    }
    if (PMIX_UNLIKELY(PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        return rc;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    PMIX_GDS_SHMEM3_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), nspace
    );
    // Done. Before this point the server should have populated the
    // shared-memory segment with the relevant data.
    return client_connect_to_shmem3_from_buffi(buff);
}

/**
 * Returns size required to store modex data.
 */
static pmix_gds_shmem3_modex_info_t
get_modex_sizing_data(
    const pmix_gds_shmem3_job_t *job,
    const pmix_buffer_t *buff
) {
    const size_t kval_size = sizeof(pmix_kval_t);
    // The default values if not provided with modex size info. More fluff than
    // in other places because this calculation is more imprecise. In many ways
    // this is okay because mmap() implements demand paging.
    float fluff = 5.0;
    // Multiplier to fudge compression factor. zlib max compression is 5:1.
    size_t segment_size = buff->bytes_used * 5;
    // Get an estimate on the number of kvals we need to store.
    const size_t nkvals = (segment_size / (float)kval_size) + kval_size;
    /* The modex table is keyed by RANK, not by key: pmix_hash_store()
     * looks up one pmix_proc_data_t per rank and hangs that rank's values
     * off it in a pointer array. So the table needs one element per rank
     * whose data lands in this segment - the procs of this namespace,
     * since a blob for another namespace goes to that job's own segment -
     * and not, as it used to compute, one per byte-group of payload. At
     * 32 ranks that was the difference between 32 elements and tens of
     * thousands, every one of which had to be zeroed into a fresh
     * mapping before the first value could be stored.
     *
     * nprocs comes from the host's PMIX_JOB_SIZE and is not guaranteed to
     * have arrived; the table grows itself if this is short, so a floor
     * of one is a slow path rather than a wrong answer. */
    size_t nranks = (size_t)job->nspace->nprocs;
    if (0 == nranks) {
        nranks = 1;
    }
    // Entries, not capacity: pmix_hash_table_init() applies the density
    // ratio itself, so handing it a capacity would double-apply it.
    const size_t nhtelems = nranks;
    // We also need storage space for the hash table, its elements, and the
    // per-rank structures those elements point at.
    segment_size += sizeof(pmix_hash_table_t);
    segment_size += pmix_hash_table_sizeof_storage(nhtelems);
    segment_size += nranks * pmix_hash_sizeof_proc_storage();
    /* The keyindex that translates the indices in that hash table lives
     * in this segment too. An empty one is already ~170 KB - both its
     * children are built at a fixed capacity whatever the key count turns
     * out to be - and each distinct key then costs a record plus three
     * copies of its own string. We cannot know the number of distinct
     * keys without unpacking, so bound it by nkvals; the real count is
     * normally far smaller.
     *
     * The key *lengths* are bounded by the blob, not by PMIX_MAX_KEYLEN.
     * Every key here arrived in the buffer being unpacked, so the key
     * strings cannot total more than that buffer decompresses to.
     * Reserving 511 bytes for each instead made this one term roughly
     * fifty times the payload - by far the largest thing in this
     * estimate, and the reason a 1.3 MB modex was getting an 80 MB
     * segment. */
    segment_size += pmix_keyindex_sizeof_fixed_storage();
    segment_size += nkvals * pmix_hash_sizeof_key_entry(0);
    // Three copies of every key string, against a payload already scaled
    // by the compression factor above.
    segment_size += 3 * (buff->bytes_used * 5);
    // Include some extra fluff that empirically seems reasonable.
    segment_size *= fluff;
    // Adjust (increase or decrease) segment size by the given parameter size.
    segment_size *= pmix_gds_shmem3_segment_size_multiplier;

    pmix_gds_shmem3_modex_info_t result = {
        .size = segment_size,
        .num_ht_elements = nhtelems
    };
    return result;
}

/**
 * This gets called for each process participating in the modex.
 */
static pmix_status_t
server_store_modex_cb(pmix_proc_t *proc,
                      pmix_buffer_t *pbkt,
                      uint8_t kind)
{
    pmix_status_t rc = PMIX_SUCCESS;
    int32_t cnt;
    pmix_kval_t kv;
    pmix_gds_shmem3_job_t *job;

    /* PMIX_MODEX_DELTA means this contribution holds only what its
     * processes published since they last took part in a collecting
     * fence, so the generation it lands in does not stand on its own. */
    const bool isdelta = (PMIX_MODEX_DELTA == (pmix_collect_t) kind);

    rc = pmix_gds_shmem3_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    if (NULL == pbkt) {
        // Segment is ready for use.
        pmix_gds_shmem3_set_status(
            job, PMIX_GDS_SHMEM3_MODEX_ID, PMIX_GDS_SHMEM3_READY_FOR_USE
        );
        return PMIX_SUCCESS;
    }

    PMIX_GDS_SHMEM3_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        proc->nspace
    );

    const bool attached = pmix_gds_shmem3_has_status(
        job, PMIX_GDS_SHMEM3_MODEX_ID, PMIX_GDS_SHMEM3_ATTACHED
    );
    /* A blob arriving for a modex we already finished means a new one has
     * begun - a second fence.
     *
     * Whether the finished generation can be dropped depends on what is
     * arriving. A cumulative contribution repeats everything its
     * processes have published, so it supersedes what came before and
     * the older generations go. A delta repeats nothing, so they are
     * kept and a read walks them (see modex_fetch in
     * gds_shmem3_fetch.c). See openpmix#4087; examples/modex_twice.c is
     * the canary. Either way the finished segment has been advertised
     * and local clients have it mapped, so it must not be written again:
     * storing into it can rehash the table and reallocate the key index
     * underneath a reader, and the segment was sized for the first
     * modex's data, so a larger second one overruns the allocator and
     * aborts the server. Start a fresh segment instead. */
    const bool complete = pmix_gds_shmem3_has_status(
        job, PMIX_GDS_SHMEM3_MODEX_ID, PMIX_GDS_SHMEM3_READY_FOR_USE
    );
    if (!attached || complete) {
        if (complete) {
            PMIX_GDS_SHMEM3_VOUT(
                "%s: modex generation %u complete; starting %u for namespace=%s",
                __func__, job->modex_generation, job->modex_generation + 1,
                proc->nspace
            );
            if (isdelta) {
                /* keep it - it is still the only copy of everything the
                 * arriving delta does not repeat */
                rc = retire_modex_segment(job);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
            } else {
                /* a cumulative contribution repeats everything, so it
                 * supersedes this generation and every one behind it */
                release_modex_segment(job);
                drop_modex_priors(job);
            }
            job->modex_generation++;
        }
        /* what the clients have to be told about this generation */
        job->modex_is_delta = isdelta;
        /* The name has to differ per generation or the backing paths
         * collide - they are built from the nspace, this pid and this
         * name. */
        char segname[PMIX_PATH_MAX];
        snprintf(segname, sizeof(segname), "modexdata.%u", job->modex_generation);
        // Get the global packed buffer size from ctx.
        pmix_gds_shmem3_modex_info_t minfo = get_modex_sizing_data(job, pbkt);
        // Create and attach to the shared-memory
        // segment that will back these data.
        rc = shmem3_segment_create_and_attach(
            job, PMIX_GDS_SHMEM3_MODEX_ID, segname, minfo.size
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
    // This is data returned via the PMIx_Fence call when data collection was
    // requested, so it only contains REMOTE/GLOBAL data. The byte object
    // contains the rank followed by pmix_kval_ts.
    // Unpack the values until we hit the end of the buffer.
    while (true) {
        // it is okay to use a static variable here and construct it
        // because we are NOT going to actually store the variable
        // anywhere - the hash_store function COPIES it into an
        // appropriately allocated object
        PMIX_CONSTRUCT(&kv, pmix_kval_t);

        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, pbkt, &kv, &cnt, PMIX_KVAL);

        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&kv);
            break;
        }

        const pmix_rank_t rank = proc->rank;
        // If the rank is undefined, then we store it on the remote table of
        // rank=0 as we know that rank must always exist.
        if (PMIX_CHECK_KEY(&kv, PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_shmem3_store_qualified(
                ht, (PMIX_RANK_UNDEF == rank) ? 0 : rank, kv.value,
                job->smmodex->keyindex
            );
        }
        else {
            // Note the keyindex: the indices this store mints have to be
            // readable by every local client that maps the segment, so
            // they come from the segment's own translation table rather
            // than from this process's global one.
            rc = pmix_hash_store(
                ht, (PMIX_RANK_UNDEF == rank) ? 0 : rank, &kv, NULL, 0,
                job->smmodex->keyindex
            );
        }
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&kv);
            break;
        }
        PMIX_DESTRUCT(&kv);
    }

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }
    else {
        // Running off the end of this proc's blob is how we know we have
        // stored all of it. The base envelope walker treats any non-success
        // return as fatal for the server contribution it is walking, so
        // reporting the unpack error here made it log an error and abandon
        // every remaining proc in that contribution.
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
server_store_modex(pmix_buffer_t *buff,
                   const char *nspace,
                   void *cbdata)
{
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    PMIX_GDS_SHMEM3_VOUT(
        "%s:%s buff_size=%zd", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), buff->bytes_used
    );
    return pmix_gds_base_store_modex(buff, nspace, server_store_modex_cb, cbdata);
}

static pmix_status_t
server_setup_fork(
    const pmix_proc_t *peer,
    char ***env
) {
    PMIX_HIDE_UNUSED_PARAMS(peer, env);
    PMIX_GDS_SHMEM3_VVOUT_HERE();
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
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    // Create a job tracker for this nspace.
    pmix_gds_shmem3_job_t *job;
    pmix_status_t rc = pmix_gds_shmem3_get_job_tracker(nspace, true, &job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    for (size_t i = 0; i < ninfo; ++i) {
        if (PMIX_CHECK_KEY(&info[i], PMIX_USERID)) {
            const uid_t nuid = (uid_t)info[i].value.data.uint32;
            PMIX_GDS_SHMEM3_VOUT(
                "%s: updating nspace=%s UID from %zd to %zd",
                __func__, nspace, (size_t)job->uid, (size_t)nuid
            );
            job->uid = nuid;
            job->chown = true;
        }
        else if (PMIX_CHECK_KEY(&info[i], PMIX_GRPID)) {
            const gid_t ngid = (gid_t)info[i].value.data.uint32;
            PMIX_GDS_SHMEM3_VOUT(
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
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    PMIX_GDS_SHMEM3_VOUT(
        "%s: %s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid), nspace
    );

    pmix_gds_shmem3_job_t *ji, *found = NULL;
    pmix_gds_shmem3_component_t *const component = &pmix_mca_gds_shmem3_component;
    /* Take the tracker off the list under the lock so a reader on
     * another thread cannot be walking the spine while it moves. The
     * release is deliberately outside: if a reader holds a reference,
     * this is not the last one and the destructor - which detaches the
     * segments - does not run until that reader is done. */
    pmix_mutex_lock(&component->joblock);
    PMIX_LIST_FOREACH (ji, &component->jobs, pmix_gds_shmem3_job_t) {
        if (0 == strcmp(nspace, ji->nspace_id)) {
            pmix_list_remove_item(&component->jobs, &ji->super);
            found = ji;
            break;
        }
    }
    pmix_mutex_unlock(&component->joblock);
    if (NULL != found) {
        PMIX_RELEASE(found);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
server_mark_modex_complete(
    struct pmix_peer_t *peer,
    pmix_list_t *nslist,
    pmix_buffer_t *reply
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    // Pack connection info for each ns in nslist.
    pmix_nspace_caddy_t *nsi;
    PMIX_LIST_FOREACH (nsi, nslist, pmix_nspace_caddy_t) {
        // false here because we should already know about the nspace.
        pmix_gds_shmem3_job_t *job;
        rc = pmix_gds_shmem3_get_job_tracker(
            nsi->ns->nspace, false, &job
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            break;
        }
        // Pack modex info, if it is ready to be shared.
        rc = pack_shmem3_seg_blob(
            job, PMIX_GDS_SHMEM3_MODEX_ID, peer, reply
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
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    return client_connect_to_shmem3_from_buffi(buff);
}

pmix_gds_base_module_t pmix_shmem3_module = {
    .name = PMIX_GDS_SHMEM3_NAME,
    /* fetch() may be called from a thread other than the progress
     * thread. It holds a reference to the job tracker for the duration,
     * which keeps the segments mapped, and everything it reads from
     * them is immutable - a segment a client can see is never written
     * again. See pmix_gds_shmem3_fetch(). */
    .is_tsafe = true,
    .init = module_init,
    .finalize = module_finalize,
    .assign_module = assign_module,
    .cache_job_info = server_cache_job_info,
    .register_job_info = server_register_job_info,
    .store_job_info = store_job_info,
    .store = NULL,
    .store_modex = server_store_modex,
    .fetch = pmix_gds_shmem3_fetch,
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
