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

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include "src/mca/gds/base/base.h"
#include "gds_shmem_utils.h"
#include "gds_shmem.h"

#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_shmem.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
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

#define PMIX_GDS_SHMEM_VOUT_HERE()                                             \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME                             \
                        ":%s at line %d", __func__, __LINE__);                 \
} while (0)

#define PMIX_GDS_SHMEM_VOUT(...)                                               \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)

#define PMIX_GDS_SHMEM_MALLOC_ALIGN(p, s)                                      \
    (uint8_t *)(p) + (((s) + 8UL - 1) & ~(8UL - 1))

/*
 * TODO(skg) Think about reallocs. Pointer arrays may change size.
 * TODO(skg) We don't know how big this will thing will be.
 * * Can do a two-step process; in memory; get size; copy into sm.
 * * On the client side need to write locally. Server as well.
 * * Try to avoid thread locks, too expensive at scale.
 * * Work on getting data stored. Cache will go into hash. Use register job
 *   info.
 */

/*
 * TODO(skg) Ideally these would (somehow) be integrated directory into the code
 * in ./include/pmix_common.h. For now we will just match it the best we can.
 */
#define PMIX_GDS_SHMEM_INFO_CREATE(m, n, tma)                                  \
do {                                                                           \
    (m) = (pmix_info_t *)(tma)->malloc((tma), (n) * sizeof(pmix_info_t));      \
    pmix_info_t *i = (pmix_info_t *)(m);                                       \
    i[(n) - 1].flags = PMIX_INFO_ARRAY_END;                                    \
} while (0)

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
static inline size_t
pad_amount_to_page(
    size_t size
) {
    const size_t page_size = get_page_size();
    return (((~size) + page_size + 1) & (page_size - 1));
}

static inline void *
tma_malloc(
    struct pmix_gds_shmem_tma *tma,
    size_t size
) {
    void *current = tma->data;
    memset(current, 0, size);
    tma->data = PMIX_GDS_SHMEM_MALLOC_ALIGN(tma->data, size);
    return current;
}

static void
tma_construct(
    pmix_gds_shmem_tma_t *s
) {
    s->malloc = tma_malloc;
    s->data = NULL;
}

static void
tma_destruct(
    pmix_gds_shmem_tma_t *s
) {
    s->malloc = NULL;
    s->data = NULL;
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_tma_t,
    pmix_object_t,
    tma_construct,
    tma_destruct
);

static void
session_construct(
    pmix_gds_shmem_session_t *s
) {
    s->session = UINT32_MAX;
    PMIX_CONSTRUCT(&s->sessioninfo, pmix_list_t);
    PMIX_CONSTRUCT(&s->nodeinfo, pmix_list_t);
}

static void
session_destruct(
    pmix_gds_shmem_session_t *s
) {
    PMIX_LIST_DESTRUCT(&s->sessioninfo);
    PMIX_LIST_DESTRUCT(&s->nodeinfo);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_session_t,
    pmix_list_item_t,
    session_construct,
    session_destruct
);

static void
job_construct(
    pmix_gds_shmem_job_t *j
) {
    j->ns = NULL;
    j->shmem = PMIX_NEW(pmix_shmem_t);
    j->tma = PMIX_NEW(pmix_gds_shmem_tma_t);
    j->nptr = NULL;
    j->session = NULL;
}

static void
job_destruct(
    pmix_gds_shmem_job_t *p
) {
    free(p->ns);
    PMIX_RELEASE(p->shmem);
    PMIX_RELEASE(p->tma);
    PMIX_RELEASE(p->nptr);
    PMIX_RELEASE(p->session);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_job_t,
    pmix_list_item_t,
    job_construct,
    job_destruct
);

static pmix_status_t
init(
    pmix_info_t info[],
    size_t ninfo
) {

    PMIX_GDS_SHMEM_UNUSED(info);
    PMIX_GDS_SHMEM_UNUSED(ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static void
finalize(void)
{
    PMIX_GDS_SHMEM_VOUT_HERE();
}

/**
 * Note: only clients enter here.
 */
static pmix_status_t
client_assign_module(
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
    // Else we are a candidate for use.
    // Look for the shared-memory segment connection environment variables: If
    // found, then we can connect to the segment. Otherwise, we have to reject.
    const char *path = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_PATH);
    const char *size = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE);
    const char *addr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR);
    if (!path || !size || !addr) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
    PMIX_GDS_SHMEM_VOUT("%s found path=%s", __func__, path);
    PMIX_GDS_SHMEM_VOUT("%s found size=0x%s B", __func__, size);
    PMIX_GDS_SHMEM_VOUT("%s found addr=0x%s", __func__, addr);
    return PMIX_SUCCESS;
}

/**
 * @note: only servers enter here.
 * At this moment we aren't called since hash has a higher priority.
 */
static pmix_status_t
server_cache_job_info(
    struct pmix_namespace_t *ns,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_GDS_SHMEM_UNUSED(ns);
    PMIX_GDS_SHMEM_UNUSED(info);
    PMIX_GDS_SHMEM_UNUSED(ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
server_register_job_info(
    struct pmix_peer_t *pr,
    pmix_buffer_t *reply
) {
    // TODO(skg) GDS_GET pass own peer to get info; cache in sm.
    // TODO(skg) Look at pmix_hash_fetch(ht, PMIX_RANK_WILDCARD, NULL, &val);
    PMIX_GDS_SHMEM_UNUSED(pr);
    PMIX_GDS_SHMEM_UNUSED(reply);

    PMIX_GDS_SHMEM_VOUT_HERE();
    /* Since the data is already in the shmem when we
     * cached it, there is nothing to do here. */
    return PMIX_SUCCESS;
}

/**
 * String to size_t.
 */
static pmix_status_t
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
 * Sets shared-memory connection information from environment variables set by
 * server at setup_fork().
 */
static pmix_status_t
set_shmem_connection_info_from_env(
    pmix_shmem_t *shmem
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t base_addr = 0, nw = 0;
    // Get the shared-memory segment connection environment variables.
    const char *pathstr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_PATH);
    const char *sizestr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE);
    const char *addrstr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR);
    // These should be set for us, but check for sanity.
    if (!pathstr || !sizestr || !addrstr) {
        rc = PMIX_ERROR;
        goto out;
    }
    // Set job segment path.
    nw = snprintf(
        shmem->backing_path, PMIX_PATH_MAX, "%s", pathstr
    );
    if (nw >= PMIX_PATH_MAX) {
        rc = PMIX_ERROR;
        goto out;
    }
    // Convert string base address to something we can use.
    rc = strtost(addrstr, 16, &base_addr);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set job segment base address.
    shmem->base_address = (void *)base_addr;
    // Set job shared-memory segment size.
    rc = strtost(sizestr, 16, &shmem->size);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }

    PMIX_GDS_SHMEM_VOUT(
        "%s: using segment backing file path %s (%zd B) at 0x%zx",
        __func__, shmem->backing_path, shmem->size, (size_t)base_addr
    );
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buf
) {
    PMIX_GDS_SHMEM_UNUSED(buf);

    pmix_status_t rc = PMIX_SUCCESS;
    uintptr_t mmap_addr = 0;
    // Get the job tracker for the given namespace.
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(nspace, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set shared-memory segment information for this job.
    rc = set_shmem_connection_info_from_env(job->shmem);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Attach to the given shared-memory segment.
    rc = pmix_shmem_segment_attach(
        job->shmem,
        // The base address was populated above.
        job->shmem->base_address,
        &mmap_addr
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Make sure that we mapped to the requested address.
    if (mmap_addr != (uintptr_t)job->shmem->base_address) {
        rc = PMIX_ERROR;
    }
    // Done. Before this point the server should have populated the
    // shared-memory segment with the relevant data.
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
store(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    pmix_kval_t *kv
) {
    PMIX_GDS_SHMEM_UNUSED(proc);
    PMIX_GDS_SHMEM_UNUSED(scope);
    PMIX_GDS_SHMEM_UNUSED(kv);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
store_modex(
    struct pmix_namespace_t *ns,
    pmix_buffer_t *buff,
    void *cbdata
) {
    PMIX_GDS_SHMEM_UNUSED(ns);
    PMIX_GDS_SHMEM_UNUSED(buff);
    PMIX_GDS_SHMEM_UNUSED(cbdata);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
fetch(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM_UNUSED(proc);
    PMIX_GDS_SHMEM_UNUSED(scope);
    PMIX_GDS_SHMEM_UNUSED(copy);
    PMIX_GDS_SHMEM_UNUSED(key);
    PMIX_GDS_SHMEM_UNUSED(qualifiers);
    PMIX_GDS_SHMEM_UNUSED(nqual);
    PMIX_GDS_SHMEM_UNUSED(kvs);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
server_setup_fork(
    const pmix_proc_t *peer,
    char ***env
) {
    // Server stashes segment connection
    // info in the environment of its children.
    pmix_status_t rc = PMIX_SUCCESS;
    char addrbuff[64] = {'\0'}, sizebuff[64] = {'\0'};
    size_t nw = 0;
    PMIX_GDS_SHMEM_VOUT(
        "%s: peer (rank=%d) namespace=%s",
        __func__, peer->rank, peer->nspace
    );
    // Get the tracker for this job.
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(peer->nspace, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set backing file path.
    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_PATH,
        job->shmem->backing_path, true, env
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set attach size to shared-memory segment.
    nw = snprintf(
        sizebuff, sizeof(sizebuff),
        "%zx", job->shmem->size
    );
    if (nw >= sizeof(sizebuff)) {
        rc = PMIX_ERROR;
        goto out;
    }
    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE,
        sizebuff, true, env
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set base address for attaching to shared-memory segment.
    nw = snprintf(
        addrbuff, sizeof(addrbuff),
        "%zx", (size_t)job->shmem->base_address
    );
    if (nw >= sizeof(addrbuff)) {
        rc = PMIX_ERROR;
        goto out;
    }

    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR,
        addrbuff, true, env
    );
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
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

static inline pmix_status_t
get_value_size(
    const pmix_value_t *value,
    size_t *result
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t total = 0;
    pmix_value_t *tmp_value = NULL;

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
        case PMIX_DATA_ARRAY: {
            size_t n = 0, sizeofn = 0;
            total += sizeof(pmix_data_array_t);
            n = value->data.darray->size;
            // TODO(skg) This probably needs lots of work: need to potentially
            // set more value info to recursively find sizes of complex types.
            PMIX_VALUE_CREATE(tmp_value, 1);
            tmp_value->type = value->data.darray->type;
            rc = get_value_size(tmp_value, &sizeofn);
            PMIX_VALUE_RELEASE(tmp_value);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            total += (n * sizeofn);
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
            // TODO(skg) Is this fine? Looks like it's just a string.
            total += strlen(value->data.string) + 1;
            break;
        case PMIX_SIZE:
            total += sizeof(size_t);
            break;
        case PMIX_STRING:
            // TODO(skg) Is there a special case for this?
            total += strlen(value->data.string) + 1;
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

static pmix_status_t
get_estimated_segment_size(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo,
    size_t *segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t total = 0, size = 0;
    // Calculate a rough (upper bound) estimate on the amount of storage
    // required to store the values associated with this namespace.
    total += (ninfo * sizeof(pmix_info_t));
    for (size_t i = 0; i < ninfo; ++i) {
        const pmix_value_t *value = &info[i].value;
        rc = get_value_size(value, &size);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        total += size;
    }
    // Add a little room in case we aren't exact, pad to page boundary.
    // XXX(skg) We can probably make this tighter.
    total = total * 1.50;
    total += pad_amount_to_page(total);
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment size=%zd B for namespace=%s nlocalprocs=%d",
        __func__, total, nspace, nlocalprocs
    );
    *segment_size = total;
    return PMIX_SUCCESS;
}

static const char *
get_shmem_backing_path(
    pmix_info_t info[],
    size_t ninfo
) {
    static char path[PMIX_PATH_MAX];
    static const char *basedir = NULL;

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
    if (!basedir) {
        if (NULL == (basedir = getenv("TMPDIR"))) {
            basedir = "/tmp";
        }
    }
    // Now that we have the base dir, append file name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/gds-%s.%d",
        basedir, PMIX_GDS_SHMEM_NAME, getpid()
    );
    if (nw >= PMIX_PATH_MAX) {
        // Best we can do. Sorry.
        return basedir;
    }
    return path;
}

static pmix_status_t
segment_create_and_attach(
    pmix_info_t info[],
    size_t ninfo,
    pmix_gds_shmem_job_t *job,
    size_t size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    const char *segment_path = NULL;
    uintptr_t mmap_addr = 0;
    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &base_addr, size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: found vmhole at address=0x%zx",
        __func__, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    segment_path = get_shmem_backing_path(info, ninfo);
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s (%zd B)",
        __func__, segment_path, size
    );
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(
        job->shmem, size, segment_path
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
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)mmap_addr
    );
    // Let our allocator know where its base address is.
    job->tma->data = (void *)mmap_addr;
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(job->shmem);
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
server_add_nspace(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_GDS_SHMEM_VOUT(
        "%s: adding namespace=%s with nlocalprocs=%d",
        __func__, nspace, nlocalprocs
    );
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_job_t *job = NULL;
    pmix_info_t *infosm = NULL;
    // Get the shared-memory segment size. Mas o menos.
    size_t segment_size = 0;
    rc = get_estimated_segment_size(
        nspace, nlocalprocs,
        info, ninfo, &segment_size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Create a job tracker.
    rc = pmix_gds_shmem_get_job_tracker(nspace, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Create and attached to the shared-memory segment, update job tracker.
    rc = segment_create_and_attach(
        info, ninfo, job, segment_size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // TODO(skg) Look at PMIX_BFROPS_PACK code.
    // TODO(skg) Look at PMIX_DATA_ARRAY_CONSTRUCT
    // Now add the information to the shared-memory segment.
    // First, create the infos.
    PMIX_GDS_SHMEM_INFO_CREATE(infosm, ninfo, job->tma);
    for (size_t i = 0; i < ninfo; ++i) {
        const char *key = info[i].key;
        const pmix_value_t value = info[i].value;
        const pmix_data_type_t val_type = value.type;

        PMIX_GDS_SHMEM_VOUT(
            "%s: Storing k=%s, v=%s", __func__,
            key, PMIx_Data_type_string(val_type)
        );
    }
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
del_nspace(
    const char *nspace
) {
    PMIX_GDS_SHMEM_UNUSED(nspace);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
assemb_kvs_req(
    const pmix_proc_t *proc,
    pmix_list_t *kvs,
    pmix_buffer_t *bo,
    void *cbdata
) {
    PMIX_GDS_SHMEM_UNUSED(proc);
    PMIX_GDS_SHMEM_UNUSED(kvs);
    PMIX_GDS_SHMEM_UNUSED(bo);
    PMIX_GDS_SHMEM_UNUSED(cbdata);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
accept_kvs_resp(
    pmix_buffer_t *buf
) {
    PMIX_GDS_SHMEM_UNUSED(buf);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

pmix_gds_base_module_t pmix_shmem_module = {
    .name = PMIX_GDS_SHMEM_NAME,
    .is_tsafe = false,
    .init = init,
    .finalize = finalize,
    .assign_module = client_assign_module,
    .cache_job_info = server_cache_job_info,
    .register_job_info = server_register_job_info,
    .store_job_info = store_job_info,
    .store = store,
    .store_modex = store_modex,
    .fetch = fetch,
    .setup_fork = server_setup_fork,
    .add_nspace = server_add_nspace,
    .del_nspace = del_nspace,
    .assemb_kvs_req = assemb_kvs_req,
    .accept_kvs_resp = accept_kvs_resp
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
