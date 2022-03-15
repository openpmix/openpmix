/*
 * Copyright (c) 2021-2022 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "src/util/pmix_shmem.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_string_copy.h"

#ifdef HAVE_ERRNO_H
#include "errno.h"
#endif
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
#include <sys/mman.h>

static void
shmem_construct(
    pmix_shmem_t *s
) {
    s->size = 0;
    s->base_address = 0;
    memset(s->backing_path, 0, PMIX_PATH_MAX);
}

static void
shmem_destruct(
    pmix_shmem_t *t
) {
    (void)pmix_shmem_segment_detach(t);
    (void)pmix_shmem_segment_unlink(t);
}

PMIX_EXPORT PMIX_CLASS_INSTANCE(
    pmix_shmem_t,
    pmix_object_t,
    shmem_construct,
    shmem_destruct
);

// TODO(skg) Add network FS warning?
pmix_status_t
pmix_shmem_segment_create(
    pmix_shmem_t *shmem,
    size_t size,
    const char *backing_path
) {
    int rc = PMIX_SUCCESS;

    int fd = open(backing_path, O_CREAT | O_RDWR, 0600);
    if (fd == -1) {
        rc = PMIX_ERR_FILE_OPEN_FAILURE;
        goto out;
    }
    // Size backing file.
    if (0 != ftruncate(fd, size)) {
        rc = PMIX_ERROR;
        goto out;
    }
    shmem->size = size;
    pmix_string_copy(shmem->backing_path, backing_path, PMIX_PATH_MAX);
out:
    if (-1 != fd) {
        (void)close(fd);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

pmix_status_t
pmix_shmem_segment_attach(
    pmix_shmem_t *shmem,
    void *requested_base_address,
    uintptr_t *actual_base_address
) {
    pmix_status_t rc = PMIX_SUCCESS;

    int fd = open(shmem->backing_path, O_RDWR);
    if (fd == -1) {
        rc = PMIX_ERR_FILE_OPEN_FAILURE;
        goto out;
    }

    shmem->base_address = mmap(
        requested_base_address, shmem->size,
        PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0
    );
    if (MAP_FAILED == shmem->base_address) {
        rc = PMIX_ERR_NOMEM;
    }
    *actual_base_address = (uintptr_t)shmem->base_address;
out:
    if (-1 != fd) {
        (void)close(fd);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
) {
    if (0 != munmap(shmem->base_address, shmem->size)) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

pmix_status_t
pmix_shmem_segment_unlink(
    pmix_shmem_t *shmem
) {
    if (-1 == unlink(shmem->backing_path)) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}
