/*
 * Copyright (c) 2021-2022 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_SHMEM_H
#define PMIX_UTIL_SHMEM_H

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"
#include "src/class/pmix_object.h"

typedef struct pmix_shmem_t {
    /** Parent class. */
    pmix_object_t super;
    /** Size of shared-memory segment. */
    size_t size;
    /** Base address of shared memory segment. */
    void *base_address;
    /** Buffer holding path to backing store. */
    char backing_path[PMIX_PATH_MAX];
} pmix_shmem_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_shmem_t);

PMIX_EXPORT pmix_status_t
pmix_shmem_segment_create(
    pmix_shmem_t *shmem,
    size_t size,
    const char *backing_path
);

PMIX_EXPORT pmix_status_t
pmix_shmem_segment_attach(
    pmix_shmem_t *shmem,
    void *requested_base_address,
    uintptr_t *actual_base_address
);

PMIX_EXPORT pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
);

PMIX_EXPORT pmix_status_t
pmix_shmem_segment_unlink(
    pmix_shmem_t *shmem
);

#endif
