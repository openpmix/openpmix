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

// TODO(skg) Add network FS warning?
// TODO(skg) Improve error messages, etc.
pmix_status_t
pmix_shmem_segment_create(
    pmix_shmem_t *shmem,
    size_t size,
    const char *backing_path
) {
    int rc = PMIX_SUCCESS;

    int fd = open(backing_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return PMIX_ERROR;
    }
    /* Size backing file. */
    if (0 != ftruncate(fd, size)) {
        rc = PMIX_ERROR;
    }
    if (PMIX_SUCCESS == rc) {
        shmem->size = size;
        pmix_string_copy(shmem->backing_path, backing_path, PMIX_PATH_MAX);
    }
    (void)close(fd);

    return rc;
}

void *
pmix_shmem_segment_attach(
    pmix_shmem_t *shmem,
    void *base_address
) {
    int fd = open(shmem->backing_path, O_RDWR);
    if (fd < 0) {
        return NULL;
    }

    void *map_addr = mmap(base_address, shmem->size,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (MAP_FAILED == map_addr) {
        (void)close(fd);
        return NULL;
    }
    shmem->base_address = (uintptr_t)map_addr;
    return map_addr;
}

pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
) {
    if (0 != munmap((void *)shmem->base_address, shmem->size)) {
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

static void tkcon(pmix_vm_tracker_t *p)
{
    p->hole_kind = VMEM_HOLE_BIGGEST;
    p->size = 0;
    p->address = 0;
    p->fd = -1;
}
static void tkdes(pmix_vm_tracker_t *p)
{
    PMIX_HIDE_UNUSED_PARAMS(p);
    // release the hole
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_vm_tracker_t,
                                pmix_object_t,
                                tkcon, tkdes);
