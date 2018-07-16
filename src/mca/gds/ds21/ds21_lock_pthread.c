/*
 * Copyright (c) 2018      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>
#include <pmix_common.h>

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include "src/mca/common/dstore/dstore_common.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pshmem/pshmem.h"

#include "src/util/error.h"
#include "src/util/output.h"

#include "ds21_lock.h"

typedef struct {
    char *lockfile;
    pmix_pshmem_seg_t *segment;
    pthread_mutex_t *mutex;
    uint32_t num_locks;
    uint32_t lock_idx;
} ds21_lock_pthread_ctx_t;

static int _pmix_getpagesize(void)
{
#if defined(_SC_PAGESIZE )
    return sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
    return sysconf(_SC_PAGE_SIZE);
#else
    return 65536; /* safer to overestimate than under */
#endif
}

#define _GET_MUTEX_PTR(lsize, seg_ptr) \
    ((pthread_mutex_t*)(seg_ptr + sizeof(lsize) + sizeof(int32_t)*lsize))

#define _GET_IDX_PTR(lsize, seg_ptr) \
    ((int32_t*)(seg_ptr + sizeof(lsize)))

#define _GET_SIZE_PTR(seg_ptr) \
    ((uint32_t*)(seg_ptr))

pmix_common_dstor_lock_ctx_t pmix_gds_ds21_lock_init(const char *base_path,  uint32_t local_size,
                                                     uid_t uid, bool setuid)
{
    ds21_lock_pthread_ctx_t *lock_ctx = NULL;
    pmix_status_t rc = PMIX_SUCCESS;
    pthread_mutexattr_t attr;
    size_t size;
    uint32_t i;
    int page_size = _pmix_getpagesize();
    uint32_t *local_size_ptr = NULL;

    lock_ctx = (ds21_lock_pthread_ctx_t*)malloc(sizeof(ds21_lock_pthread_ctx_t));
    if (NULL == lock_ctx) {
        PMIX_ERROR_LOG(PMIX_ERR_INIT);
        goto error;
    }
    memset(lock_ctx, 0, sizeof(ds21_lock_pthread_ctx_t));

    lock_ctx->segment = (pmix_pshmem_seg_t *)malloc(sizeof(pmix_pshmem_seg_t));
    if (NULL == lock_ctx->segment ) {
        rc = PMIX_ERR_OUT_OF_RESOURCE;
        goto error;
    }

    /* create a lock file to prevent clients from reading while server is writing
     * to the shared memory. This situation is quite often, especially in case of
     * direct modex when clients might ask for data simultaneously. */
    if(0 > asprintf(&lock_ctx->lockfile, "%s/dstore_sm.lock", base_path)) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto error;
    }
    PMIX_OUTPUT_VERBOSE((10, pmix_gds_base_framework.framework_output,
        "%s:%d:%s _lockfile_name: %s, local_size %d", __FILE__, __LINE__, __func__, lock_ctx->lockfile, local_size));

    /*
     * Lock segment format:
     * 1. local_size:              sizeof(uint32_t)
     * 2. Array of in use indexes: sizeof(int32_t)*local_size
     * 3. Double array of locks:   sizeof(pthread_mutex_t)*local_size*2
     */
    if (PMIX_PROC_IS_SERVER(pmix_globals.mypeer)) {
        lock_ctx->num_locks = local_size;
        size = ((sizeof(uint32_t) + sizeof(int32_t) * lock_ctx->num_locks +
                2 * lock_ctx->num_locks * sizeof(pthread_mutex_t)) / page_size + 1) * page_size;

        if (PMIX_SUCCESS != (rc = pmix_pshmem.segment_create(lock_ctx->segment,
                             lock_ctx->lockfile, size))) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
        memset(lock_ctx->segment->seg_base_addr, 0, size);
        if (0 != setuid) {
            if (0 > chown(lock_ctx->lockfile, (uid_t) uid, (gid_t) -1)){
                PMIX_ERROR_LOG(PMIX_ERROR);
                goto error;
            }
            /* set the mode as required */
            if (0 > chmod(lock_ctx->lockfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP )) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                goto error;
            }
        }

        if (0 != pthread_mutexattr_init(&attr)) {
            PMIX_ERROR_LOG(PMIX_ERR_INIT);
            goto error;
        }
        if (0 != pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) {
            rc = PMIX_ERR_INIT;
            pthread_mutexattr_destroy(&attr);
            PMIX_ERROR_LOG(PMIX_ERR_INIT);
            goto error;
        }

        /* store the local_size into lock segment */
        local_size_ptr = _GET_SIZE_PTR(lock_ctx->segment->seg_base_addr);
        *local_size_ptr = local_size;
        lock_ctx->mutex = _GET_MUTEX_PTR(local_size, lock_ctx->segment->seg_base_addr);

        for(i = 0; i < lock_ctx->num_locks * 2; i++) {
            if (0 != pthread_mutex_init(&lock_ctx->mutex[i], &attr)) {
                pthread_mutexattr_destroy(&attr);
                PMIX_ERROR_LOG(PMIX_ERR_INIT);
                goto error;
            }
        }
        if (0 != pthread_mutexattr_destroy(&attr)) {
            PMIX_ERROR_LOG(PMIX_ERR_INIT);
            goto error;
        }
    }
    else {
        int32_t *lock_idx_ptr;
        bool idx_found = false;

        lock_ctx->segment->seg_size = page_size;
        snprintf(lock_ctx->segment->seg_name, PMIX_PATH_MAX, "%s", lock_ctx->lockfile);
        if (PMIX_SUCCESS != (rc = pmix_pshmem.segment_attach(lock_ctx->segment, PMIX_PSHMEM_RW))) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
        /* clients gets the num procs from the segment */
        local_size = *_GET_SIZE_PTR(lock_ctx->segment->seg_base_addr);
        if (0 == local_size) {
            rc = PMIX_ERROR;
            PMIX_ERROR_LOG(rc);
            goto error;
        }
        lock_ctx->num_locks = local_size;
        /* calculate the new size considering the obtained proc size */
        size = ((sizeof(uint32_t) + sizeof(int32_t) * lock_ctx->num_locks +
                2 * lock_ctx->num_locks * sizeof(pthread_mutex_t)) / page_size + 1) * page_size;
        /* checking for need reattach the segment with the new size */
        if (size != lock_ctx->segment->seg_size) {
            pmix_pshmem.segment_detach(lock_ctx->segment);
            memset(lock_ctx->segment, 0, sizeof(pmix_pshmem_seg_t));
            /* set new seg size */
            lock_ctx->segment->seg_size = size;
            snprintf(lock_ctx->segment->seg_name, PMIX_PATH_MAX, "%s", lock_ctx->lockfile);
            if (PMIX_SUCCESS != (rc = pmix_pshmem.segment_attach(lock_ctx->segment, PMIX_PSHMEM_RW))) {
                PMIX_ERROR_LOG(rc);
                goto error;
            }
        }

        lock_idx_ptr = _GET_IDX_PTR(local_size, lock_ctx->segment->seg_base_addr);
        for (i = 0; i < local_size; i++) {
            int32_t expected = 0;
            if (pmix_atomic_compare_exchange_strong_32(&lock_idx_ptr[i], &expected, 1)) {
                lock_ctx->lock_idx = i;
                idx_found = true;
                break;
            }
        }

        if (false == idx_found) {
            rc = PMIX_ERROR;
            PMIX_ERROR_LOG(rc);
            goto error;
        }
        lock_ctx->mutex = _GET_MUTEX_PTR(local_size, lock_ctx->segment->seg_base_addr);
    }

    return (pmix_common_dstor_lock_ctx_t)lock_ctx;

error:
    if (NULL != lock_ctx) {
        if (PMIX_PROC_IS_SERVER(pmix_globals.mypeer)) {
            if (NULL != lock_ctx->lockfile) {
                free(lock_ctx->lockfile);
            }
            if (NULL != lock_ctx->segment) {
                pmix_pshmem.segment_detach(lock_ctx->segment);
                /* detach & unlink from current desc */
                if (lock_ctx->segment->seg_cpid == getpid()) {
                    pmix_pshmem.segment_unlink(lock_ctx->segment);
                }
                if (lock_ctx->mutex) {
                    for(i = 0; i < lock_ctx->num_locks * 2; i++) {
                        pthread_mutex_destroy(&lock_ctx->mutex[i]);
                    }
                }
                free(lock_ctx->segment);
            }
            free(lock_ctx);
            lock_ctx = NULL;
        } else {
            if (lock_ctx->lockfile) {
                free(lock_ctx->lockfile);
            }
            if (NULL != lock_ctx->segment) {
                pmix_pshmem.segment_detach(lock_ctx->segment);
                free(lock_ctx->segment);
                free(lock_ctx);
            }
        }
    }

    return NULL;
}

void pmix_ds21_lock_finalize(pmix_common_dstor_lock_ctx_t lock_ctx)
{
    uint32_t i;

    ds21_lock_pthread_ctx_t *pthread_lock =
            (ds21_lock_pthread_ctx_t*)lock_ctx;

    if (NULL == pthread_lock) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        return;
    }

    if (NULL == pthread_lock->segment) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return;
    }

    if(PMIX_PROC_IS_SERVER(pmix_globals.mypeer)) {
        for(i = 0; i < pthread_lock->num_locks * 2; i++) {
            if (0 != pthread_mutex_destroy(&pthread_lock->mutex[i])) {
                PMIX_ERROR_LOG(PMIX_ERROR);
                return;
            }
        }
        /* detach & unlink from current desc */
        if (pthread_lock->segment->seg_cpid == getpid()) {
            pmix_pshmem.segment_unlink(pthread_lock->segment);
        }
    }
    pmix_pshmem.segment_detach(pthread_lock->segment);
    free(pthread_lock->segment);
}

pmix_status_t pmix_ds21_lock_wr_get(pmix_common_dstor_lock_ctx_t lock_ctx)
{
    pthread_mutex_t *locks;
    uint32_t num_locks;
    uint32_t i;

    ds21_lock_pthread_ctx_t *pthread_lock = (ds21_lock_pthread_ctx_t*)lock_ctx;
    pmix_status_t rc;

    if (NULL == pthread_lock) {
        rc = PMIX_ERR_NOT_FOUND;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    locks = pthread_lock->mutex;
    num_locks = pthread_lock->num_locks;

    /* Lock the "signalling" lock first to let clients know that
     * server is going to get a write lock.
     * Clients do not hold this lock for a long time,
     * so this loop should be relatively dast.
     */
    for (i = 0; i < num_locks; i++) {
        if (0 != pthread_mutex_lock(&locks[2*i])) {
            return PMIX_ERROR;
        }
    }

    /* Now we can go and grab the main locks
     * New clients will be stopped at the previous
     * "barrier" locks.
     * We will wait here while all clients currently holding
     * locks will be done
     */
    for(i = 0; i < num_locks; i++) {
        if (0 != pthread_mutex_lock(&locks[2*i + 1])) {
            return PMIX_ERROR;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_ds21_lock_wr_rel(pmix_common_dstor_lock_ctx_t lock_ctx)
{
    pthread_mutex_t *locks;
    uint32_t num_locks;
    uint32_t i;

    ds21_lock_pthread_ctx_t *pthread_lock = (ds21_lock_pthread_ctx_t*)lock_ctx;
    pmix_status_t rc;

    if (NULL == pthread_lock) {
        rc = PMIX_ERR_NOT_FOUND;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    locks = pthread_lock->mutex;
    num_locks = pthread_lock->num_locks;

    /* Lock the second lock first to ensure that all procs will see
     * that we are trying to grab the main one */
    for(i=0; i<num_locks; i++) {
        if (0 != pthread_mutex_unlock(&locks[2*i])) {
            return PMIX_ERROR;
        }
        if (0 != pthread_mutex_unlock(&locks[2*i + 1])) {
            return PMIX_ERROR;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_ds21_lock_rd_get(pmix_common_dstor_lock_ctx_t lock_ctx)
{
    pthread_mutex_t *locks;
    uint32_t idx;

    ds21_lock_pthread_ctx_t *pthread_lock = (ds21_lock_pthread_ctx_t*)lock_ctx;
    pmix_status_t rc;

    if (NULL == pthread_lock) {
        rc = PMIX_ERR_NOT_FOUND;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    locks = pthread_lock->mutex;
    idx = pthread_lock->lock_idx;

    /* This mutex is only used to acquire the next one,
     * this is a barrier that server is using to let clients
     * know that it is going to grab the write lock
     */
    if (0 != pthread_mutex_lock(&locks[2 * idx])) {
        return PMIX_ERROR;
    }

    /* Now grab the main lock */
    if (0 != pthread_mutex_lock(&locks[2*idx + 1])) {
        return PMIX_ERROR;
    }

    /* Once done - release signalling lock */
    if (0 != pthread_mutex_unlock(&locks[2*idx])) {
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_ds21_lock_rd_rel(pmix_common_dstor_lock_ctx_t lock_ctx)
{
    ds21_lock_pthread_ctx_t *pthread_lock = (ds21_lock_pthread_ctx_t*)lock_ctx;
    pmix_status_t rc;
    pthread_mutex_t *locks;
    uint32_t idx;

    if (NULL == pthread_lock) {
        rc = PMIX_ERR_NOT_FOUND;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    locks = pthread_lock->mutex;
    idx = pthread_lock->lock_idx;

    /* Release the main lock */
    if (0 != pthread_mutex_unlock(&locks[2*idx + 1])) {
        return PMIX_SUCCESS;
    }

    return PMIX_SUCCESS;
}
