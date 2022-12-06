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
#include "gds_shmem_tma.h"
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
#include "src/util/pmix_show_help.h"

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

//
// Notes for developers:
// We cannot use PMIX_CONSTRUCT for data that are stored in shared memory
// because their address is on the stack of the process in which they are
// constructed.
//

// Some items for future consideration:
// * Address FT case at some point. We need to have a broader conversion about
//   how we go about doing this. Ralph has some ideas.
// * Is it worth adding memory arena boundary checks to our TMA allocators?

/**
 * Key names used to find shared-memory segment info.
 */
#define PMIX_GDS_SHMEM_SEG_BLOB_KEY "PMIX_GDS_SHMEM_SEG_BLOB"
#define PMIX_GDS_SHMEM_SEG_PATH_KEY "PMIX_GDS_SHMEM_SEG_PATH"
#define PMIX_GDS_SHMEM_SEG_SIZE_KEY "PMIX_GDS_SHMEM_SEG_SIZE"
#define PMIX_GDS_SHMEM_SEG_ADDR_KEY "PMIX_GDS_SHMEM_SEG_ADDR"

/**
 * Stores packed job statistics.
 */
typedef struct {
    size_t packed_size;
    size_t hash_table_size;
} pmix_gds_shmem_local_job_stats_t;

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
 * 8-byte address alignment.
 */
static inline void *
addr_align_8(
    void *base,
    size_t size
) {
#if 0 // Helpful debug
    PMIX_GDS_SHMEM_VVOUT("------------------------ADDRINN=%p,%zd", base, size);
#endif
    void *result = (void *)(((uintptr_t)base + size + 7) & ~(uintptr_t)0x07);
#if 0 // Helpful debug
    // Make sure that it's 8-byte aligned.
    assert ((uintptr_t)result % 8 == 0);
    PMIX_GDS_SHMEM_VVOUT("------------------------ADDROUT=%p,%zd", result, size);
#endif
    return result;
}

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

static inline void *
tma_realloc(
    pmix_tma_t *tma,
    void *ptr,
    size_t size
) {
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr, size);
    // We don't support realloc.
    assert(false);
    return NULL;
}

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
    job->shmem = PMIX_NEW(pmix_shmem_t);
    job->modex_shmem = PMIX_NEW(pmix_shmem_t);
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
    // Releases memory for the structures located in shared-memory.
    if (job->shmem) {
        PMIX_RELEASE(job->shmem);
    }
    job->shmem = NULL;
    // Releases memory for the structures located in shared-memory.
    if (job->modex_shmem) {
        PMIX_RELEASE(job->modex_shmem);
    }
    job->modex_shmem = NULL;
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

static void
session_construct(
    pmix_gds_shmem_session_t *s
) {
    pmix_tma_t *tma = pmix_obj_get_tma(&s->super.super);

    s->session = UINT32_MAX;
    s->sessioninfo = PMIX_NEW(pmix_list_t, tma);
    s->nodeinfo = PMIX_NEW(pmix_list_t, tma);
}

static void
session_destruct(
    pmix_gds_shmem_session_t *s
) {
    if (s->sessioninfo) {
        PMIX_LIST_DESTRUCT(s->sessioninfo);
    }
    if (s->nodeinfo) {
        PMIX_LIST_DESTRUCT(s->nodeinfo);
    }
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_session_t,
    pmix_list_item_t,
    session_construct,
    session_destruct
);

static pmix_status_t
job_smdata_construct(
    pmix_gds_shmem_job_t *job,
    size_t htsize
) {
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its smdata located at the base of the segment.
    void *const baseaddr = job->shmem->base_address;
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
    pmix_tma_t *const tma = pmix_gds_shmem_get_job_tma(job);
    // Now that we know the TMA, initialize smdata structures using it. Note
    // that both smdata->session and smdata->smmodex are both NULL.
    job->smdata->jobinfo = PMIX_NEW(pmix_list_t, tma);
    job->smdata->nodeinfo = PMIX_NEW(pmix_list_t, tma);
    job->smdata->apps = PMIX_NEW(pmix_list_t, tma);
    // Will always have local data, so set it up.
    job->smdata->local_hashtab = PMIX_NEW(pmix_hash_table2_t, tma);
    pmix_hash_table2_init(job->smdata->local_hashtab, htsize);

    pmix_gds_shmem_vout_smdata(job);

    return PMIX_SUCCESS;
}

static pmix_status_t
modex_smdata_construct(
    pmix_gds_shmem_job_t *job,
    size_t htsize
) {
    // Setup the shared information structure. It will be at the base address of
    // the shared-memory segment. The memory is already allocated, so let the
    // job know about its modex_smdata located at the base of the segment.
    void *const baseaddr = job->modex_shmem->base_address;
    job->smdata->smmodex = baseaddr;
    memset(job->smdata->smmodex, 0, sizeof(*job->smdata->smmodex));
    // Save the starting address for TMA memory allocations.
    job->smdata->smmodex->current_addr = baseaddr;
    // Setup the TMA.
    tma_init(&job->smdata->smmodex->tma);
    job->smdata->smmodex->tma.data_ptr = &job->smdata->smmodex->current_addr;
    // Now we need to update the TMA's pointer to account for our using up some
    // space for its header.
    *(job->smdata->smmodex->tma.data_ptr) = addr_align_8(baseaddr, sizeof(*job->smdata->smmodex));
    // We can now safely get the TMA.
    pmix_tma_t *const tma = &job->smdata->smmodex->tma;
    // Now that we know the TMA, initialize smdata structures using it.
    job->smdata->smmodex->hashtab = PMIX_NEW(pmix_hash_table2_t, tma);
    pmix_hash_table2_init(job->smdata->smmodex->hashtab, htsize);

    return PMIX_SUCCESS;
}

static pmix_status_t
module_init(
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.jobs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_mca_gds_shmem_component.sessions, pmix_list_t);
    return PMIX_SUCCESS;
}

static void
module_finalize(void)
{
    PMIX_GDS_SHMEM_VOUT_HERE();
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem_component.jobs);
    // TODO(skg) Fix crash.
    //PMIX_LIST_DESTRUCT(&pmix_mca_gds_shmem_component.sessions);
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
prepare_shmem_store_for_local_job_data(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_local_job_stats_t *stats
) {
    pmix_status_t rc = PMIX_SUCCESS;
    const size_t kvsize = (sizeof(pmix_kval_t) + sizeof(pmix_value_t));
    // Initial hash table size.
    const size_t htsize = stats->hash_table_size;
    // Calculate a rough estimate on the amount of storage required to store the
    // values associated with the pmix_gds_shmem_shared_job_data_t. Err on the
    // side of overestimation.
    // Start with the size of the segment header.
    size_t seg_size = sizeof(pmix_gds_shmem_shared_job_data_t);
    // We need to store a hash table in the shared-memory segment, so calculate
    // a rough estimate on the memory required for its storage.
    seg_size += sizeof(pmix_hash_table2_t);
    seg_size += htsize * pmix_hash_table2_t_sizeof_hash_element();
    // Add a little extra to compensate for the value storage requirements. Here
    // we add an additional storage space for each entry.
    seg_size += htsize * kvsize;
    // Finally add the data size contribution, plus a little extra.
    seg_size += stats->packed_size * 1.25;
    // Add some extra fluff in case we weren't precise enough.
    // TODO(skg) Consider making fluff an MCA parameter.
    seg_size *= 2;
    // Pad to fill an entire page.
    seg_size += pmix_gds_shmem_pad_amount_to_page(seg_size);
    // Create and attach to the shared-memory segment associated with this job.
    // This will be the backing store for metadata associated with static,
    // read-only data shared between the server and its clients.
    rc = pmix_gds_shmem_segment_create_and_attach(
        job, job->shmem, "jobdata", seg_size
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = job_smdata_construct(job, htsize);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }

    return rc;
}

static pmix_status_t
pack_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_peer_t *peer,
    pmix_buffer_t *buffer
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s:%s for peer (ID=%d) namespace=%s", __func__,
        PMIX_NAME_PRINT(&pmix_globals.myid),
        peer->info->peerid, job->nspace_id
    );
    // Pack the backing file path.
    pmix_kval_t kv;
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = strdup(PMIX_GDS_SHMEM_SEG_PATH_KEY);
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
    kv.key = strdup(PMIX_GDS_SHMEM_SEG_SIZE_KEY);
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
    kv.key = strdup(PMIX_GDS_SHMEM_SEG_ADDR_KEY);
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
 * Sets shared-memory connection information from a pmix_kval_t by
 * unpacking the blob and saving the values for the caller.
 */
static pmix_status_t
unpack_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_kval_t *kvbo
) {
    pmix_status_t rc = PMIX_SUCCESS;

    // Make sure this is the expected type.
    if (PMIX_BYTE_OBJECT != kvbo->value->type) {
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

        const char *val = kv.value->data.string;
        if (PMIX_CHECK_KEY(&kv, PMIX_GDS_SHMEM_SEG_PATH_KEY)) {
            // Set job segment path.
            int nw = snprintf(
                job->shmem->backing_path, PMIX_PATH_MAX, "%s", val
            );
            if (nw >= PMIX_PATH_MAX) {
                rc = PMIX_ERROR;
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, PMIX_GDS_SHMEM_SEG_SIZE_KEY)) {
            // Set job shared-memory segment size.
            rc = strtost(val, 16, &job->shmem->size);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, PMIX_GDS_SHMEM_SEG_ADDR_KEY)) {
            size_t base_addr = 0;
            // Convert string base address to something we can use.
            rc = strtost(val, 16, &base_addr);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
            // Set job segment base address.
            job->shmem->base_address = (void *)base_addr;
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

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        rc = PMIX_ERR_UNPACK_FAILURE;
        PMIX_ERROR_LOG(rc);
    }
    else {
        PMIX_GDS_SHMEM_VVOUT(
            "%s: " PMIX_GDS_SHMEM_SEG_PATH_KEY "=%s",
            __func__, job->shmem->backing_path
        );
        PMIX_GDS_SHMEM_VVOUT(
            "%s: " PMIX_GDS_SHMEM_SEG_SIZE_KEY "=%zd B",
            __func__, job->shmem->size
        );
        PMIX_GDS_SHMEM_VVOUT(
            "%s: " PMIX_GDS_SHMEM_SEG_ADDR_KEY "=0x%zx",
            __func__, (size_t)job->shmem->base_address
        );
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/**
 * Fetches a complete copy of the job-level information.
 */
static pmix_status_t
fetch_local_job_data(
    pmix_namespace_t *ns,
    pmix_cb_t *job_cb
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_proc_t wildcard;
    PMIX_LOAD_PROCID(&wildcard, ns->nspace, PMIX_RANK_WILDCARD);

    job_cb->key = NULL;
    job_cb->proc = &wildcard;
    job_cb->copy = true;
    job_cb->scope = PMIX_LOCAL;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, job_cb);

    return rc;
}

/**
 * Internally the hash table can do some interesting sizing calculations, so we
 * just construct a temporary one with the number of expected elements, then
 * query it for its actual capacity.
 */
static size_t
get_actual_hashtab_capacity(
    size_t num_elements
) {
    pmix_hash_table2_t tmp;
    PMIX_CONSTRUCT(&tmp, pmix_hash_table2_t);
    pmix_hash_table2_init(&tmp, num_elements);
    // Grab the actual capacity.
    const size_t result = tmp.ht_capacity;
    PMIX_DESTRUCT(&tmp);

    return result;
}

static pmix_status_t
get_local_job_data_stats(
    pmix_peer_t *peer,
    pmix_cb_t *job_cb,
    pmix_gds_shmem_local_job_stats_t *stats
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t nhtentries = 0;

    memset(stats, 0, sizeof(*stats));

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
        }
        // Just a key/value pair, so they will likely go into the hash table.
        else {
            nhtentries += 1;
        }

        PMIX_BFROPS_PACK(rc, peer, &data, kvi, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto out;
        }
    }
    stats->packed_size = data.bytes_used;
    stats->hash_table_size = get_actual_hashtab_capacity(nhtentries);
out:
    PMIX_DESTRUCT(&data);
    return rc;
}

static pmix_status_t
publish_shmem_connection_info(
    pmix_gds_shmem_job_t *job,
    pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_namespace_t *ns = peer->nptr;

    // Pack the payload for delivery. Note that the message we are going to send
    // is simply the shared memory connection information that is shared among
    // clients on a single node.

    // Start with the namespace name.
    PMIX_BFROPS_PACK(rc, peer, reply, &ns->nspace, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_buffer_t buff;
    do {
        PMIX_CONSTRUCT(&buff, pmix_buffer_t);

        rc = pack_shmem_connection_info(job, peer, &buff);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }

        pmix_value_t blob = {
            .type = PMIX_BYTE_OBJECT
        };
        pmix_kval_t kv = {
            .key = PMIX_GDS_SHMEM_SEG_BLOB_KEY,
            .value = &blob
        };

        PMIX_UNLOAD_BUFFER(&buff, blob.data.bo.bytes, blob.data.bo.size);
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_VALUE_DESTRUCT(&blob);
    } while (0);
    PMIX_DESTRUCT(&buff);

    // Make sure packing the shared-memory connection pack was successful.
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // If we have more than one local client for this nspace,
    // save this packed object so we don't do this again.
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) ||
        ns->nlocalprocs > 1) {
        PMIX_RETAIN(reply);
        ns->jobbkt = reply;
    }
    return rc;
}

static pmix_status_t
server_register_new_job_info(
    pmix_peer_t *peer,
    pmix_buffer_t *reply
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_namespace_t *const ns = peer->nptr;

    // Setup a new job tracker for this peer's nspace.
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(ns->nspace, true, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Ask for a complete copy of the job-level information.
    pmix_cb_t job_cb;
    PMIX_CONSTRUCT(&job_cb, pmix_cb_t);

    rc = fetch_local_job_data(ns, &job_cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Pack the data so we can see how large it is. This will help inform how
    // large to make the shared-memory segment associated with these data.
    pmix_gds_shmem_local_job_stats_t stats;
    rc = get_local_job_data_stats(peer, &job_cb, &stats);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Get the shared-memory segment ready for job data.
    rc = prepare_shmem_store_for_local_job_data(job, &stats);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // Store fetched data into a shared-memory segment.
    rc = pmix_gds_shmem_store_local_job_data_in_shmem(job, &job_cb.kvs);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto out;
    }
    // You guessed it, publish shared-memory connection info.
    rc = publish_shmem_connection_info(job, peer, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
out:
    PMIX_DESTRUCT(&job_cb);
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
    // Else we need to actually store and register the job info.
    PMIX_GDS_SHMEM_VOUT(
        "[%s:%d] no cached payload. registering a new one.",
        pmix_globals.myid.nspace, pmix_globals.myid.rank
    );
    return server_register_new_job_info(peer, reply);
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
    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(nspace, true, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_kval_t kval;
    while (true) {
        PMIX_CONSTRUCT(&kval, pmix_kval_t);

        int32_t cnt = 1;
        PMIX_BFROPS_UNPACK(
            rc, pmix_client_globals.myserver,
            buff, &kval, &cnt, PMIX_KVAL
        );
        if (PMIX_SUCCESS != rc) {
            break;
        }

        if (PMIX_CHECK_KEY(&kval, PMIX_GDS_SHMEM_SEG_BLOB_KEY)) {
            rc = unpack_shmem_connection_info(job, &kval);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        else if (PMIX_CHECK_KEY(&kval, PMIX_SESSION_INFO_ARRAY) ||
                 PMIX_CHECK_KEY(&kval, PMIX_NODE_INFO_ARRAY) ||
                 PMIX_CHECK_KEY(&kval, PMIX_APP_INFO_ARRAY)) {
            PMIX_GDS_SHMEM_VVOUT("%s:skipping type=%s", __func__, kval.key);
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
        pmix_show_help(
            "help-gds-shmem.txt",
            "shmem-segment-attach:address-mismatch",
            true,
            (size_t)job->shmem->base_address,
            (size_t)mmap_addr

        );
        rc = PMIX_ERROR;
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
#if 0
    // Protect memory: clients can only read from here.
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
shmem_store_modex(
    pmix_gds_base_ctx_t ctx,
    pmix_proc_t *proc,
    pmix_gds_modex_key_fmt_t key_fmt,
    char **kmap,
    pmix_buffer_t *pbkt
) {
    PMIX_HIDE_UNUSED_PARAMS(ctx);
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "[%s:%d] %s for nspace %s",
        pmix_globals.myid.nspace,
        pmix_globals.myid.rank,
        __func__, proc->nspace
    );

    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(proc->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_hash_table2_t *const ht = job->smdata->smmodex->hashtab;
    pmix_tma_t *const tma = &job->smdata->smmodex->tma;

    // This is data returned via the PMIx_Fence call when data collection was
    // requested, so it only contains REMOTE/GLOBAL data. The byte object
    // contains the rank followed by pmix_kval_ts. The list of callbacks
    // contains all local participants.
    pmix_kval_t *kv = PMIX_NEW(pmix_kval_t, tma);
    // Unpack the values until we hit the end of the buffer.
    rc = pmix_gds_base_modex_unpack_kval(key_fmt, pbkt, kmap, kv);
    while (PMIX_SUCCESS == rc) {
        if (PMIX_RANK_UNDEF == proc->rank) {
            // If the rank is undefined, then we store it on the
            // remote table of rank=0 as we know that rank must
            // always exist.
            if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_shmem_store_qualified(ht, 0, kv->value);
            }
            else {
                rc = pmix_hash2_store(ht, 0, kv, NULL, 0);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        else {
            // Store this in the hash table.
            if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_shmem_store_qualified(ht, proc->rank, kv->value);
            }
            else {
                rc = pmix_hash2_store(ht, proc->rank, kv, NULL, 0);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
        PMIX_DESTRUCT(kv);
        // Continue along.
        kv = PMIX_NEW(pmix_kval_t, tma);
        rc = pmix_gds_base_modex_unpack_kval(key_fmt, pbkt, kmap, kv);
    }
    PMIX_DESTRUCT(kv);

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }
    else {
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
store_modex(
    struct pmix_namespace_t *ns,
    pmix_buffer_t *buff,
    void *cbdata
) {
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_gds_shmem_job_t *job;
    rc = pmix_gds_shmem_get_job_tracker(
        ((pmix_namespace_t *)(ns))->nspace, false, &job
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    // Estimated size required to store the unpacked modex data.
    // TODO(skg) Improve estimate.
    size_t seg_size = buff->bytes_used * 1024;
    seg_size += pmix_gds_shmem_pad_amount_to_page(seg_size);
    // Create and attach to the shared-memory segment that will back these data.
    rc = pmix_gds_shmem_segment_create_and_attach(
        job, job->modex_shmem, "modexdata", seg_size
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    // TODO(skg) We need to calculate this somehow.
    const size_t htsize = 256 * job->nspace->nprocs;
    rc = modex_smdata_construct(job, htsize);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    return pmix_gds_base_store_modex(
        ns, buff, NULL, shmem_store_modex, cbdata
    );
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

static pmix_status_t
del_nspace(
    const char *nspace
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

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

static void mark_modex_complete(struct pmix_peer_t *peer,
                                pmix_list_t *nslist,
                                pmix_buffer_t *buff)
{
    PMIX_HIDE_UNUSED_PARAMS(peer, nslist, buff);
    return;
}

static void recv_modex_complete(pmix_buffer_t *buff)
{
    PMIX_HIDE_UNUSED_PARAMS(buff);
    return;
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
    .store_modex = store_modex,
    .fetch = pmix_gds_shmem_fetch,
    .setup_fork = server_setup_fork,
    .add_nspace = server_add_nspace,
    .del_nspace = del_nspace,
    .assemb_kvs_req = NULL,
    .accept_kvs_resp = NULL,
    .mark_modex_complete = mark_modex_complete,
    .recv_modex_complete = recv_modex_complete
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
