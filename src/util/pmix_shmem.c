/*
 * Copyright (c) 2021-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/util/pmix_shmem.h"

#include "pmix_common.h"
#include "pmix_output.h"
#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <sys/mman.h>
#include <errno.h>

// MAP_FIXED_NOREPLACE (Linux 4.17+) may be unavailable on older systems; fall
// back to a plain address hint, which the post-mmap address check validates.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

typedef struct pmix_shmem_header_t {
    /** Reference count. */
    pmix_atomic_int32_t ref_count;
} pmix_shmem_header_t;

static void *
data_addr_from_base(
    void *base_addr
) {
    const size_t header_offset = pmix_shmem_utils_pad_to_page(
        sizeof(pmix_shmem_header_t)
    );
    return (void *)((uintptr_t)base_addr + header_offset);
}

static inline void
inc_ref_count(
    pmix_shmem_header_t *header
) {
    (void)pmix_atomic_fetch_add_32(&header->ref_count, 1);
}

static inline bool
dec_ref_count(
    pmix_shmem_header_t *header
) {
    return pmix_atomic_sub_fetch_32(&header->ref_count, 1) == 0;
}

static pmix_status_t
segment_attach(
    pmix_shmem_t *shmem,
    uintptr_t desired_base_address,
    pmix_shmem_flags_t flags
) {
    pmix_status_t rc = PMIX_SUCCESS;
    void *mmap_addr = MAP_FAILED;

    const int fd = open(shmem->backing_path, O_RDWR);
    if (fd == -1) {
        rc = PMIX_ERR_FILE_OPEN_FAILURE;
        if (0 < pmix_output_get_verbosity(pmix_gds_base_framework.framework_output)) {
            pmix_show_help("help-pmix-util.txt", "failed-file-open", true,
                           shmem->backing_path, strerror(errno));
        }
        goto out;
    }

    // When the caller requires a specific base address (gds/shmem2 stores
    // absolute pointers, so every process must map the segment at the same
    // address), use MAP_FIXED_NOREPLACE: the kernel either honors the address
    // exactly or fails with EEXIST. A bare address is only a hint that the
    // kernel may silently relocate even when the target range is free.
    int mmap_flags = MAP_SHARED;
    const int must_map_at_raddr =
        (flags & PMIX_SHMEM_MUST_MAP_AT_RADDR) &&
        (uintptr_t) NULL != desired_base_address;
    if (must_map_at_raddr) {
        mmap_flags |= MAP_FIXED_NOREPLACE;
    }
    mmap_addr = mmap(
        (void *)desired_base_address, shmem->size,
        PROT_READ | PROT_WRITE, mmap_flags, fd, 0
    );
    if (MAP_FAILED == mmap_addr) {
        // EEXIST: the requested address is occupied. Report it as "not
        // available" so the caller can retry at a different address.
        rc = (must_map_at_raddr && EEXIST == errno)
                 ? PMIX_ERR_NOT_AVAILABLE : PMIX_ERR_NOMEM;
        goto out;
    }
    // Fallback for kernels that ignore MAP_FIXED_NOREPLACE and treat the
    // address as a hint: reject a mapping that did not land where required.
    if (must_map_at_raddr && desired_base_address != (uintptr_t)mmap_addr) {
        // The mapping succeeded, just at the wrong address; unmap it here.
        // The error path below calls pmix_shmem_segment_detach(), which only
        // unmaps once shmem->attached is set (success only), so the mapping
        // would otherwise leak -- once per attempt under the gds/shmem2 retry.
        (void)munmap(mmap_addr, shmem->size);
        mmap_addr = MAP_FAILED;
        rc = PMIX_ERR_NOT_AVAILABLE;
        goto out;
    }
out:
    if (-1 != fd) {
        (void)close(fd);
    }
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(shmem);
    }
    else {
        shmem->attached = true;
    }
    // Always set base addresses. On error it is useful for reporting.
    shmem->hdr_address = mmap_addr;
    shmem->data_address = data_addr_from_base(mmap_addr);
    return rc;
}

static pmix_status_t
add_internal_segment_header(
    pmix_shmem_t *shmem
) {
    int rc = PMIX_SUCCESS;
    // The base address here is inconsequential because this is a temporary,
    // internal attachment site that should not be exposed to the caller.
    rc = segment_attach(shmem, (uintptr_t)NULL, 0);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    // Add the header.
    pmix_shmem_header_t shmem_header = {
        .ref_count = 0
    };
    memmove(shmem->hdr_address, &shmem_header, sizeof(shmem_header));
    // Done with internal mapping, so detach.
    return pmix_shmem_segment_detach(shmem);
}

// TODO(skg) Add network FS warning?
pmix_status_t
pmix_shmem_segment_create(
    pmix_shmem_t *shmem,
    size_t size,
    const char *backing_path
) {
    int rc = PMIX_SUCCESS;
    // Real size of the segment: add header plus extra to pad to page boundary.
    const size_t real_size = pmix_shmem_utils_pad_to_page(
        size + sizeof(pmix_shmem_header_t)
    );

    const int fd = open(backing_path, O_CREAT | O_RDWR, 0600);
    if (fd == -1) {
        rc = PMIX_ERR_FILE_OPEN_FAILURE;
        goto out;
    }
    // Size backing file.
    if (0 != ftruncate(fd, real_size)) {
        rc = PMIX_ERROR;
        goto out;
    }
    // Update segment properties.
    shmem->size = real_size;
    pmix_string_copy(shmem->backing_path, backing_path, PMIX_PATH_MAX);
    // Add internal segment header.
    rc = add_internal_segment_header(shmem);
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
    uintptr_t desired_base_address,
    pmix_shmem_flags_t flags
) {
    const pmix_status_t rc = segment_attach(
        shmem, desired_base_address, flags
    );

    if (PMIX_SUCCESS == rc) {
        inc_ref_count(shmem->hdr_address);
    }
    return rc;
}

pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
) {
    int rc = 0;

    if (shmem && shmem->attached) {
        rc = munmap(shmem->hdr_address, shmem->size);
        shmem->attached = false;
        shmem->hdr_address = NULL;
        shmem->data_address = NULL;
    }

    return (0 == rc) ? PMIX_SUCCESS : PMIX_ERROR;
}

pmix_status_t
pmix_shmem_segment_chown(
    pmix_shmem_t *shmem,
    uid_t owner,
    gid_t group
) {
    pmix_status_t rc = PMIX_SUCCESS;

    if (lchown(shmem->backing_path, owner, group) != 0) {  // DO NOT FOLLOW LINKS
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

pmix_status_t
pmix_shmem_segment_chmod(
    pmix_shmem_t *shmem,
    mode_t mode
) {
    pmix_status_t rc = PMIX_SUCCESS;

    if (chmod(shmem->backing_path, mode) != 0) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

pmix_status_t
pmix_shmem_segment_unlink(
    pmix_shmem_t *shmem
) {
    const int rc = unlink(shmem->backing_path);
    memset(shmem->backing_path, 0, PMIX_PATH_MAX);

    return (0 == rc) ? PMIX_SUCCESS : PMIX_ERROR;
}

/**
 * Returns page size.
 */
static size_t
get_page_size(void)
{
    const long i = sysconf(_SC_PAGE_SIZE);
    if (0 > i) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return 0;
    }
    return i;
}

size_t
pmix_shmem_utils_pad_to_page(
    size_t size
) {
    const size_t page_size = get_page_size();
    const size_t pad = ((~size) + page_size + 1) & (page_size - 1);
    return size + pad;
}

static void
shmem_construct(
    pmix_shmem_t *s
) {
    s->attached = false;
    s->size = 0;
    s->hdr_address = NULL;
    s->data_address = NULL;
    memset(s->backing_path, 0, PMIX_PATH_MAX);
}

static void
shmem_destruct(
    pmix_shmem_t *s
) {
    // We don't have access to a reference count, so bail.
    if (!s->attached) {
        return;
    }
    // If our reference count has reached zero, then unlink the backing file.
    if (dec_ref_count(s->hdr_address)) {
        (void)pmix_shmem_segment_unlink(s);
    }
    (void)pmix_shmem_segment_detach(s);
}

PMIX_EXPORT PMIX_CLASS_INSTANCE(
    pmix_shmem_t,
    pmix_object_t,
    shmem_construct,
    shmem_destruct
);
