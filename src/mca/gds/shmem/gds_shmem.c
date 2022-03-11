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

#if 0
static void *
tma_shmem_malloc(
    pmix_tma_t *tma,
    size_t length
) {
    void *current = tma->data;
    tma->data = PMIX_GDS_SHMEM_MALLOC_ALIGN(tma->data, length);
    return current;
}
#endif

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
    pmix_gds_shmem_job_t *p
) {
    p->ns = NULL;
    p->shmem = PMIX_NEW(pmix_shmem_t);
    p->nptr = NULL;
    p->session = NULL;
}

static void
job_destruct(
    pmix_gds_shmem_job_t *p
) {
    free(p->ns);
    PMIX_RELEASE(p->shmem);
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
    bool specified = false;
    // The incoming info always overrides anything in the
    // environment as it is set by the application itself.
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    char **options = NULL;
    for (size_t n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_GDS_MODULE, PMIX_MAX_KEYLEN)) {
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
    char *path = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_PATH);
    char *addr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR);
    if (!path || !addr) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
    PMIX_GDS_SHMEM_VOUT("%s found path=%s", __func__, path);
    PMIX_GDS_SHMEM_VOUT("%s found addr=%s", __func__, addr);
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
    PMIX_GDS_SHMEM_UNUSED(pr);
    PMIX_GDS_SHMEM_UNUSED(reply);

    PMIX_GDS_SHMEM_VOUT_HERE();
    /* Since the data is already in the shmem when we
     * cached it, there is nothing to do here. */
    return PMIX_SUCCESS;
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buf
) {
    PMIX_GDS_SHMEM_UNUSED(nspace);
    PMIX_GDS_SHMEM_UNUSED(buf);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
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
    // Get the tracker for this job.
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(peer->nspace, &job);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    pmix_shmem_t *shmem = job->shmem;
    // Set backing file path.
    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_PATH,
        shmem->backing_path, true, env
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Set base address for attaching to shared-memory segment.
    char addrbuff[64] = {'\0'};
    size_t nw = snprintf(
        addrbuff, sizeof(addrbuff),
        "%zx", (size_t)shmem->base_address
    );
    if (nw >= sizeof(addrbuff)) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR,
        addrbuff, true, env
    );
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
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
    PMIX_GDS_SHMEM_UNUSED(nspace);
    PMIX_GDS_SHMEM_UNUSED(nlocalprocs);
    PMIX_GDS_SHMEM_UNUSED(info);
    PMIX_GDS_SHMEM_UNUSED(ninfo);
    // Calculate a rough (upper bound) estimate on the amount of storage
    // required to store the values associated with this namespace.
    // TODO(skg) Implement.
    *segment_size = 1024 << 20;
    PMIX_GDS_SHMEM_VOUT(
        "%s: determined required segment size of %zd B", __func__, *segment_size
    );
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
        if (0 == strcmp(PMIX_NSDIR, info[i].key)) {
            basedir = info[i].value.data.string;
            break;
        }
        if (0 == strcmp(PMIX_TMPDIR, info[i].key)) {
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
    int nw = snprintf(
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
segment_create(
    pmix_info_t info[],
    size_t ninfo,
    pmix_gds_shmem_job_t *job,
    size_t size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(VMEM_HOLE_BIGGEST, &base_addr, size);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: found vmhole at address=%zx", __func__, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    const char *segment_path = get_shmem_backing_path(info, ninfo);
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s", __func__, segment_path
    );
    // Create the shared-memory segment with the given address.
    rc = pmix_shmem_segment_create(job->shmem, size, segment_path);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    uintptr_t mmap_addr = 0;
    rc = pmix_shmem_segment_attach(job->shmem, (void *)base_addr, &mmap_addr);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=%zx", __func__, (size_t)mmap_addr
    );
    return rc;
}

static pmix_status_t
server_add_nspace(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo
) {
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_GDS_SHMEM_VOUT(
        "%s: adding namespace=%s with nlocalprocs=%d",
        __func__, nspace, nlocalprocs
    );
    // Get the shared-memory segment size. Mas o menos.
    size_t segment_size = 0;
    rc = get_estimated_segment_size(
        nspace, nlocalprocs,
        info, ninfo, &segment_size
    );
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Create a job tracker.
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(nspace, &job);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    // Create the shared-memory segment; update job tracker.
    return segment_create(info, ninfo, job, segment_size);
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
