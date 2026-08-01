/*
 * Copyright (c) 2021-2023 Triad National Security, LLC. All rights reserved.
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

#include <sys/stat.h>

/**
 * Bitmap container for pmix_shmem flags.
 */
typedef uint8_t pmix_shmem_flags_t;

typedef enum {
    /**
     * Indicates that during attach the requested address
     * must match the address returned by memory mapping.
     */
    PMIX_SHMEM_MUST_MAP_AT_RADDR = 0x01
} pmix_shmem_flag_t;

typedef struct pmix_shmem_t {
    /** Parent class. */
    pmix_object_t super;
    /** Flag indicating if attached to segment. */
    volatile bool attached;
    /** Size of shared-memory segment. */
    size_t size;
    /** Address of shared memory segment header. */
    void *hdr_address;
    /** Base data address of shared memory segment. */
    void *data_address;
    /** Buffer holding path to backing store. */
    char backing_path[PMIX_PATH_MAX];
} pmix_shmem_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_shmem_t);

/**
 * Create a segment, stamping it with the caller's layout ID.
 *
 * The layout ID identifies the in-memory shape of whatever the caller
 * intends to store in the segment. A segment is shared between processes
 * that read those structures *in place*, so the two ends must agree on that
 * shape exactly; if they do not, every field the reader touches is at the
 * wrong offset. pmix_shmem_segment_attach() refuses a segment whose stamp
 * does not match what the attaching process expects, which is the only
 * thing standing between a layout disagreement and silent corruption.
 *
 * The ID is opaque here - only the caller knows what it stores - but it
 * must be derived from the layout itself (sizeof/offsetof of the types that
 * go in), not hand-maintained. A number someone has to remember to bump is
 * a number that will not get bumped.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_create(
    pmix_shmem_t *shmem,
    size_t size,
    const char *backing_path,
    uint32_t layout_id
);

/**
 * Attach to a segment, requiring its stamp to equal expected_layout_id.
 *
 * Returns PMIX_ERR_NOT_SUPPORTED if the segment was written by a process
 * whose layout differs from ours - distinct from the PMIX_ERR_NOT_AVAILABLE
 * a failed fixed-address map returns, so the caller can say which happened.
 * Callers are expected to treat either as "this datastore is unusable to
 * me" and fall back, not as fatal.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_attach(
    pmix_shmem_t *shmem,
    uintptr_t desired_base_address,
    pmix_shmem_flags_t flags,
    uint32_t expected_layout_id
);

PMIX_EXPORT pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
);

PMIX_EXPORT pmix_status_t
pmix_shmem_segment_unlink(
    pmix_shmem_t *shmem
);

/**
 * Change ownership of given shmem. Similar to chown(2).
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_chown(
    pmix_shmem_t *shmem,
    uid_t owner,
    gid_t group
);

/**
 * Change permissions of given shmem. Similar to chmod(2).
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_chmod(
    pmix_shmem_t *shmem,
    mode_t mode
);

/**
 * Returns size padded to page boundary.
 */
PMIX_EXPORT size_t
pmix_shmem_utils_pad_to_page(
    size_t size
);

#endif
