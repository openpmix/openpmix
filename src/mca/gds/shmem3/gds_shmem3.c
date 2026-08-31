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

#include "src/mca/bfrops/base/bfrop_base_tma.h"

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
/* Session id, on a SESSION blob only. A client needs it to decide which
 * session tracker a segment belongs to, and it cannot read that out of
 * the segment: the answer is what says whether to map the segment at
 * all. Appended, so an older peer simply skips it. */
#define SHMEM3_SEG_SSID_KEY "PMIX_GDS_SHMEM3_SEG_SSID"
/* A key this job has stopped answering for, as "<rank>:<key>". The
 * segments still contain it - they are never rewritten - so a client
 * attaching after the removal has to be told, or it would read what
 * every process already attached has been told to ignore. */
#define SHMEM3_TOMBSTONE_KEY "PMIX_GDS_SHMEM3_TOMBSTONE"
#define SHMEM3_SEG_ARBS_KEY "PMIX_GDS_SHMEM3_SEG_ARBS"
#define SHMEM3_SEG_ARSZ_KEY "PMIX_GDS_SHMEM3_SEG_ARSZ"


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
    /* Upper bound on the distinct keys this modex will register. The
     * segment's key index is sized from it, and must be sized from the
     * SAME number the estimate reserved for - the allocator behind it
     * cannot grow, so an index that rehashes runs off the end. */
    size_t nkvals;
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
    /** Session id from a SESSION blob; UINT32_MAX if none was sent. */
    uint32_t ssid;
    /** The job's address-space arena, if it has one. Carried on every
     *  seg blob rather than only the first, so a client that has not yet
     *  reserved it learns of it from whichever blob reaches it first. */
    size_t arena_base;
    size_t arena_size;
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
    ub->ssid = UINT32_MAX;
    ub->arena_base = 0;
    ub->arena_size = 0;
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
    job->chain_incomplete = false;
    job->shmem3_status = 0;
    job->shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smdata = NULL;
    // Modex
    job->modex_shmem3_status = 0;
    job->modex_generation = 0;
    job->modex_shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
    job->modex_is_delta = false;
    job->job_generation = 0;
    atomic_init(&job->job_chain, NULL);
    atomic_init(&job->modex_chain, NULL);
    atomic_init(&job->tombstones, NULL);
    // Address-space arena
    job->arena_base = 0;
    job->arena_size = 0;
    job->arena_static_used = 0;
    job->arena_slot_bytes = 0;
    job->arena_slots = 0;
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

/* Report a segment's utilization. Takes the segment and its allocator
 * rather than a (job, id) pair because the session's segment belongs to
 * the session object, which has no job to be asked about. */
static void
emit_segment_usage_stats(
    const pmix_shmem_t *shmem3,
    pmix_tma_t *tma,
    const char *smname
) {
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

    emit_segment_usage_stats(
        shmem3,
        get_tma_by_shmem3_id(job, shmem3_id),
        get_shmem3_id_name(shmem3_id)
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
    pmix_gds_shmem3_chain_destruct(&job->job_chain);
    pmix_gds_shmem3_chain_destruct(&job->modex_chain);
    /* Nothing can still be reading these - we are the destructor, and a
     * reader holds a reference on this tracker. */
    {
        pmix_gds_shmem3_tombstone_t *t =
            atomic_load_explicit(&job->tombstones, memory_order_acquire);
        while (NULL != t) {
            pmix_gds_shmem3_tombstone_t *const prior = t->prior;
            if (NULL != t->key) {
                free(t->key);
            }
            free(t);
            t = prior;
        }
    }

    /* The session's segment is deliberately not in here: it belongs to
     * the session object, which more than one job may hold, and is given
     * back by session_destruct() when the last of them lets go. */
    /* The job's BUILD slot. Its published segments are on the chain,
     * which gave them back above; this is the handle the slot was left
     * holding - either a segment that was being written when the job
     * went away, or the empty one publishing hands back. Either way
     * nothing ever saw it, so nothing can be reading it. */
    if (NULL != job->shmem3) {
        if (NULL != job->smdata &&
            (job->shmem3_status & PMIX_GDS_SHMEM3_MINE)) {
            emit_segment_usage_stats(
                job->shmem3, &job->smdata->tma,
                get_shmem3_id_name(PMIX_GDS_SHMEM3_JOB_ID)
            );
            PMIX_RELEASE(job->smdata->tma.data_context);
        }
        PMIX_RELEASE(job->shmem3);
        job->shmem3 = NULL;
        job->smdata = NULL;
        job->shmem3_status = 0;
    }

    /* Neither the job's nor the session's published segments are in the
     * loop below: both are held by chains, which gave them back above.
     * What is left is the modex BUILD slot - a generation that was being
     * assembled when this job went away and was never published. */
    static const pmix_gds_shmem3_job_shmem3_id_t shmem3_ids[] = {
        PMIX_GDS_SHMEM3_MODEX_ID,
        PMIX_GDS_SHMEM3_INVALID_ID
    };
    for (int i = 0; shmem3_ids[i] != PMIX_GDS_SHMEM3_INVALID_ID; ++i) {
        const pmix_gds_shmem3_job_shmem3_id_t sid = shmem3_ids[i];

        pmix_shmem_t *shmem3;
        rc = pmix_gds_shmem3_get_job_shmem3_by_id(job, sid, &shmem3);
        if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
            /* Neither of the two ids above can fail this lookup - both
             * handles are allocated by job_construct(). Checked anyway,
             * and skipped rather than returned on, because abandoning
             * the loop would also abandon the arena reservation and the
             * session reference given back below. */
            PMIX_ERROR_LOG(rc);
            continue;
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

    /* The arena goes last: releasing it unmaps everything inside it, so
     * every segment that was carved from it has to have let go first.
     * The session segment is never carved from it - see the refusal in
     * shmem3_segment_create_and_attach() - because a session outlives
     * the job that first described it, and unmapping a shared session's
     * segment along with this job's arena would pull it out from under
     * whoever still holds a reference. */
    if (0 != job->arena_size) {
        pmix_vmem_release(job->arena_base, job->arena_size);
        job->arena_base = 0;
        job->arena_size = 0;
    }

    if (job->session) {
        PMIX_RELEASE(job->session);
    }
}

/* Give a chain node and its segment back.
 *
 * A chain node is a plain allocation rather than a pmix_object_t: it is
 * never referenced by anything but the chain, and a refcount would
 * suggest it can be released from more than one place - which is exactly
 * what a lock-free chain must not permit. Only a caller that knows no
 * reader can be walking it may call this; see
 * pmix_gds_shmem3_chain_destruct().
 */
void
pmix_gds_shmem3_seg_release(
    pmix_gds_shmem3_seg_t *seg
) {
    if (NULL == seg) {
        return;
    }
    /* Mirrors what job_destruct() does for the current generation -
     * keep the two in step. Order matters: the allocator context is
     * reached through smdata, so it goes first. */
    if (NULL != seg->shmem3) {
        if ((PMIX_GDS_SHMEM3_MINE & seg->status) && NULL != seg->smdata) {
            pmix_tma_t *tma = NULL;
            switch (seg->smid) {
                case PMIX_GDS_SHMEM3_JOB_ID:
                    tma = &((pmix_gds_shmem3_shared_job_data_t *)
                            seg->smdata)->tma;
                    break;
                case PMIX_GDS_SHMEM3_SESSION_ID:
                    tma = &((pmix_gds_shmem3_shared_session_data_t *)
                            seg->smdata)->tma;
                    break;
                case PMIX_GDS_SHMEM3_MODEX_ID:
                    tma = &((pmix_gds_shmem3_shared_modex_data_t *)
                            seg->smdata)->tma;
                    break;
                default:
                    break;
            }
            if (NULL != tma) {
                PMIX_RELEASE(tma->data_context);
            }
        }
        PMIX_RELEASE(seg->shmem3);
    }
    free(seg);
}

void
pmix_gds_shmem3_chain_destruct(
    pmix_gds_shmem3_chain_t *chain
) {
    pmix_gds_shmem3_seg_t *seg = pmix_gds_shmem3_chain_head(chain);

    atomic_store_explicit(chain, NULL, memory_order_release);
    while (NULL != seg) {
        pmix_gds_shmem3_seg_t *const prior = seg->prior;
        pmix_gds_shmem3_seg_release(seg);
        seg = prior;
    }
}

/**
 * Publish the generation just built, making it visible to readers.
 *
 * Until this runs, the segment is described only by the build fields on
 * the job tracker, which no reader touches - so a generation under
 * construction cannot be read half-built. Publishing is a single
 * release-store onto the chain, which is why this needs no lock: there
 * is no window in which a reader sees anything partial.
 *
 * The node takes ownership of the segment handle and the build slot gets
 * a fresh one, so the two never both own it. The backing file is
 * reference counted, so a client that has this generation mapped is
 * undisturbed by anything that happens here.
 */
static pmix_status_t
publish_modex_generation(
    pmix_gds_shmem3_job_t *job
) {
    if (!pmix_gds_shmem3_has_status(job, PMIX_GDS_SHMEM3_MODEX_ID,
                                    PMIX_GDS_SHMEM3_ATTACHED)) {
        return PMIX_SUCCESS;
    }
    pmix_gds_shmem3_seg_t *seg = calloc(1, sizeof(*seg));
    if (PMIX_UNLIKELY(NULL == seg)) {
        return PMIX_ERR_NOMEM;
    }
    /* Fill the node in completely before publishing it - a reader that
     * loads the head must never see a half-built one. */
    seg->smid = PMIX_GDS_SHMEM3_MODEX_ID;
    seg->status = job->modex_shmem3_status;
    seg->shmem3 = job->modex_shmem3;
    seg->smdata = job->smmodex;
    seg->generation = job->modex_generation;
    seg->is_delta = job->modex_is_delta;

    pmix_gds_shmem3_chain_publish(&job->modex_chain, seg);

    /* The build slot starts empty again. Nothing reads these, so there
     * is no ordering requirement against the store above. */
    job->modex_shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smmodex = NULL;
    pmix_gds_shmem3_clearall_status(job, PMIX_GDS_SHMEM3_MODEX_ID);
    return PMIX_SUCCESS;
}

/**
 * Publish the job segment just built, making it readable.
 *
 * Same discipline as the modex and the session: until this runs the
 * segment is described only by the build fields, which no reader
 * touches, so a reader cannot see one half-built. Publishing is a
 * single release-store and needs no lock.
 */
pmix_status_t
pmix_gds_shmem3_publish_job_segment(
    pmix_gds_shmem3_job_t *job
) {
    if (NULL == job->smdata) {
        return PMIX_SUCCESS;
    }
    pmix_gds_shmem3_seg_t *seg = calloc(1, sizeof(*seg));
    if (PMIX_UNLIKELY(NULL == seg)) {
        return PMIX_ERR_NOMEM;
    }
    seg->smid = PMIX_GDS_SHMEM3_JOB_ID;
    seg->status = job->shmem3_status;
    seg->shmem3 = job->shmem3;
    seg->smdata = job->smdata;

    seg->generation = job->job_generation;
    pmix_gds_shmem3_chain_publish(&job->job_chain, seg);
    job->job_generation += 1;

    /* The build slot starts empty again; the node owns the segment. */
    job->shmem3 = PMIX_NEW(pmix_shmem_t);
    job->smdata = NULL;
    job->shmem3_status = 0;
    return PMIX_SUCCESS;
}

/**
 * Publish the session segment just built, making it readable.
 *
 * Same discipline as publish_modex_generation(): until this runs the
 * segment is described only by the build fields on the session object,
 * which no reader touches, so a reader cannot see one half-built.
 * Publishing is a single release-store and needs no lock.
 */
pmix_status_t
pmix_gds_shmem3_publish_session_segment(
    pmix_gds_shmem3_session_t *sesh
) {
    if (NULL == sesh || NULL == sesh->smdata) {
        return PMIX_SUCCESS;
    }
    pmix_gds_shmem3_seg_t *seg = calloc(1, sizeof(*seg));
    if (PMIX_UNLIKELY(NULL == seg)) {
        return PMIX_ERR_NOMEM;
    }
    seg->smid = PMIX_GDS_SHMEM3_SESSION_ID;
    seg->status = sesh->shmem3_status;
    seg->shmem3 = sesh->shmem3;
    seg->smdata = sesh->smdata;
    seg->generation = sesh->generation;

    pmix_gds_shmem3_chain_publish(&sesh->segments, seg);
    sesh->generation += 1;

    /* The build slot starts empty again; the node owns the segment now. */
    sesh->shmem3 = PMIX_NEW(pmix_shmem_t);
    sesh->smdata = NULL;
    sesh->shmem3_status = 0;
    return PMIX_SUCCESS;
}

/**
 * Note that this job has taken on a new modex generation.
 *
 * Both ends do this, and for different reasons: the server needs a
 * number that names the next backing file, and both need one that dates
 * a tombstone. See pmix_gds_shmem3_job_t.modex_generation - a client
 * that never advanced it made every tombstone shadow every generation.
 */
static void
advance_modex_generation(
    pmix_gds_shmem3_job_t *job
) {
    /* Atomic because a read on another thread compares a tombstone
     * against it. A plain increment would be a torn read there; nothing
     * else about it needs ordering, since a tombstone carries its own
     * copy of the number it was recorded at. */
    atomic_fetch_add_explicit(&job->modex_generation, 1,
                              memory_order_relaxed);
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
    s->id = UINT32_MAX;
    s->sinfo = NULL;
    s->nsinfo = 0;
    s->described = false;
    s->generation = 0;
    s->shmem3 = PMIX_NEW(pmix_shmem_t);
    s->shmem3_status = 0;
    s->smdata = NULL;
    s->chain_incomplete = false;
    atomic_init(&s->segments, NULL);
}

static void
session_destruct(
    pmix_gds_shmem3_session_t *s
) {
    /* Nothing unlinks here. The component's list holds a reference of
     * its own, so a session on it cannot reach this destructor; only
     * del_session() takes that reference back, and then the object lives
     * on until the last job in the session has let go too.
     *
     * That ordering is the point. A session is not over because its last
     * job ended - a session with no jobs running is an ordinary state,
     * and another job may be about to be launched into it - so only the
     * host can say, through PMIx_server_deregister_session(). */
    /* Every segment describing this session. They are the session's own:
     * this used to be job_destruct()'s job, which works only while every
     * session has exactly one job - the first job destructed would unmap
     * them while every other holder's reference still said the object,
     * and the smdata pointing into that mapping, was alive. A reference
     * counts the object; releasing here makes it count the mappings
     * too. */
    pmix_gds_shmem3_chain_destruct(&s->segments);
    if (NULL != s->shmem3) {
        /* A segment that was being built when this session went away -
         * never published, so nothing can be reading it. */
        if (NULL != s->smdata && (s->shmem3_status & PMIX_GDS_SHMEM3_MINE)) {
            emit_segment_usage_stats(
                s->shmem3, &s->smdata->tma,
                get_shmem3_id_name(PMIX_GDS_SHMEM3_SESSION_ID)
            );
            PMIX_RELEASE(s->smdata->tma.data_context);
        }
        PMIX_RELEASE(s->shmem3);
        s->shmem3 = NULL;
    }
    if (NULL != s->sinfo) {
        PMIX_INFO_FREE(s->sinfo, s->nsinfo);
        s->sinfo = NULL;
        s->nsinfo = 0;
    }
    s->described = false;
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
    size_t htsize,
    size_t nkeys
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
    /* Sized to this segment's own key count, and it must be the same
     * count the estimate reserved for: the allocator behind it cannot
     * grow, so an index that rehashes here runs off the end of the
     * segment. */
    rc = pmix_keyindex_init(job->smdata->keyindex, nkeys);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }

    pmix_gds_shmem3_vout_smdata(job);
out:
    if (PMIX_SUCCESS != rc) {
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
    size_t htsize,
    size_t nkeys
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
    /* Sized to this segment's own key count - see the note in
     * job_smdata_construct(). */
    rc = pmix_keyindex_init(job->smmodex->keyindex, nkeys);
    if (PMIX_SUCCESS != rc) {
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
        const char *tmpdir = getenv("TMPDIR");
        if (NULL == tmpdir) {
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
    uint32_t session_id,
    uint32_t generation
) {
    static char name[64] = {'\0'};
    /* The generation has to be in the name, for the same reason it is in
     * a modex segment's: the backing paths are built from it, so two
     * segments describing one session would collide. It is also what a
     * client tells the generations apart by. */
    size_t nw = snprintf(
        name, sizeof(name), "session.%zx.%u",
        (size_t)session_id, (unsigned)generation
    );
    if (nw >= sizeof(name)) {
        return NULL;
    }
    return name;
}

/**
 * The rule to use when a segment has to place itself independently.
 *
 * See VMEM_HOLE_BIGGEST_OFFSET: the midpoint of the biggest hole is where
 * every implementation of this idea in the stack converges, so it is the
 * worst place to put something that another process also has to map.
 */
static inline pmix_vmem_hole_kind_t
segment_hole_kind(void)
{
    return pmix_gds_shmem3_offset_placement
        ? VMEM_HOLE_BIGGEST_OFFSET
        : VMEM_HOLE_BIGGEST;
}

/**
 * A value that differs between jobs and is the same everywhere within
 * one, used to keep concurrent jobs' segments from being placed on top
 * of each other.
 *
 * The namespace is the right thing to derive it from: it is unique
 * within the scope of the resource manager, which is exactly the scope
 * over which two jobs can be resident on a node at once, and every
 * process of a job knows it. It matters that this is a pure function of
 * the name and nothing else - a server and its clients never compare
 * notes about the address, they each compute it and are expected to
 * arrive at the same one.
 *
 * FNV-1a, because it is four lines and the requirement is only that
 * different names land in different places, not that anything is hard to
 * guess. A collision costs a failed reservation and a fall back to
 * placing segments individually, which is where we were before.
 */
static inline uint64_t
nspace_scatter(
    const char *nspace
) {
    uint64_t h = 14695981039346656037ULL;

    if (NULL == nspace) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)nspace; *p; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

/**
 * Is [addr, addr+size) inside this job's arena?
 */
static inline bool
addr_in_arena(
    const pmix_gds_shmem3_job_t *job,
    uintptr_t addr,
    size_t size
) {
    if (0 == job->arena_size || 0 == size) {
        return false;
    }
    if (addr < job->arena_base || size > job->arena_size) {
        return false;
    }
    // Written as a subtraction so the sum cannot wrap.
    return (addr - job->arena_base) <= (job->arena_size - size);
}

/* Which delivery a seg blob arrived on.
 *
 * This has to be carried rather than inferred, because it decides what a
 * failed attach is allowed to do about itself and the two answers are
 * opposites.
 *
 * At INIT the client is inside PMIx_Init reading the job-info reply. A
 * segment it cannot map is answered by abandoning this module wholesale:
 * PMIX_ERR_TAKE_NEXT_OPTION travels up to fallback_to_next_gds(), which
 * re-points the peer at gds/hash and re-requests the job data in that
 * module's format. Dropping the tracker is part of that - nothing will
 * read it again.
 *
 * Afterwards there is no such move to make. A modex generation cannot be
 * re-delivered in another module's format, and neither can an update to
 * job or session data: both arrive on a one-way notification with no
 * re-request behind it. So a failure has to cost only the segment that
 * failed. Dropping the tracker there takes down the job and session
 * segments this client has been reading happily since PMIx_Init, so
 * pmix_gds_shmem3_fetch() then answers every lookup for its OWN
 * namespace with PMIX_ERR_INVALID_NAMESPACE - there being no tracker to
 * acquire. That is openpmix#4156, and it was fixed for the modex by
 * testing the segment id. That test was a proxy for this distinction and
 * only covered the case that had been noticed; a job or session segment
 * arriving on an update took the other arm and dropped the tracker
 * exactly as the modex used to.
 *
 * Be precise about what the APPLICATION sees, because it is not that: an
 * ordinary PMIx_Get misses locally, goes up to the server, and is
 * answered correctly, so the cost is a round trip per lookup for the
 * rest of the run. It takes PMIX_OPTIONAL - which confines the request
 * to this process - to see it at all, which is what
 * test/unit/update_attach_fail asks with and why it had to.
 */
typedef enum {
    /** The job-info reply, inside PMIx_Init. A failure may fall back. */
    PMIX_GDS_SHMEM3_ATTACH_INIT = 0,
    /** Anything described after that - a modex generation at a fence, or
     *  an update to job or session data. A failure costs one segment. */
    PMIX_GDS_SHMEM3_ATTACH_UPDATE
} pmix_gds_shmem3_attach_ctx_t;

/**
 * Take a job tracker off the component list and give up our reference.
 *
 * The removal has to be under joblock for the same reason del_nspace()
 * does it that way: a reader on another thread can be walking this
 * spine at any moment (pmix_gds_shmem3_acquire_job_tracker(), reached
 * from a fetch on the application's thread), and pmix_list_remove_item()
 * is several stores it could catch part way through.
 *
 * The release stays OUTSIDE the lock, and that is not a detail. If a
 * reader already holds a reference, this is not the last one, and the
 * destructor - which detaches the segments the reader is walking - does
 * not run until that reader is done with it.
 */
static void
drop_job_tracker(
    pmix_gds_shmem3_job_t *job
) {
    pmix_gds_shmem3_component_t *const component = &pmix_mca_gds_shmem3_component;

    pmix_mutex_lock(&component->joblock);
    pmix_list_remove_item(&component->jobs, &job->super);
    pmix_mutex_unlock(&component->joblock);
    PMIX_RELEASE(job);
}

/**
 * Attaches to the given shared-memory segment.
 */
static pmix_status_t
shmem3_attach(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    uintptr_t req_addr,
    pmix_gds_shmem3_attach_ctx_t ctx
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

    /* If this address is inside the arena we reserved for the job, then
     * we are not asking for it - we are already holding it, and mapping
     * over our own reservation cannot be refused. That is the point of
     * having reserved it: the client's address space has moved on since
     * PMIx_Init, but this range has been ours the whole time. */
    const pmix_shmem_flags_t aflags =
        addr_in_arena(job, req_addr, shmem3->size)
            ? PMIX_SHMEM_MAP_OVER_RESERVATION
            : PMIX_SHMEM_MUST_MAP_AT_RADDR;

    rc = pmix_shmem_segment_attach(
        shmem3, req_addr, aflags, PMIX_GDS_SHMEM3_LAYOUT_ID
    );
    if (PMIX_UNLIKELY(pmix_gds_shmem3_force_client_attach_failure)) {
        // Testing only: pretend the fixed-address attach failed so the
        // client exercises the GDS fallback path. The shared "out" cleanup
        // below detaches the segment if the real attach actually succeeded.
        rc = PMIX_ERR_NOT_AVAILABLE;
    }
    if (PMIX_UNLIKELY(pmix_gds_shmem3_force_update_attach_failure &&
                      PMIX_GDS_SHMEM3_ATTACH_UPDATE == ctx &&
                      PMIX_GDS_SHMEM3_MODEX_ID != shmem3_id)) {
        /* Testing only: fail a job or session segment that arrives after
         * PMIx_Init. Neither of the parameters above can reach that -
         * force_client_attach_failure fails the init attach too, so the
         * client leaves PMIx_Init on hash, and the modex one is scoped to
         * the modex. */
        rc = PMIX_ERR_NOT_AVAILABLE;
    }
    if (PMIX_UNLIKELY(pmix_gds_shmem3_force_modex_attach_failure &&
                      PMIX_GDS_SHMEM3_MODEX_ID == shmem3_id)) {
        /* Testing only: fail just this segment. Unlike the parameter
         * above, this leaves the client on shmem3 through PMIx_Init and
         * so reaches the fence-time attach - the path that has no GDS
         * fallback to take. */
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
        if (PMIX_GDS_SHMEM3_ATTACH_UPDATE == ctx) {
            /* Only this segment is lost. Everything else this client has
             * been reading since PMIx_Init is mapped and perfectly good,
             * so keep the tracker: dropping it took those down too, and
             * left the client answering every job-level lookup for its
             * OWN namespace with PMIX_ERR_INVALID_NAMESPACE - long after
             * the delivery that caused it. See openpmix#4156, which is
             * this failure for the modex; a job or session segment
             * arriving on an update reached it the same way, because the
             * test here used to be on the segment id rather than on how
             * the segment arrived.
             *
             * What the client loses is the shared copy of what that one
             * segment held - and it must now DECLINE to answer for that
             * realm locally, which is what the flag below arranges.
             *
             * It is tempting to think the loss takes care of itself,
             * and this comment used to say so: a chain the segment was
             * never added to does not answer for its keys, so the miss
             * goes to the server like any other. That is true only of a
             * key the segment INTRODUCED. An update mostly carries keys
             * whose values CHANGED, and for those the older segment is
             * still on the chain and still answers - job_fetch() stops
             * at the newest segment holding the key. So the client did
             * not miss; it returned the value the missed segment had
             * been published to replace, with PMIX_SUCCESS, for the
             * rest of the run, while every peer that mapped the segment
             * read the new one. A silent wrong answer, and a divergence
             * between processes on one node.
             *
             * Clearing the status is what leaves a later generation -
             * or a later update - free to attach cleanly. */
            if (PMIX_GDS_SHMEM3_SESSION_ID == shmem3_id) {
                if (NULL != job->session) {
                    job->session->chain_incomplete = true;
                }
            } else {
                job->chain_incomplete = true;
            }
            pmix_gds_shmem3_clearall_status(job, shmem3_id);
        }
        else {
            /* Inside PMIx_Init. The caller is about to switch this peer
             * to another module and re-request its job data, so nothing
             * will read this tracker again. */
            drop_job_tracker(job);
        }
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

    /* A modex generation becomes visible to this process's readers here,
     * whole - the same discipline the server follows. Until now it was
     * described only by the build fields, which no reader consults. */
    if (PMIX_GDS_SHMEM3_MODEX_ID == shmem3_id) {
        return publish_modex_generation(job);
    }
    if (PMIX_GDS_SHMEM3_SESSION_ID == shmem3_id) {
        return pmix_gds_shmem3_publish_session_segment(job->session);
    }
    if (PMIX_GDS_SHMEM3_JOB_ID == shmem3_id) {
        return pmix_gds_shmem3_publish_job_segment(job);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t
shmem3_segment_attach_and_init(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_unpacked_seg_blob_t *seginfo,
    pmix_gds_shmem3_attach_ctx_t ctx
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
    rc = shmem3_attach(job, seginfo->smid, req_addr, ctx);
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
 * Reserve this job's address-space arena.
 *
 * Called by the server once, before it creates the first of the job's
 * segments, and mirrored by each client the first time it is told about
 * the arena - which is inside PMIx_Init, while the client's address space
 * is still close to empty and a fixed address is still easy to come by.
 * That timing is the whole idea: the address a client will need at fence
 * time, when it is full of MPI, is claimed while it is not.
 *
 * A failure here is not one: without an arena every segment places itself
 * as it did before, which is what the caller does anyway when the arena
 * cannot hold what it is asked for.
 */
static void
arena_reserve(
    pmix_gds_shmem3_job_t *job,
    size_t job_segsize
) {
    if (0 == pmix_gds_shmem3_arena_slot_size) {
        // Explicitly disabled.
        return;
    }
    if (0 != job->arena_size) {
        // Already have one.
        return;
    }

    /* Ask what the segment will actually occupy, rather than assuming it
     * is the size we asked for: a segment maps a page of header ahead of
     * its data. Carving to the requested size instead left every segment
     * overlapping the next by that page. */
    /* Room for the job's own segment AND for the ones a host publishes
     * later by adding to a registered job. Sizing this for exactly one -
     * which the job's own then filled - meant every later job segment
     * was placed from the server's own address map and offered to
     * clients at an address nothing had reserved. */
    size_t job_slots = pmix_gds_shmem3_arena_job_slots;
    if (0 == job_slots) {
        job_slots = 1;
    }
    const size_t one_static = pmix_shmem_utils_segment_footprint(job_segsize);
    if (one_static > SIZE_MAX / job_slots) {
        return;
    }
    const size_t statics = one_static * job_slots;
    size_t slot = pmix_shmem_utils_pad_to_page(
        pmix_gds_shmem3_arena_slot_size
    );
    /* A job whose job-level data alone exceeds the configured slot is
     * one whose modex will not be small either. Do not hand it slots it
     * is guaranteed to overflow. */
    if (slot < one_static) {
        slot = one_static;
    }

    /* One slot per modex generation that can be live at once. More than
     * one is live whenever a contribution carried only what changed -
     * the generations before it are still the only copy of what it did
     * not repeat - so this is a depth, not the two an alternation would
     * need. The bitmap in arena_alloc_modex() is what caps it at 32. */
    size_t slots = pmix_gds_shmem3_arena_modex_slots;
    if (0 == slots) {
        return;
    }
    if (slots > 32) {
        slots = 32;
    }
    // Guard the arithmetic rather than trust two tunables to be sane.
    if (slot > (SIZE_MAX - statics) / slots) {
        return;
    }
    const size_t total = statics + (slots * slot);

    uintptr_t base = 0;
    if (PMIX_SUCCESS != pmix_vmem_reserve(segment_hole_kind(),
                                          nspace_scatter(job->nspace_id),
                                          total, &base)) {
        PMIX_GDS_SHMEM3_VOUT(
            "%s: could not reserve a %zu B arena for namespace=%s; "
            "segments will be placed individually",
            __func__, total, job->nspace_id
        );
        return;
    }
    job->arena_base = base;
    job->arena_size = total;
    job->arena_static_used = 0;
    job->arena_slot_bytes = slot;
    job->arena_slots = slots;

    PMIX_GDS_SHMEM3_VOUT(
        "%s: reserved arena [0x%zx, 0x%zx) (%zu B, %zu slots of %zu B) "
        "for namespace=%s",
        __func__, (size_t)base, (size_t)(base + total), total, slots, slot,
        job->nspace_id
    );
}

/**
 * Hand out arena space for a segment that lives as long as the job does.
 *
 * Returns false if there is no arena or it cannot hold this, in which
 * case the caller places the segment the old way.
 */
static bool
arena_alloc_static(
    pmix_gds_shmem3_job_t *job,
    size_t size,
    uintptr_t *addr
) {
    if (0 == job->arena_size || 0 == job->arena_slots) {
        return false;
    }
    const size_t statics =
        job->arena_size - (job->arena_slots * job->arena_slot_bytes);
    if (size > statics - job->arena_static_used) {
        return false;
    }
    *addr = job->arena_base + job->arena_static_used;
    job->arena_static_used += size;
    return true;
}

/**
 * Where the modex slots begin - everything below this is the static
 * region the job segment came out of.
 */
static inline uintptr_t
arena_modex_base(
    const pmix_gds_shmem3_job_t *job
) {
    return job->arena_base + job->arena_size
           - (job->arena_slots * job->arena_slot_bytes);
}

/**
 * Mark the slot a mapped segment occupies, if it is in the arena.
 */
static inline void
note_slot_in_use(
    const pmix_gds_shmem3_job_t *job,
    const pmix_shmem_t *shmem3,
    uint32_t *inuse
) {
    if (NULL == shmem3 || !shmem3->attached || NULL == shmem3->hdr_address) {
        return;
    }
    const uintptr_t addr = (uintptr_t)shmem3->hdr_address;
    const uintptr_t mbase = arena_modex_base(job);

    if (addr < mbase || addr >= job->arena_base + job->arena_size) {
        // Placed outside the arena; owns no slot.
        return;
    }
    *inuse |= (uint32_t)1 << ((addr - mbase) / job->arena_slot_bytes);
}

/**
 * Hand out an arena slot for a new modex generation.
 *
 * A generation cannot simply take the slot before last. It used to be
 * true that at most one was live at a time, and this alternated between
 * two slots on that basis; it stopped being true when a contribution
 * became able to carry only what changed. A delta generation leaves
 * every generation before it mapped and answerable on job->modex_chain,
 * so the slot each of those sits in is still in use, and writing over
 * one would take away data nothing else holds a copy of.
 *
 * So read the occupancy off the segments themselves - the current
 * generation and every retired one - rather than keeping a count.
 * Deriving it means there is no allocation tally to be left stale by a
 * path that releases a generation without telling us, and the whole
 * thing is a walk of a list that is empty in the common case.
 *
 * Returns false when every slot is taken, which is not an error: the
 * caller places the generation outside the arena exactly as it would
 * have without one.
 */
static bool
arena_alloc_modex(
    pmix_gds_shmem3_job_t *job,
    size_t size,
    uintptr_t *addr
) {
    uint32_t inuse = 0;

    if (0 == job->arena_size || 0 == job->arena_slots) {
        return false;
    }
    if (size > job->arena_slot_bytes) {
        return false;
    }

    note_slot_in_use(job, job->modex_shmem3, &inuse);
    for (pmix_gds_shmem3_seg_t *seg =
             pmix_gds_shmem3_chain_head(&job->modex_chain);
         NULL != seg; seg = seg->prior) {
        note_slot_in_use(job, seg->shmem3, &inuse);
    }

    for (size_t slot = 0; slot < job->arena_slots; ++slot) {
        if (0 == (inuse & ((uint32_t)1 << slot))) {
            *addr = arena_modex_base(job) + (slot * job->arena_slot_bytes);
            return true;
        }
    }
    PMIX_GDS_SHMEM3_VOUT(
        "%s: all %zu modex slots are in use for namespace=%s; placing "
        "generation %u outside the arena",
        __func__, job->arena_slots, job->nspace_id, job->modex_generation
    );
    return false;
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
    // Place the segment. Out of the job's arena where it fits, because a
    // reserved address is one every client can be sure of holding too;
    // otherwise from a hole located here and now, which is what the arena
    // exists to avoid having to do.
    //
    // Carve by what the mapping will occupy, not by what we asked to
    // store: pmix_shmem_segment_create() put a page of header in front of
    // it, and shmem3->size below reports the larger number.
    const size_t mapped_size = pmix_shmem_utils_segment_footprint(
        real_segsize
    );
    uintptr_t arena_addr = 0;
    bool from_arena;
    switch (shmem3_id) {
        case PMIX_GDS_SHMEM3_SESSION_ID:
            /* Never out of this job's arena. The session's segment is
             * released with the session object, which can outlive this
             * job and be held by other jobs, while job_destruct() unmaps
             * the arena wholesale - so a session placed inside it would
             * go away under a live holder.
             *
             * This is stated rather than left to fall out. The static
             * region now holds several job segments (see
             * arena_job_slots), so a session segment WOULD fit in it -
             * arithmetic no longer refuses what the teardown requires,
             * and this arm is the only thing that does. */
            from_arena = false;
            break;
        case PMIX_GDS_SHMEM3_MODEX_ID:
            from_arena = arena_alloc_modex(job, mapped_size, &arena_addr);
            break;
        default:
            from_arena = arena_alloc_static(job, mapped_size, &arena_addr);
            break;
    }

    if (from_arena) {
        PMIX_GDS_SHMEM3_VOUT(
            "%s: %s placed in arena at address=0x%zx",
            __func__, segment_name, (size_t)arena_addr
        );
        rc = pmix_shmem_segment_attach(
            shmem3, arena_addr, PMIX_SHMEM_MAP_OVER_RESERVATION,
            PMIX_GDS_SHMEM3_LAYOUT_ID
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            // Mapping over our own reservation cannot lose a race for the
            // address, so this is a real failure, not a lost hole.
            PMIX_ERROR_LOG(rc);
            goto out_release;
        }
    }
    else {
        // Find a hole in virtual memory and attach the segment there.
        //
        // The hole is located by scanning /proc/self/maps and is then claimed
        // by a separate mmap(); those two steps are not atomic. PMIx runs its
        // internal work on a single progress thread, so PMIx never races
        // itself here -- but the address space is process-wide, so other
        // activity in the process can take the located hole in between:
        // another thread, the dynamic loader (dlopen as components/namespaces
        // come up), the C allocator growing an arena via mmap, or
        // AddressSanitizer's own mappings. This is rare in general but
        // considerably more likely under ASAN and during MPI_Comm_spawn.
        // Because the server chooses this address (clients are later told
        // whichever one we land on), recover from a lost hole by selecting a
        // new one and retrying rather than failing the spawned job's
        // PMIx_Init.
        //
        // Note what this retry cannot do anything about: it re-picks an
        // address that is free *here*, and the client that has to map at the
        // same one is a different process. That is the failure the arena
        // above addresses, and reaching this path means we are back to
        // hoping - which is why the placement rule matters here most.
        //
        // Bypass shmem3_attach() here: on a failed map it reports a
        // client-style address mismatch and tears the job down, which is
        // wrong for the server, which chose the address and can simply try
        // another hole.
        enum { MAX_ATTACH_ATTEMPTS = 16 };
        /* Spread by namespace here too, for the same reason the arena
         * does. Segment id and attempt go into it as well: without them
         * this job's three segments would each be pointed at one address
         * and a retry would keep naming the one it just lost. */
        const uint64_t scatter =
            nspace_scatter(job->nspace_id) + (uint64_t)shmem3_id;
        for (int attempt = 1; ; ++attempt) {
            size_t base_addr = 0;
            rc = pmix_vmem_find_hole_scattered(
                segment_hole_kind(), scatter + (uint64_t)attempt,
                &base_addr, real_segsize
            );
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                goto out_release;
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
            // Only the "could not map at the requested address" case is
            // retryable; pmix_shmem_segment_attach() already detached its
            // failed attempt.
            if (PMIX_ERR_NOT_AVAILABLE != rc ||
                attempt >= MAX_ATTACH_ATTEMPTS) {
                PMIX_ERROR_LOG(rc);
                goto out_release;
            }
        }
    }
    pmix_gds_shmem3_set_status(job, shmem3_id, PMIX_GDS_SHMEM3_ATTACHED);
    /* Fix up the backing file's permissions. A failure here is reported
     * and then dropped, deliberately: the segment is created, mapped and
     * perfectly usable by US, and what a client loses is the ability to
     * open the backing file - which it reports as a failed attach and
     * answers by falling back to hash. Propagating it instead failed the
     * whole of register_job_info(), so no client got job data at all,
     * while leaving this segment attached and NOT marked MINE - so the
     * teardown below skipped its allocator context. Degrade, do not
     * break. */
    if (PMIX_UNLIKELY(PMIX_SUCCESS != shmem3_segment_fix_perms(job, shmem3))) {
        PMIX_GDS_SHMEM3_VOUT(
            "%s: could not set ownership/permissions on %s; a client that "
            "cannot open it will fall back to another GDS module",
            __func__, shmem3->backing_path
        );
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
    /* Mirror shmem3_attach()'s failure handling: detach and drop the job
     * tracker entry.
     *
     * Take the backing file with it. pmix_shmem_segment_create() made it
     * and shmem_destruct() only unlinks a segment it managed to ATTACH,
     * so a failure between the two left the file in the session tmpdir
     * for the life of the node - and the path is built from a pid, which
     * gets reused. */
    (void)pmix_shmem_segment_unlink(shmem3);
    (void)pmix_shmem_segment_detach(shmem3);
    drop_job_tracker(job);
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
    /* Jobs first: each holds a reference on its session, so this is what
     * lets the session list's own references be the last ones out. */
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem3_component.jobs);
    /* Any session the host never deregistered. Finalize is the backstop
     * for those, not the intended path - see del_session(). */
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem3_component.sessions);
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
     * this segment too, sized for the key count we pass to
     * pmix_keyindex_init() below - the two have to agree, since the
     * allocator cannot grow. On top of that each distinct key costs a
     * record and three copies of its own string. Bound the entry count
     * by nkvals - the real number of distinct keys is normally far
     * smaller, which is why the index is sized rather than fixed.
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
    seg_size += pmix_keyindex_sizeof_storage(nkvals);
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
    /* Reserve this job's address space before placing anything in it.
     * Everything below, and every modex this job ever produces, is then
     * carved from a range that clients will hold too - which is what a
     * fixed-address attach needs and cannot otherwise be given, because
     * the client that has to honor it does not exist yet. */
    arena_reserve(job, seg_size);
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
    /* Now the session. A named session is shared: point this job at the
     * one object every job in that session holds, creating it if this is
     * the first to describe it. An unnamed one (UINT32_MAX) stays the
     * private object the job was constructed with. */
    if (NULL == pmix_gds_shmem3_get_session_tracker(
                    job, pji->session_id, true)) {
        rc = PMIX_ERR_NOT_FOUND;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* A session that already has a published segment is one another job
     * in the same session put there. Nothing more to do: this job now
     * describes that segment to its clients rather than making a second
     * copy of it.
     *
     * Ask the CHAIN, not the build slot - the slot is emptied when a
     * segment is published, so it says nothing about what the session
     * already holds. */
    if (NULL != pmix_gds_shmem3_chain_head(&job->session->segments)) {
        return job_smdata_construct(job, htsize, pji->nkvals);
    }

    /* Note that we recycle the segment size calculated above because we
     * know that it will be at least as big as we need for this session
     * information. */
    const char *session_name =
        get_shmem3_session_name(pji->session_id, 0);
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
    rc = job_smdata_construct(job, htsize, pji->nkvals);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = session_smdata_construct(job, pji->session_id);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* If the host described this session through
     * PMIx_server_register_session, that description has been waiting for
     * a segment to live in - a session can be registered before any job
     * is running in it, and the segment is placed in, and named after, a
     * job. This is the first job, so write it now; the job's own session
     * array, if it carries one, then finds the session already described
     * and leaves it alone. */
    if (NULL != job->session->sinfo && !job->session->described) {
        pmix_value_t sval;
        rc = pmix_gds_base_wrap_session_info(
            job->session->id, job->session->sinfo, job->session->nsinfo, &sval
        );
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        rc = pmix_gds_shmem3_store_session_array(job, &sval);
        pmix_gds_base_release_session_info(&sval);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
        }
    }
    return rc;
}

static inline pmix_status_t
pack_shmem3_connection_info(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_gds_shmem3_seg_t *which,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM3_VVOUT(
        "%s:%s for peer (ID=%d) namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        peer->info->peerid, job->nspace_id
    );

    /* The modex is described from the newest published generation, not
     * from the build slot - see pack_shmem3_seg_blob(). Its delta-ness
     * comes from the same node, because that is a property of the
     * generation being described rather than of the job. */
    pmix_shmem_t *shmem3;
    bool is_delta = false;
    if (NULL != which) {
        /* a named segment on a chain - see pack_shmem3_seg_blob() */
        shmem3 = which->shmem3;
        is_delta = which->is_delta;
    }
    else if (PMIX_GDS_SHMEM3_MODEX_ID == shmem3_id) {
        pmix_gds_shmem3_seg_t *const head =
            pmix_gds_shmem3_chain_head(&job->modex_chain);
        if (PMIX_UNLIKELY(NULL == head)) {
            rc = PMIX_ERR_NOT_FOUND;
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        shmem3 = head->shmem3;
        is_delta = head->is_delta;
    }
    else {
        rc = pmix_gds_shmem3_get_job_shmem3_by_id(
            job, shmem3_id, &shmem3
        );
        if (PMIX_UNLIKELY(rc != PMIX_SUCCESS)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
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
        if (0 == job->arena_size) {
            // Nothing more to say - this job has no arena.
            break;
        }
        PMIX_DESTRUCT(&kv);
        /* Describe the arena. This rides on every seg blob rather than
         * only the first, so a client reserves it on the strength of
         * whichever one reaches it first and does not depend on the
         * order they were packed in. */
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_ARBS_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        nw = pmix_asprintf(
            &kv.value->data.string, "%zx", (size_t)job->arena_base
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
        PMIX_DESTRUCT(&kv);
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_ARSZ_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        nw = pmix_asprintf(&kv.value->data.string, "%zx", job->arena_size);
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
        kv.value->data.string = strdup(is_delta ? "1" : "0");
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

        /* The session id, on a session blob only. It says which session
         * tracker this segment belongs to, which a client cannot work out
         * from the segment because the answer decides whether to map it.
         * A job that named no session sends nothing, and its receiver
         * keeps the private tracker it was constructed with. */
        if (PMIX_GDS_SHMEM3_SESSION_ID != shmem3_id ||
            NULL == job->session || UINT32_MAX == job->session->id) {
            break;
        }
        PMIX_DESTRUCT(&kv);
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_SEG_SSID_KEY);
        kv.value = (pmix_value_t *)calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            break;
        }
        kv.value->type = PMIX_STRING;
        int nws = pmix_asprintf(
            &kv.value->data.string, "%zu", (size_t)job->session->id
        );
        if (PMIX_UNLIKELY(nws == -1)) {
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
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_ARBS_KEY)) {
            rc = strtost(val, 16, &usb->arena_base);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_ARSZ_KEY)) {
            rc = strtost(val, 16, &usb->arena_size);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, SHMEM3_SEG_SSID_KEY)) {
            /* Decimal, as SMID is; only a session blob carries it, and
             * absent means the sending job named no session. */
            size_t st_ssid;
            rc = strtost(val, 10, &st_ssid);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            usb->ssid = (uint32_t)st_ssid;
        }
        else {
            /* A key we have not been taught to hear. Skip it.
             *
             * This blob grows the way every other PMIx wire structure
             * grows - by appending - and the project's own rule is that
             * a reader tolerates what it does not recognize. Refusing
             * the whole blob instead makes every future addition a flag
             * day: a server that has learned to send one more field
             * cannot talk to a client that has not yet learned to read
             * it, and the client does not degrade, it fails PMIx_Init.
             *
             * That is not hypothetical - it is what a newer server
             * describing its address-space arena to an older client
             * does, and the xversion CI job is where it shows up. The
             * fields such a client skips are ones it has no use for by
             * construction: it does not know the feature they describe,
             * so it does exactly what it did before they existed. */
            PMIX_GDS_SHMEM3_VOUT(
                "%s: ignoring unrecognized seg blob key=%s",
                __func__, (NULL == kv.key) ? "(null)" : kv.key
            );
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
            /* See if this is the job's session ID. If so, capture it.
             *
             * Only look inside an array whose elements really are
             * pmix_info_t. A host is free to hand us a PMIX_DATA_ARRAY of
             * anything - procs, strings, scalars - and reading element
             * zero of one of those as a pmix_info_t makes PMIX_CHECK_KEY
             * walk up to PMIX_MAX_KEYLEN bytes off the end of an
             * allocation that may be eight bytes long. */
            if (PMIX_INFO == kvi->value->data.darray->type) {
                pmix_info_t *info;
                info = (pmix_info_t *)kvi->value->data.darray->array;
                if (PMIX_CHECK_KEY(&info[0], PMIX_SESSION_ID)) {
                    rc = PMIx_Value_get_number(
                        &info[0].value, &sid, PMIX_UINT32
                    );
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                        PMIX_ERROR_LOG(rc);
                        goto out;
                    }
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

/* Pack one blob describing one segment. */
static pmix_status_t
pack_one_seg_blob(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_gds_shmem3_seg_t *which,
    struct pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_buffer_t buff;

    do {
        PMIX_CONSTRUCT(&buff, pmix_buffer_t);

        rc = pack_shmem3_connection_info(
            job, shmem3_id, which, peer, &buff
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

/* Pack a whole chain, OLDEST first.
 *
 * Order is load-bearing: the client publishes each segment as it
 * attaches it, so describing them oldest-first leaves its chain in the
 * same order as ours - newest at the head, which is where a read starts.
 * Recursive because the chain runs the other way and its depth is the
 * number of updates, which is small. */
static pmix_status_t
pack_seg_chain(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    pmix_gds_shmem3_seg_t *seg,
    struct pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc;

    if (NULL == seg) {
        return PMIX_SUCCESS;
    }
    rc = pack_seg_chain(job, shmem3_id, seg->prior, peer, reply);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    return pack_one_seg_blob(job, shmem3_id, seg, peer, reply);
}

static inline pmix_status_t
pack_shmem3_seg_blob(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_job_shmem3_id_t shmem3_id,
    struct pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    /* Only describe a segment that is ready for use - anything else is
     * half-built and must not be shared with a client.
     *
     * The modex and the session are chains: the build slot holds
     * whatever is being assembled now, which is exactly what a client
     * must not be pointed at, so what gets described is what has been
     * published. A session ships its WHOLE chain, because an update
     * carries only what changed and a client needs the segments behind
     * it to answer for everything else. The modex ships only its newest,
     * which is what it has always done - a client already holding older
     * generations keeps them, and one attaching now is told about the
     * rest at its next fence. */
    if (PMIX_GDS_SHMEM3_MODEX_ID == shmem3_id) {
        pmix_gds_shmem3_seg_t *const head =
            pmix_gds_shmem3_chain_head(&job->modex_chain);
        if (NULL == head) {
            return PMIX_SUCCESS;
        }
        return pack_one_seg_blob(job, shmem3_id, head, peer, reply);
    }
    if (PMIX_GDS_SHMEM3_SESSION_ID == shmem3_id) {
        if (NULL == job->session) {
            return PMIX_SUCCESS;
        }
        return pack_seg_chain(
            job, shmem3_id,
            pmix_gds_shmem3_chain_head(&job->session->segments),
            peer, reply
        );
    }
    /* The job's data is a chain too - a host adding to it after the job
     * was registered publishes a segment carrying what is new - so ship
     * all of it, oldest first. */
    return pack_seg_chain(
        job, shmem3_id,
        pmix_gds_shmem3_chain_head(&job->job_chain),
        peer, reply
    );
}

/* Pack this job's tombstones for a connecting client - see
 * SHMEM3_TOMBSTONE_KEY. Packs nothing, successfully, for the ordinary
 * job nobody has deleted from. */
static pmix_status_t
pack_tombstones(
    pmix_gds_shmem3_job_t *job,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_gds_shmem3_tombstone_t *t;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_kval_t kv;

    for (t = atomic_load_explicit(&job->tombstones, memory_order_acquire);
         NULL != t; t = t->prior) {
        if (NULL == t->key) {
            continue;
        }
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = strdup(SHMEM3_TOMBSTONE_KEY);
        kv.value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv.key || NULL == kv.value)) {
            PMIX_DESTRUCT(&kv);
            return PMIX_ERR_NOMEM;
        }
        kv.value->type = PMIX_STRING;
        if (0 > pmix_asprintf(&kv.value->data.string, "%s:%u:%s",
                              job->nspace_id, (unsigned) t->rank, t->key)) {
            PMIX_DESTRUCT(&kv);
            return PMIX_ERR_NOMEM;
        }
        PMIX_BFROPS_PACK(rc, peer, buffer, &kv, 1, PMIX_KVAL);
        PMIX_DESTRUCT(&kv);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return PMIX_SUCCESS;
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
    // Anything this job has stopped answering for. The segments still
    // contain it, so a client attaching now has to be told, or it would
    // read what every already-attached process has been told to ignore.
    rc = pack_tombstones(job, me, job->conni);
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

/* Did this delivery refuse a segment? See
 * client_connect_to_shmem3_from_buffi(), which resets it. */
static bool shmem3_delivery_refused = false;

static pmix_status_t
unpack_shmem3_seg_blob_and_attach_if_necessary(
    pmix_kval_t *kvbo,
    pmix_gds_shmem3_attach_ctx_t ctx
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
        /* Everything below assumes the blob described a segment this
         * build knows how to place, and named the job and the file. Only
         * the sender guarantees that, and the sender may be a different
         * release: an id outside our enum reaches the default arm of
         * pmix_gds_shmem3_get_job_shmem3_by_id(), which abort()s, and a
         * missing nspace reaches strcmp() with NULL.
         *
         * Skip such a blob rather than failing - it is the same forward
         * compatibility the unrecognized-key arm of the unpacker keeps,
         * one level up. A segment kind we have never heard of is one we
         * have no use for by construction. */
        if (PMIX_GDS_SHMEM3_JOB_ID != usb.smid &&
            PMIX_GDS_SHMEM3_SESSION_ID != usb.smid &&
            PMIX_GDS_SHMEM3_MODEX_ID != usb.smid) {
            PMIX_GDS_SHMEM3_VOUT(
                "%s: ignoring seg blob for unrecognized segment id=%u",
                __func__, (unsigned)usb.smid
            );
            break;
        }
        if (NULL == usb.nsid || NULL == usb.seg_path) {
            rc = PMIX_ERR_BAD_PARAM;
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
        /* Claim the job's arena the first time we are told about it.
         *
         * This is the moment that matters. We are inside PMIx_Init, the
         * address space is nearly as empty as it will ever be, and the
         * range we are claiming is the one the server will place THIS
         * job's later segments in - including the modex, which does not
         * exist yet and will be handed to us at a fence, by which time
         * this process is full of whatever it went on to load. Taking the
         * address now is what makes that later attach a certainty rather
         * than a coin toss.
         *
         * Failing is survivable and deliberately quiet: without the
         * reservation each attach falls back to asking for its address
         * and hoping, which is exactly what this component did before. */
        if (0 != usb.arena_size && 0 == job->arena_size) {
            if (PMIX_SUCCESS == pmix_vmem_reserve_at(
                    (uintptr_t)usb.arena_base, usb.arena_size)) {
                job->arena_base = (uintptr_t)usb.arena_base;
                job->arena_size = usb.arena_size;
                PMIX_GDS_SHMEM3_VOUT(
                    "%s: reserved arena [0x%zx, 0x%zx) for namespace=%s",
                    __func__, usb.arena_base,
                    usb.arena_base + usb.arena_size, usb.nsid
                );
            }
            else {
                PMIX_GDS_SHMEM3_VOUT(
                    "%s: could not reserve arena [0x%zx, 0x%zx) for "
                    "namespace=%s; attaching without one",
                    __func__, usb.arena_base,
                    usb.arena_base + usb.arena_size, usb.nsid
                );
            }
        }
        /* A session blob that names a session belongs to a shared
         * tracker. Point this job at the one object every local job in
         * that session holds, so a second namespace on this node maps
         * the segment once between them rather than once apiece - and so
         * the already-attached test just below sees the mapping the
         * first of them made.
         *
         * Creating it here is the point of carrying the id on the wire:
         * this is the first anyone on a client hears of the session, and
         * the id could not have come out of the segment, because whether
         * to map the segment is the question being answered. */
        if (PMIX_GDS_SHMEM3_SESSION_ID == usb.smid && UINT32_MAX != usb.ssid) {
            if (NULL == pmix_gds_shmem3_get_session_tracker(
                            job, usb.ssid, true)) {
                rc = PMIX_ERR_NOT_FOUND;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        /* Make sure we aren't already attached to the given shmem3.
         *
         * A published modex generation is on the chain rather than in
         * the build slot, so ask the chain for it: the slot is empty
         * between generations, and a blob we already acted on would
         * otherwise look new and be mapped a second time. The same blob
         * does arrive more than once - mark_modex_complete() sends one
         * to every local peer. */
        pmix_shmem_t *cur = NULL;
        bool held = false;
        {
            pmix_gds_shmem3_seg_t *seg = NULL;
            if (PMIX_GDS_SHMEM3_MODEX_ID == usb.smid) {
                seg = pmix_gds_shmem3_chain_head(&job->modex_chain);
            }
            else if (PMIX_GDS_SHMEM3_JOB_ID == usb.smid) {
                seg = pmix_gds_shmem3_chain_head(&job->job_chain);
            }
            else if (NULL != job->session) {
                seg = pmix_gds_shmem3_chain_head(&job->session->segments);
            }
            if (NULL != seg) {
                cur = seg->shmem3;
                held = true;
            }
            /* The WHOLE chain, not just its head: a session ships every
             * segment it has, so a client rebuilding one walks past
             * blobs it already holds. Matching only the head would map
             * each of those a second time. */
            for (; NULL != seg; seg = seg->prior) {
                if (NULL != seg->shmem3 &&
                    0 == strcmp(seg->shmem3->backing_path, usb.seg_path)) {
                    cur = seg->shmem3;
                    break;
                }
            }
        }
        if (held) {
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
                /* Nothing to set aside: the generation we hold was
                 * published onto the chain as soon as it was attached,
                 * and the build slot has been empty since. It stays on
                 * the chain whatever arrives now - a generation is never
                 * unmapped, so the chain only grows and a read walking
                 * it cannot have a segment taken away underneath, which
                 * is what lets the walk run with no lock. A cumulative
                 * contribution makes the older generations redundant
                 * rather than wrong: the walk stops at the newest
                 * segment holding the key, and a deleted key is shadowed
                 * by the PMIX_UNDEF entry the newer one carries. */
                /* We are about to map a generation we have not held
                 * before, which is the same step the server counts on
                 * its side. Both have to count it: the number is what
                 * dates a tombstone, and one that never moves makes
                 * every tombstone shadow every generation. */
                advance_modex_generation(job);
            }
            /* Every kind grows a chain now, so there is no arm here
               that has to refuse one. */
        }
        // Looks like we have to attach and initialize it.
        rc = shmem3_segment_attach_and_init(job, &usb, ctx);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            /* A segment we could not map after PMIx_Init makes for a
             * slower client, not a broken one, and there is no "next
             * option" to take by then anyway. PMIX_GDS_RECV_MODEX_COMPLETE
             * resolves one module and the modex cannot be re-delivered in
             * another's format; an update to job or session data arrives
             * on a one-way notification with no re-request behind it at
             * all. Reporting the failure upward put
             * PMIX_ERR_TAKE_NEXT_OPTION in the application's hands as the
             * return of PMIx_Fence, where it was either checked and
             * treated as a failed fence or - more often - not checked at
             * all. Say what happened and carry on; the data is still
             * reachable, one request to the server at a time.
             * See openpmix#4156. */
            if (PMIX_ERR_TAKE_NEXT_OPTION == rc &&
                PMIX_GDS_SHMEM3_ATTACH_UPDATE == ctx) {
                PMIX_GDS_SHMEM3_VOUT(
                    "%s: could not map the %s segment for namespace=%s; "
                    "this process will fetch what it holds from its "
                    "server instead of reading it from shared memory",
                    __func__, get_shmem3_id_name(usb.smid), usb.nsid
                );
                shmem3_delivery_refused = true;
                rc = PMIX_SUCCESS;
                break;
            }
            break;
        }
        /* Attached. If nothing has been refused in this delivery, the
         * client now holds everything the server described - the whole
         * chain is sent on every notice and the client skips what it
         * already has - so this realm may answer locally again. */
        if (PMIX_GDS_SHMEM3_ATTACH_UPDATE == ctx && !shmem3_delivery_refused) {
            if (PMIX_GDS_SHMEM3_SESSION_ID == usb.smid) {
                if (NULL != job->session) {
                    job->session->chain_incomplete = false;
                }
            } else if (PMIX_GDS_SHMEM3_MODEX_ID != usb.smid) {
                job->chain_incomplete = false;
            }
        }
    } while (false);
    PMIX_DESTRUCT(&usb);

    return rc;
}

static pmix_status_t
del_key(
    const pmix_proc_t *proc,
    const char *key
);

/* Record a key the server has stopped answering for, from the job-info
 * reply. The value is "<nspace>:<rank>:<key>" - the key may itself
 * contain a colon, so only the first two separators are significant. */
static pmix_status_t
unpack_tombstone(
    pmix_kval_t *kv
) {
    char *sep1, *sep2;
    pmix_proc_t proc;
    unsigned long rank;

    if (NULL == kv->value || PMIX_STRING != kv->value->type
        || NULL == kv->value->data.string) {
        return PMIX_ERR_TYPE_MISMATCH;
    }
    sep1 = strchr(kv->value->data.string, ':');
    if (NULL == sep1) {
        return PMIX_ERR_BAD_PARAM;
    }
    sep2 = strchr(sep1 + 1, ':');
    if (NULL == sep2 || sep2 == sep1 + 1) {
        return PMIX_ERR_BAD_PARAM;
    }
    *sep1 = '\0';
    *sep2 = '\0';
    rank = strtoul(sep1 + 1, NULL, 10);
    PMIX_LOAD_PROCID(&proc, kv->value->data.string, (pmix_rank_t) rank);
    return del_key(&proc, sep2 + 1);
}

static pmix_status_t
client_connect_to_shmem3_from_buffi(
    pmix_buffer_t *buff,
    pmix_gds_shmem3_attach_ctx_t ctx
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    pmix_status_t rc = PMIX_SUCCESS;

    /* Nothing refused YET in this delivery. A segment that is refused
     * sets this, and it stays set for the rest of the delivery, so a
     * segment attaching after one was refused does not get to say the
     * chain is whole again. Progress-thread only: both callers -
     * store_job_info() and client_accept_update() - run there. */
    shmem3_delivery_refused = false;

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
            rc = unpack_shmem3_seg_blob_and_attach_if_necessary(&kval, ctx);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kval, SHMEM3_TOMBSTONE_KEY)) {
            rc = unpack_tombstone(&kval);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                break;
            }
        }
        else {
            PMIX_GDS_SHMEM3_VOUT(
                "%s:ERROR unexpected key=%s", __func__,
                (NULL == kval.key) ? "(null)" : kval.key
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
    //
    // This is the one delivery a failed attach can answer by falling
    // back to another module, because PMIx_Init re-requests the job data
    // and the server can re-register it in that module's format.
    return client_connect_to_shmem3_from_buffi(
        buff, PMIX_GDS_SHMEM3_ATTACH_INIT
    );
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
     * in this segment too, sized for the same nkvals we hand
     * modex_smdata_construct() - the two have to agree, since the
     * allocator cannot grow. Each distinct key then costs a record plus
     * three copies of its own string. We cannot know the number of
     * distinct keys without unpacking, so bound it by nkvals; the real
     * count is normally far smaller.
     *
     * The key *lengths* are bounded by the blob, not by PMIX_MAX_KEYLEN.
     * Every key here arrived in the buffer being unpacked, so the key
     * strings cannot total more than that buffer decompresses to.
     * Reserving 511 bytes for each instead made this one term roughly
     * fifty times the payload - by far the largest thing in this
     * estimate, and the reason a 1.3 MB modex was getting an 80 MB
     * segment. */
    segment_size += pmix_keyindex_sizeof_storage(nkvals);
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
        .num_ht_elements = nhtelems,
        .nkvals = nkvals
    };
    return result;
}

/**
 * Stop answering for this key.
 *
 * The data itself stays where it is: it lives in a shared segment that
 * local clients have mapped, and such a segment is never written again.
 * The removal is recorded instead, and every read consults the record.
 *
 * Called on the server when a deregistration or a client's delete
 * retracts a key, and on each client when its server passes that on -
 * so every process holding the segment ends up with the same record. A
 * client that attaches afterwards is given the list in the job-info
 * reply, which is why the cached reply is dropped here.
 */
static pmix_status_t
del_key(
    const pmix_proc_t *proc,
    const char *key
) {
    pmix_gds_shmem3_job_t *job;
    pmix_gds_shmem3_tombstone_t *t;
    pmix_status_t rc;

    if (NULL == proc || NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* do not create a tracker for a namespace we know nothing about -
     * there is no data of ours for the key to shadow */
    rc = pmix_gds_shmem3_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        return PMIX_SUCCESS;
    }
    const uint32_t generation =
        atomic_load_explicit(&job->modex_generation, memory_order_relaxed);

    /* Already shadowed at or past this generation? Then recording it
     * again would not change any answer - tombstoned() is satisfied by
     * ANY matching entry, so the one already there says everything a
     * new one would. This is the only reason to look before publishing;
     * it is a pure read, and it is what keeps a key deleted repeatedly
     * from growing the chain without bound.
     *
     * Only the progress thread writes here, so no lock is needed to
     * make the look-then-publish safe against another writer, and a
     * reader cannot see a node before it is whole. */
    for (t = atomic_load_explicit(&job->tombstones, memory_order_acquire);
         NULL != t; t = t->prior) {
        if (t->rank == proc->rank && NULL != t->key &&
            0 == strcmp(t->key, key) && generation <= t->generation) {
            return PMIX_SUCCESS;
        }
    }
    t = calloc(1, sizeof(*t));
    if (PMIX_UNLIKELY(NULL == t)) {
        return PMIX_ERR_NOMEM;
    }
    t->rank = proc->rank;
    t->key = strdup(key);
    if (PMIX_UNLIKELY(NULL == t->key)) {
        free(t);
        return PMIX_ERR_NOMEM;
    }
    t->generation = generation;
    /* Complete before it is published; the release-store is what makes
     * that so for the reader that acquire-loads the head. */
    t->prior = atomic_load_explicit(&job->tombstones, memory_order_relaxed);
    atomic_store_explicit(&job->tombstones, t, memory_order_release);

    /* the cached job-info reply still says the key exists, and it is
     * what the next client to attach is handed - build a fresh one */
    if (NULL != job->conni) {
        PMIX_RELEASE(job->conni);
        job->conni = NULL;
    }
    return PMIX_SUCCESS;
}

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
        /* Every blob is stored, so this generation is whole - publish it.
         * A reader sees it here for the first time, complete. */
        pmix_gds_shmem3_set_status(
            job, PMIX_GDS_SHMEM3_MODEX_ID, PMIX_GDS_SHMEM3_READY_FOR_USE
        );
        return publish_modex_generation(job);
    }

    PMIX_GDS_SHMEM3_VOUT(
        "%s:%s for namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        proc->nspace
    );

    const bool attached = pmix_gds_shmem3_has_status(
        job, PMIX_GDS_SHMEM3_MODEX_ID, PMIX_GDS_SHMEM3_ATTACHED
    );
    /* An empty build slot means this blob starts a generation - either
     * the first, or a new one after the last was published.
     *
     * A finished segment has been advertised and local clients have it
     * mapped, so it must never be written again: storing into it can
     * rehash the table and reallocate the key index underneath a reader,
     * and it was sized for the data that built it, so a larger second
     * modex overruns the allocator and aborts the server. Every
     * generation therefore gets a segment of its own, and they are read
     * back through the chain - see modex_fetch() in gds_shmem3_fetch.c
     * and openpmix#4087; examples/modex_twice.c is the canary. */
    if (!attached) {
        /* A generation we finished is already on the chain - it was
         * published the moment it completed, and the build slot has been
         * empty since. Its existence is what says this is not the first
         * one, and therefore that the counter has to move: the number
         * names the next backing file and dates a tombstone. */
        if (NULL != pmix_gds_shmem3_chain_head(&job->modex_chain)) {
            PMIX_GDS_SHMEM3_VOUT(
                "%s: modex generation %u complete; starting %u for namespace=%s",
                __func__, job->modex_generation, job->modex_generation + 1,
                proc->nspace
            );
            advance_modex_generation(job);
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

        rc = modex_smdata_construct(job, minfo.num_ht_elements, minfo.nkvals);
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
        /* An entry whose value is PMIX_UNDEF is the contributing process
         * saying the key is gone. It is stored like anything else - this
         * component cannot take a key out of a segment its clients have
         * mapped, so the read path steps over it instead - but our own
         * local clients cached the key when they read it, and their
         * copies are in their own hash tables where nothing here reaches.
         * Tell them, exactly as gds/hash does when it applies one.
         *
         * Our own process holds a second copy as well, and it is not in
         * a segment. A direct modex - a client asking for a peer this
         * server has no fence data for - caches the answer through
         * pmix_globals.mypeer, which is pinned to "hash", so it lands in
         * that module's remote table (pmix_server_dmodex.c). Nothing
         * about storing into a shmem3 segment reaches it, and it is the
         * copy that answers the next such request: leaving it in place
         * let the server re-serve the deleted key to the very client it
         * had just told to drop it, which the client then cached again. */
        if (NULL != kv.value && PMIX_UNDEF == kv.value->type
            && PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
            pmix_proc_t dproc;
            pmix_kval_t dkv;
            pmix_status_t drc;

            PMIX_LOAD_PROCID(&dproc, proc->nspace,
                             (PMIX_RANK_UNDEF == rank) ? 0 : rank);
            /* a view of the key, so it must not be destructed */
            dkv.key = kv.key;
            dkv.value = NULL;
            PMIX_GDS_STORE_KV(drc, pmix_globals.mypeer, &dproc,
                              PMIX_DEL_REMOTE, &dkv);
            if (PMIX_SUCCESS != drc) {
                PMIX_ERROR_LOG(drc);
            }
            pmix_server_notify_deleted(&dproc, PMIX_DEL_REMOTE, kv.key, NULL);
        }
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

    /* These come from the host environment, which is not this project, so
     * the key does not guarantee the type. Reading the union directly
     * turned a PMIX_STRING into the low half of a pointer, and the
     * resulting garbage id was not merely recorded - it set chown/chgrp,
     * so shmem3_segment_fix_perms() later handed it to chown() on this
     * job's backing files. Ask for a number and let the conversion fail. */
    for (size_t i = 0; i < ninfo; ++i) {
        if (PMIX_CHECK_KEY(&info[i], PMIX_USERID)) {
            uint32_t nuid;
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[i].value, &nuid,
                                                      PMIX_UINT32)) {
                PMIX_GDS_SHMEM3_VOUT(
                    "%s: ignoring nspace=%s " PMIX_USERID " of type %s",
                    __func__, nspace, PMIx_Data_type_string(info[i].value.type)
                );
                continue;
            }
            PMIX_GDS_SHMEM3_VOUT(
                "%s: updating nspace=%s UID from %zd to %zd",
                __func__, nspace, (size_t)job->uid, (size_t)nuid
            );
            job->uid = (uid_t)nuid;
            job->chown = true;
        }
        else if (PMIX_CHECK_KEY(&info[i], PMIX_GRPID)) {
            uint32_t ngid;
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[i].value, &ngid,
                                                      PMIX_UINT32)) {
                PMIX_GDS_SHMEM3_VOUT(
                    "%s: ignoring nspace=%s " PMIX_GRPID " of type %s",
                    __func__, nspace, PMIx_Data_type_string(info[i].value.type)
                );
                continue;
            }
            PMIX_GDS_SHMEM3_VOUT(
                "%s: updating nspace=%s GID from %zd to %zd",
                __func__, nspace, (size_t)job->gid, (size_t)ngid
            );
            job->gid = (gid_t)ngid;
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
    return client_connect_to_shmem3_from_buffi(
        buff, PMIX_GDS_SHMEM3_ATTACH_UPDATE
    );
}

/* What does the session's chain currently answer for this key?
 *
 * Newest-first, stopping at the first segment carrying it, which is the
 * same rule a read follows - so this compares against what a client
 * would actually see rather than against any one segment. */
static pmix_kval_t *
session_current_value(
    pmix_gds_shmem3_session_t *sesh,
    const char *key
) {
    pmix_gds_shmem3_seg_t *seg;

    for (seg = pmix_gds_shmem3_chain_head(&sesh->segments);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_session_data_t *const sd = seg->smdata;
        pmix_kval_t *kvi;
        if (NULL == sd) {
            continue;
        }
        PMIX_LIST_FOREACH (kvi, sd->sessioninfo, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kvi, key)) {
                return kvi;
            }
        }
    }
    return NULL;
}

/* Reduce a description to what has actually changed.
 *
 * A host describing a session mostly restates what we already hold -
 * every job registration in a session carries the same session array,
 * and a host may re-register a session without changing it. Publishing
 * a segment for that would cost one per restatement to say nothing.
 *
 * So compare each entry against what the chain answers today and keep
 * only the ones that differ or are new. Returns the number kept, with
 * *out pointing at borrowed entries of the caller's array - no copy,
 * because the segment build copies what it stores.
 *
 * A NULL-key entry, or the leading PMIX_SESSION_ID, is skipped: the id
 * identifies the session rather than describing it.
 */
static size_t
session_changed_entries(
    pmix_gds_shmem3_session_t *sesh,
    pmix_info_t info[],
    size_t ninfo,
    pmix_info_t **out
) {
    pmix_info_t *kept;
    size_t nkept = 0;

    *out = NULL;
    if (0 == ninfo || NULL == info) {
        return 0;
    }
    kept = (pmix_info_t *) calloc(ninfo, sizeof(pmix_info_t));
    if (NULL == kept) {
        return 0;
    }
    for (size_t n = 0; n < ninfo; n++) {
        if (0 == strlen(info[n].key) ||
            PMIX_CHECK_KEY(&info[n], PMIX_SESSION_ID)) {
            continue;
        }
        pmix_kval_t *const cur = session_current_value(sesh, info[n].key);
        if (NULL != cur &&
            PMIX_EQUAL == PMIx_Value_compare(cur->value,
                                             (pmix_value_t *) &info[n].value)) {
            continue;
        }
        /* Shallow: the caller owns these, and the segment build copies
         * whatever it stores into shared memory. */
        memcpy(&kept[nkept], &info[n], sizeof(pmix_info_t));
        nkept++;
    }
    if (0 == nkept) {
        free(kept);
        return 0;
    }
    *out = kept;
    return nkept;
}

/* Publish a changed session description as a NEW segment.
 *
 * A session's resources change under it - PMIX_SESSION_EXTEND grows one
 * "in terms of time or resources", PMIX_SESSION_PREEMPT takes resources
 * back, a node goes down - so PMIX_UNIV_SIZE and the session's node set
 * are values a host restates. The segment already published cannot be
 * rewritten: local clients have it mapped, and a segment a client can
 * see is never written again. So the change goes into a segment of its
 * own at the head of the chain, and a read walking newest-first sees it
 * before the value it replaces.
 *
 * Only the keys the host restated are in the new segment. Everything it
 * did not mention is still answered by the segments behind it, which is
 * what makes an update cheap: a session that gained a node ships a node
 * array, not the whole session.
 *
 * Needs a job in the session, because a segment is placed in - and named
 * after - one. A host that updates a session with nothing running in it
 * has its description held on the tracker instead, and the first job to
 * arrive builds from that.
 */
static pmix_status_t
publish_session_update(
    pmix_gds_shmem3_session_t *sesh,
    pmix_gds_shmem3_job_t *job,
    pmix_info_t info[],
    size_t ninfo
) {
    pmix_status_t rc;
    pmix_value_t sval;

    rc = pmix_gds_base_wrap_session_info(sesh->id, info, ninfo, &sval);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* Size it for what this update carries rather than for the session.
     * The floor is what an empty segment costs - its own header, a key
     * index sized for these keys, and the two lists - so a small update
     * is a small segment. */
    size_t seg_size = sizeof(pmix_gds_shmem3_shared_session_data_t);
    seg_size += pmix_keyindex_sizeof_storage(ninfo);
    seg_size += 2 * sizeof(pmix_list_t);
    seg_size += ninfo * (sizeof(pmix_kval_t) + sizeof(pmix_value_t)
                         + PMIX_MAX_KEYLEN + 1);
    /* node arrays are stored as whole nodeinfo objects, and a session
     * that grew ships one per node - charge for them generously rather
     * than walk the array here */
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO_ARRAY) &&
            PMIX_DATA_ARRAY == info[n].value.type &&
            NULL != info[n].value.data.darray) {
            const size_t nsub = info[n].value.data.darray->size;
            seg_size += sizeof(pmix_gds_shmem3_nodeinfo_t)
                        + 2 * sizeof(pmix_list_t)
                        + nsub * (sizeof(pmix_kval_t) + sizeof(pmix_value_t)
                                  + PMIX_MAX_KEYLEN + 1);
        }
    }
    seg_size *= 4;   // the same empirical fluff the other estimates carry
    seg_size *= pmix_gds_shmem3_segment_size_multiplier;

    const char *name = get_shmem3_session_name(sesh->id, sesh->generation);
    if (PMIX_UNLIKELY(NULL == name)) {
        rc = PMIX_ERROR;
        goto out;
    }
    rc = shmem3_segment_create_and_attach(
        job, PMIX_GDS_SHMEM3_SESSION_ID, name, seg_size
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    rc = session_smdata_construct(job, sesh->id);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    rc = pmix_gds_shmem3_store_session_info(job, sesh, &sval);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    rc = pmix_gds_shmem3_publish_session_segment(sesh);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    /* The cached job-info replies still describe the chain as it was,
     * and they are what the next client to attach is handed. */
    PMIX_LIST_FOREACH (job, &pmix_mca_gds_shmem3_component.jobs,
                       pmix_gds_shmem3_job_t) {
        if (job->session == sesh && NULL != job->conni) {
            PMIX_RELEASE(job->conni);
            job->conni = NULL;
        }
    }
    PMIX_GDS_SHMEM3_VOUT(
        "%s: session %u published update generation %u",
        __func__, (unsigned) sesh->id, (unsigned)(sesh->generation - 1)
    );
out:
    pmix_gds_base_release_session_info(&sval);
    return rc;
}

/* Apply a session description from either source.
 *
 * There are two, and they are NOT distinguished here:
 *
 *   - PMIx_server_register_session, the host stating the session's
 *     description directly;
 *   - a PMIX_SESSION_INFO_ARRAY inside a job registration, which is how
 *     every host described a session before that API existed and how
 *     hosts that have not adopted it still do.
 *
 * Treating the second as "a job merely naming its session" and ignoring
 * it would mean a host that never adopts the new call could never update
 * a session at all - and those hosts will be around for a long time. So
 * both go through here, and what decides whether anything happens is
 * whether the description actually differs from what we already hold.
 * A restatement of what the session already says costs nothing.
 */
pmix_status_t
pmix_gds_shmem3_session_describe(
    pmix_gds_shmem3_session_t *sesh,
    pmix_gds_shmem3_job_t *job,
    pmix_info_t info[],
    size_t ninfo
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_info_t *changed = NULL;
    size_t nchanged;

    if (NULL == sesh || 0 == ninfo || NULL == info) {
        return PMIX_SUCCESS;
    }

    /* Nothing published yet: hold the description for the first job in
     * the session to build its segment from. Replace whatever was held,
     * so a job arriving later builds one segment carrying the current
     * description rather than replaying every step that got there. */
    if (NULL == pmix_gds_shmem3_chain_head(&sesh->segments)) {
        if (NULL != sesh->sinfo) {
            PMIX_INFO_FREE(sesh->sinfo, sesh->nsinfo);
            sesh->sinfo = NULL;
            sesh->nsinfo = 0;
        }
        PMIX_INFO_CREATE(sesh->sinfo, ninfo);
        if (PMIX_UNLIKELY(NULL == sesh->sinfo)) {
            return PMIX_ERR_NOMEM;
        }
        sesh->nsinfo = ninfo;
        for (size_t n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&sesh->sinfo[n], &info[n]);
        }
        return PMIX_SUCCESS;
    }

    /* Only what differs from what the chain answers today. */
    nchanged = session_changed_entries(sesh, info, ninfo, &changed);
    if (0 == nchanged) {
        PMIX_GDS_SHMEM3_VOUT(
            "%s: session %u restated nothing new; no segment published",
            __func__, (unsigned) sesh->id
        );
        return PMIX_SUCCESS;
    }

    /* A segment is placed in, and named after, a job - any job in the
     * session will do, since the segment belongs to the session. */
    if (NULL == job) {
        PMIX_LIST_FOREACH (job, &pmix_mca_gds_shmem3_component.jobs,
                           pmix_gds_shmem3_job_t) {
            if (job->session == sesh) {
                break;
            }
        }
    }
    if (NULL == job || job->session != sesh) {
        /* A published session with no job left in it. Nothing can be
         * reading it either, so there is nobody to tell. */
        free(changed);
        return PMIX_SUCCESS;
    }

    rc = publish_session_update(sesh, job, changed, nchanged);
    free(changed);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* Clients already attached hold what the session said when they
     * attached, so they have to be told - otherwise a job launched after
     * this change sees the new value and one already running does not.
     *
     * Every local client is told, not just those in this session: a
     * session spans namespaces, so there is no one nspace to name here.
     * The cost of telling one whose session did not change is a pack it
     * already holds and a walk that skips every segment - and a session
     * changes when an allocation grows or shrinks, not in any loop.
     * Both paths into this function end here, so a description arriving
     * through a job registration reaches attached clients exactly as one
     * from PMIx_server_register_session does. */
    pmix_server_notify_gds_update(NULL);
    return PMIX_SUCCESS;
}

static pmix_status_t
server_add_session(
    uint32_t sessionID,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();

    /* Establish the tracker even with nothing to record: a session that
     * exists and carries no attributes is still a session, and a job
     * naming it later has to find this one rather than register a second
     * beside it. */
    pmix_gds_shmem3_session_t *sesh;
    sesh = pmix_gds_shmem3_find_session(sessionID, true);
    if (PMIX_UNLIKELY(NULL == sesh)) {
        return PMIX_ERR_NOMEM;
    }
    if (0 == ninfo || NULL == info) {
        return PMIX_SUCCESS;
    }

    /* The host stating the description. Same path as a job registration
     * carrying one - see pmix_gds_shmem3_session_describe(). */
    return pmix_gds_shmem3_session_describe(sesh, NULL, info, ninfo);
}

static pmix_status_t
server_del_session(
    uint32_t sessionID
) {
    pmix_gds_shmem3_session_t *sesh;

    PMIX_GDS_SHMEM3_VVOUT_HERE();

    PMIX_LIST_FOREACH (sesh, &pmix_mca_gds_shmem3_component.sessions,
                       pmix_gds_shmem3_session_t) {
        if (sesh->id != sessionID) {
            continue;
        }
        /* Drop the list's reference only. Jobs still registered in this
         * session hold their own, and their clients have the segment
         * mapped, so the mapping survives until the last of them is
         * deregistered - which lets a host end a session and tear its
         * jobs down in either order. */
        pmix_list_remove_item(
            &pmix_mca_gds_shmem3_component.sessions, &sesh->super
        );
        PMIX_RELEASE(sesh);
        return PMIX_SUCCESS;
    }
    /* Not an error. A host deregisters a session across every daemon,
     * and this one may have had no job in it. */
    return PMIX_SUCCESS;
}

/* Pack what this peer needs in order to see a session whose
 * description has changed.
 *
 * The whole session chain, oldest first - the same thing
 * register_job_info() hands a client that attaches now. It is packed
 * whole rather than as "just the new one" because that makes it
 * idempotent: the client skips every segment it already holds, so a
 * notice arriving twice, or one arriving for a peer that has already
 * caught up, costs a walk and nothing else.
 *
 * Nothing is packed when the peer's job has no session segments, which
 * is the ordinary case for a job whose host never described a session.
 */
/* How many segments a chain holds.
 *
 * Only ever asked of a chain this process publishes to, and only from
 * the progress thread, so the walk cannot race a publisher. It starts
 * from the same acquire-load a reader uses regardless - the head is
 * atomic and the nodes behind it are immutable.
 */
static size_t
chain_length(
    pmix_gds_shmem3_chain_t *chain
) {
    size_t n = 0;
    for (pmix_gds_shmem3_seg_t *seg = pmix_gds_shmem3_chain_head(chain);
         NULL != seg; seg = seg->prior) {
        n++;
    }
    return n;
}

static pmix_status_t
server_pack_update(
    struct pmix_peer_t *pr,
    pmix_buffer_t *buff
) {
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_gds_shmem3_job_t *job;
    pmix_status_t rc;

    if (NULL == peer || NULL == peer->nptr) {
        return PMIX_SUCCESS;
    }
    rc = pmix_gds_shmem3_get_job_tracker(peer->nptr->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        /* not a job we hold - nothing to say */
        return PMIX_SUCCESS;
    }
    /* BOTH chains, because both can grow after a client has attached:
     * server_add_job_data() publishes a new job segment when a host adds
     * to a registered job, and a changed session description publishes a
     * new session segment. This is the only thing that tells a client
     * already running that either happened.
     *
     * Whole and oldest-first, the way the job-info reply sends them,
     * because that is what leaves the client's chain in our order -
     * newest at the head, which is where a read starts. A client skips
     * every segment it already holds by backing path, so this is
     * idempotent and a client that missed an earlier notice catches up
     * on the next one.
     *
     * BUT SAY NOTHING WHEN THERE IS NOTHING TO SAY. PMIX_GDS_PACK_UPDATE
     * is contracted to pack nothing when a module has nothing to add,
     * and pmix_server_notify_gds_update() is built on that: a peer whose
     * buffer comes back empty is skipped entirely, and gets no message.
     *
     * That matters well beyond wasted bytes, because a sweep is not only
     * fired by a job-data update. Registering a SESSION fires one with a
     * NULL namespace - every peer of every namespace on this server -
     * and so does a session change. Packing this chain unconditionally
     * therefore sent a message to every shmem3 client on the node every
     * time any session was described, each one carrying descriptors for
     * segments the client already had. A job being launched is exactly
     * when sessions are described and clients are still arriving, which
     * is where that lands.
     *
     * A client is handed the whole job chain as it stands in the
     * job-info reply it receives during PMIx_Init, so a chain still one
     * segment long holds nothing a client can be missing. More than one
     * means server_add_job_data() published an update, which is the case
     * this notice exists for. */
    if (1 < chain_length(&job->job_chain)) {
        rc = pack_shmem3_seg_blob(job, PMIX_GDS_SHMEM3_JOB_ID, peer, buff);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    if (NULL == job->session ||
        NULL == pmix_gds_shmem3_chain_head(&job->session->segments)) {
        return PMIX_SUCCESS;
    }
    return pack_shmem3_seg_blob(
        job, PMIX_GDS_SHMEM3_SESSION_ID, peer, buff
    );
}

/* Take delivery of the above: attach whatever we do not already hold.
 *
 * The same unpacker the job-info reply and the modex-complete use, for
 * the same reason - a seg blob is a seg blob, and the duplicate check
 * inside it is what makes an update idempotent. */
static pmix_status_t
client_accept_update(
    pmix_buffer_t *buff
) {
    PMIX_GDS_SHMEM3_VVOUT_HERE();
    return client_connect_to_shmem3_from_buffi(
        buff, PMIX_GDS_SHMEM3_ATTACH_UPDATE
    );
}

/* What does this job's chain currently answer for this job-level key?
 *
 * Newest-first, stopping at the first segment carrying it - the same
 * rule a read follows, so this compares against what a client would
 * actually see rather than against any one segment.
 *
 * The caller must PMIX_RELEASE what comes back. Its session counterpart
 * hands back a borrowed pointer because a session's entries sit on a
 * plain list; job-level values live in a hash table, and fetching one
 * copies it.
 */
static pmix_kval_t *
job_current_value(
    pmix_gds_shmem3_job_t *job,
    const char *key
) {
    pmix_gds_shmem3_seg_t *seg;

    for (seg = pmix_gds_shmem3_chain_head(&job->job_chain);
         NULL != seg; seg = seg->prior) {
        pmix_gds_shmem3_shared_job_data_t *const sd = seg->smdata;
        pmix_list_t kvs;
        pmix_kval_t *kv;

        if (NULL == sd || NULL == sd->local_hashtab) {
            continue;
        }
        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        if (PMIX_SUCCESS != pmix_hash_fetch(sd->local_hashtab,
                                            PMIX_RANK_WILDCARD, key,
                                            NULL, 0, &kvs, sd->keyindex)) {
            PMIX_LIST_DESTRUCT(&kvs);
            continue;
        }
        kv = (pmix_kval_t *) pmix_list_remove_first(&kvs);
        PMIX_LIST_DESTRUCT(&kvs);
        if (NULL != kv) {
            return kv;
        }
    }
    return NULL;
}

/* Reduce an addition to what has actually changed.
 *
 * A host updating a job's data may send only what changed, or restate
 * the whole description with a few values different in it - the API
 * forbids neither, and PMIx_server_register_nspace(3) says as much. Both
 * have to cost the same, because what a restatement must not do is
 * publish a segment per unchanged key: a published segment is never
 * reclaimed (nothing is ever removed from the chain - that is what makes
 * the walk lock-free), so a host that restates its job description
 * periodically would grow this job's address-space footprint without
 * bound and lengthen every read that walks past it.
 *
 * So compare each entry against what the chain answers today and keep
 * only the ones that differ or are new. SIZE_MAX means the allocation
 * failed, which is not the same answer as zero. Returns the number kept, with
 * *out pointing at borrowed entries of the caller's array - no copy,
 * because the segment build copies what it stores.
 *
 * Exactly the rule session_changed_entries() applies to a session
 * description, for the same reason.
 */
static size_t
job_changed_entries(
    pmix_gds_shmem3_job_t *job,
    pmix_info_t info[],
    size_t ninfo,
    pmix_info_t **out
) {
    pmix_info_t *kept;
    size_t nkept = 0;

    *out = NULL;
    if (0 == ninfo || NULL == info) {
        return 0;
    }
    kept = (pmix_info_t *) calloc(ninfo, sizeof(pmix_info_t));
    if (NULL == kept) {
        /* NOT 0 - that is this function's word for "restated, nothing
         * changed", which the caller answers by publishing nothing and
         * returning success. Reporting a failed allocation that way
         * would drop the update silently while gds/hash, running in the
         * same fan-out, stored it and told the clients: the two modules
         * would then answer differently for the same namespace, which
         * is the one outcome src/mca/gds/AGENTS.md says must not be
         * reachable. */
        return SIZE_MAX;
    }
    for (size_t n = 0; n < ninfo; n++) {
        if (0 == strlen(info[n].key)) {
            continue;
        }
        /* not const: PMIX_RELEASE() nulls what it is given */
        pmix_kval_t *cur = job_current_value(job, info[n].key);
        if (NULL != cur) {
            const bool same =
                (PMIX_EQUAL == PMIx_Value_compare(cur->value,
                                    (pmix_value_t *) &info[n].value));
            PMIX_RELEASE(cur);
            if (same) {
                continue;
            }
        }
        /* Shallow: the caller owns these, and the segment build copies
         * whatever it stores into shared memory. */
        memcpy(&kept[nkept], &info[n], sizeof(pmix_info_t));
        nkept++;
    }
    if (0 == nkept) {
        free(kept);
        return 0;
    }
    *out = kept;
    return nkept;
}

/* Add job-level data to a namespace that is already registered.
 *
 * The segment already published cannot be rewritten - local clients have
 * it mapped - so the addition goes into a segment of its own at the head
 * of the job's chain, carrying only what is being added. A read walks
 * newest-first, so the new keys answer from here and everything else
 * still answers from the segments behind.
 *
 * Named after the job's own generation, so successive additions do not
 * collide, and the clients are told so a job already running sees it -
 * which is the whole point: without this, a host adding a resource
 * mid-run reaches only the namespaces registered afterwards.
 */
static pmix_status_t
server_add_job_data(
    const char *nspace,
    pmix_info_t info[],
    size_t ninfo
) {
    pmix_gds_shmem3_job_t *job;
    pmix_status_t rc;
    size_t seg_size;
    pmix_info_t *changed;
    size_t nchanged;

    if (NULL == nspace || 0 == ninfo || NULL == info) {
        return PMIX_SUCCESS;
    }
    rc = pmix_gds_shmem3_get_job_tracker(nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        /* not a job we hold */
        return PMIX_SUCCESS;
    }
    /* Nothing published yet: this job has not built its segments, so the
     * addition will be picked up from the global cache when it does -
     * hash_cache_job_info()'s gdata_added path. */
    if (NULL == pmix_gds_shmem3_chain_head(&job->job_chain)) {
        return PMIX_SUCCESS;
    }

    /* Only what actually differs from what this job already answers.
     *
     * A host is free to send just the changed values or to restate the
     * whole description, and this is what makes the two cost the same.
     * It also decides whether anything is published at all: a
     * restatement that changes nothing publishes nothing, which matters
     * because a published segment is never taken back. */
    changed = NULL;
    nchanged = job_changed_entries(job, info, ninfo, &changed);
    if (SIZE_MAX == nchanged) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return PMIX_ERR_NOMEM;
    }
    if (0 == nchanged) {
        PMIX_GDS_SHMEM3_VOUT(
            "%s: namespace=%s restated %zu job value%s, none of them "
            "changed - nothing published", __func__, nspace, ninfo,
            (1 == ninfo) ? "" : "s"
        );
        return PMIX_SUCCESS;
    }

    /* Measure the payload before sizing the segment for it.
     *
     * Every term below this is a per-entry CONSTANT, and the values are
     * copied into the segment twice - once by the value transfer and
     * again by pmix_hash_store(), which makes its own copy. So an
     * estimate built from the key count alone holds only while the
     * values are small, and a host sending one large value - a topology
     * XML, a fabric coordinate array, a regex for a big irregular
     * cluster - runs the bump allocator off the end of the segment.
     * That is not a soft failure: tma_alloc_request_will_overflow()
     * aborts, and the process is the server every client on this node
     * is talking to.
     *
     * The registration path measures its data for exactly this reason
     * (see the note on pji->packed_size in
     * prepare_shmem3_stores_for_local_job_data), and an update has to
     * do the same or it is a smaller segment holding data of the same
     * kind. */
    size_t payload = 0;
    {
        pmix_buffer_t sizer;
        PMIX_CONSTRUCT(&sizer, pmix_buffer_t);
        for (size_t n = 0; n < nchanged; n++) {
            pmix_status_t prc;
            PMIX_BFROPS_PACK(prc, pmix_globals.mypeer, &sizer,
                             &changed[n], 1, PMIX_INFO);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != prc)) {
                PMIX_ERROR_LOG(prc);
                PMIX_DESTRUCT(&sizer);
                free(changed);
                return prc;
            }
        }
        payload = sizer.bytes_used;
        PMIX_DESTRUCT(&sizer);
    }

    /* Size it for what is being added rather than for the job. */
    seg_size = sizeof(pmix_gds_shmem3_shared_job_data_t);
    seg_size += pmix_keyindex_sizeof_storage(nchanged);
    seg_size += 2 * sizeof(pmix_list_t);
    seg_size += sizeof(pmix_hash_table_t);
    seg_size += pmix_hash_table_sizeof_storage(1);
    seg_size += pmix_hash_sizeof_proc_storage();
    seg_size += nchanged * (sizeof(pmix_kval_t) + sizeof(pmix_value_t)
                            + pmix_hash_sizeof_key_entry(0)
                            + 4 * (PMIX_MAX_KEYLEN + 1));
    /* Four copies of every key string and two of every value, on the
     * same reasoning the registration estimate spells out. */
    seg_size += 5 * payload;
    seg_size *= 4;   // the same empirical fluff the other estimates carry
    seg_size *= pmix_gds_shmem3_segment_size_multiplier;

    char segname[PMIX_PATH_MAX];
    snprintf(segname, sizeof(segname), "jobdata.%u",
             (unsigned) job->job_generation);
    rc = shmem3_segment_create_and_attach(
        job, PMIX_GDS_SHMEM3_JOB_ID, segname, seg_size
    );
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        free(changed);
        return rc;
    }
    /* one hash-table element: everything here is job-level, so it all
     * hangs off the single PMIX_RANK_WILDCARD entry */
    rc = job_smdata_construct(job, 1, nchanged);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        free(changed);
        return rc;
    }

    pmix_tma_t *const tma = &job->smdata->tma;
    for (size_t n = 0; n < nchanged; n++) {
        pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
        if (PMIX_UNLIKELY(NULL == kv)) {
            free(changed);
            return PMIX_ERR_NOMEM;
        }
        kv->key = pmix_tma_strdup(tma, changed[n].key);
        kv->value = pmix_tma_calloc(tma, 1, sizeof(pmix_value_t));
        if (PMIX_UNLIKELY(NULL == kv->key || NULL == kv->value)) {
            free(changed);
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_bfrops_base_tma_value_xfer(kv->value, &changed[n].value, tma);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            free(changed);
            return rc;
        }
        rc = pmix_hash_store(job->smdata->local_hashtab, PMIX_RANK_WILDCARD,
                             kv, NULL, 0, job->smdata->keyindex);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            free(changed);
            return rc;
        }
    }
    free(changed);

    rc = pmix_gds_shmem3_publish_job_segment(job);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* the cached job-info reply describes the chain as it was */
    if (NULL != job->conni) {
        PMIX_RELEASE(job->conni);
        job->conni = NULL;
    }
    PMIX_GDS_SHMEM3_VOUT(
        "%s: namespace=%s published job data generation %u",
        __func__, nspace, (unsigned)(job->job_generation - 1)
    );
    /* and tell the clients already reading it */
    pmix_server_notify_gds_update(nspace);
    return PMIX_SUCCESS;
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
    .add_session = server_add_session,
    .del_session = server_del_session,
    .pack_update = server_pack_update,
    .accept_update = client_accept_update,
    .add_job_data = server_add_job_data,
    .del_key = del_key,
    .assemb_kvs_req = NULL,
    .accept_kvs_resp = NULL,
    .mark_modex_complete = server_mark_modex_complete,
    .recv_modex_complete = client_recv_modex_complete
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
