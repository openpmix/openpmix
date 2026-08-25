/*
 * Copyright (c) 2021-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
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
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"
#include "src/util/pmix_vmem.h"

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

/* Marks a segment as one of ours, so a stale or foreign backing file is
 * rejected rather than interpreted. */
#define PMIX_SHMEM_HEADER_MAGIC 0x504d5853u /* "PMXS" */

/* This header sits at the segment base and is the ONE structure both ends
 * must agree on before anything else can be trusted. Keep every field
 * fixed-width and keep it free of any type whose layout can change - no
 * pmix_object_t, no class-derived type, nothing behind PMIX_ENABLE_DEBUG.
 * If this struct can shift, it cannot be used to detect a shift. */
typedef struct pmix_shmem_header_t {
    /** Reference count. */
    pmix_atomic_int32_t ref_count;
    /** Identifies this as a PMIx segment. */
    uint32_t magic;
    /** Layout of what the creator stored - see pmix_shmem_segment_create. */
    uint32_t layout_id;
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

    /* opened relative to its directory rather than by name - see
     * pmix_os_dirpath_open_file() - so a symlink at the backing path
     * does not send the attach at some unrelated file */
    const int fd = pmix_os_dirpath_open_file(shmem->backing_path, O_RDWR, 0);
    if (fd == -1) {
        rc = PMIX_ERR_FILE_OPEN_FAILURE;
        if (0 < pmix_output_get_verbosity(pmix_gds_base_framework.framework_output)) {
            pmix_show_help("help-pmix-util.txt", "failed-file-open", true,
                           shmem->backing_path, strerror(errno));
        }
        goto out;
    }

    // When the caller requires a specific base address (gds/shmem3 stores
    // absolute pointers, so every process must map the segment at the same
    // address), use MAP_FIXED_NOREPLACE: the kernel either honors the address
    // exactly or fails with EEXIST. A bare address is only a hint that the
    // kernel may silently relocate even when the target range is free.
    //
    // Unless, that is, the caller is mapping over a range it already
    // reserved. Then the address is not a request that might be refused -
    // the range is held, by the caller, for exactly this - so MAP_FIXED
    // takes it back with no chance of EEXIST. This is the one path that is
    // immune to the address having been claimed by something else while
    // the process was busy, which is the whole point of reserving.
    int mmap_flags = MAP_SHARED;
    const int over_reservation =
        (flags & PMIX_SHMEM_MAP_OVER_RESERVATION) &&
        (uintptr_t) NULL != desired_base_address;
    const int must_map_at_raddr =
        over_reservation ||
        ((flags & PMIX_SHMEM_MUST_MAP_AT_RADDR) &&
         (uintptr_t) NULL != desired_base_address);
    if (over_reservation) {
        mmap_flags |= MAP_FIXED;
    }
    else if (must_map_at_raddr) {
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
        // would otherwise leak -- once per attempt under the gds/shmem3 retry.
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
        /* Remember how we got here: detach has to hand this range back to
         * the reservation it came out of, not simply drop it. */
        shmem->in_reservation = (0 != over_reservation);
    }
    // Always set base addresses. On error it is useful for reporting.
    shmem->hdr_address = mmap_addr;
    shmem->data_address = data_addr_from_base(mmap_addr);
    return rc;
}

static pmix_status_t
add_internal_segment_header(
    pmix_shmem_t *shmem,
    uint32_t layout_id
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
        .ref_count = 0,
        .magic = PMIX_SHMEM_HEADER_MAGIC,
        .layout_id = layout_id
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
    const char *backing_path,
    uint32_t layout_id
) {
    int rc = PMIX_SUCCESS;
    // Real size of the segment: the data region begins a full page in
    // (data_addr_from_base() rounds the header up to a page boundary), so
    // this is larger than what the caller asked to store. The arithmetic
    // is in pmix_shmem_utils_segment_footprint() because callers placing
    // segments adjacently need the same answer, and two copies of it
    // would drift.
    const size_t real_size = pmix_shmem_utils_segment_footprint(size);

    /* O_TRUNC is what makes "a freshly created segment reads as zero" a
     * guarantee rather than an assumption. Segments are unlinked when their
     * last holder lets go, so a path collides only with a file some earlier
     * server left behind when it died - but the path is built from the pid,
     * and pids are reused. Without the truncate, ftruncate() to the same or a
     * smaller size leaves that corpse's bytes in place, and only the extension
     * past the old end reads as zero. gds/shmem3's allocator relies on the
     * whole region being zero so it does not have to memset what it hands out;
     * see tma_carve() there. */
    /* O_EXCL, and opened relative to its directory rather than by name.
     *
     * This process composes the whole of backing_path out of its own
     * naming scheme, so anything already sitting there is either a file
     * left over from an earlier run - the name carries a pid, and pids
     * get reused - or something this code did not put there. O_EXCL
     * declines both, and with O_CREAT it declines a symlink in
     * particular, which a bare O_CREAT | O_TRUNC would instead follow,
     * truncating and overwriting some unrelated file.
     *
     * Two passes at most, matching write_rndz_file(): a leftover file is
     * reclaimed once and the create retried. unlink() removes the name
     * it is given and never follows it onward. */
    int fd = -1;
    for (int pass = 0; pass < 2; pass++) {
        fd = pmix_os_dirpath_open_file(backing_path,
                                       O_CREAT | O_EXCL | O_RDWR, 0600);
        if (0 <= fd || EEXIST != errno) {
            break;
        }
        if (0 != unlink(backing_path) && ENOENT != errno) {
            break;
        }
    }
    if (fd == -1) {
        rc = PMIX_ERR_FILE_OPEN_FAILURE;
        goto out;
    }
    /* A freshly created file is empty, so this only extends it - and an
     * extension reads as zero, which is the guarantee gds/shmem3's
     * allocator relies on (see tma_carve() there) so that it need not
     * memset what it hands out. */
    if (0 != ftruncate(fd, real_size)) {
        rc = PMIX_ERROR;
        goto out;
    }
    // Update segment properties.
    shmem->size = real_size;
    pmix_string_copy(shmem->backing_path, backing_path, PMIX_PATH_MAX);
    // Add internal segment header.
    rc = add_internal_segment_header(shmem, layout_id);
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
    pmix_shmem_flags_t flags,
    uint32_t expected_layout_id
) {
    pmix_status_t rc = segment_attach(
        shmem, desired_base_address, flags
    );
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    // The mapping succeeded, which says nothing about whether we can read
    // what is in it. Check the stamp BEFORE taking a reference or letting
    // the caller near the data: the whole point is to reject a segment
    // written by a process that lays these structures out differently, and
    // every read past this point assumes they match.
    const pmix_shmem_header_t *header = (pmix_shmem_header_t *)shmem->hdr_address;
    if (PMIX_SHMEM_HEADER_MAGIC != header->magic ||
        expected_layout_id != header->layout_id) {
        pmix_output_verbose(
            2, pmix_globals.debug_output,
            "shmem: refusing segment %s - magic 0x%08x (want 0x%08x), "
            "layout 0x%08x (want 0x%08x)",
            shmem->backing_path, header->magic, PMIX_SHMEM_HEADER_MAGIC,
            header->layout_id, expected_layout_id
        );
        (void)pmix_shmem_segment_detach(shmem);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    inc_ref_count(shmem->hdr_address);
    return rc;
}

pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
) {
    int rc = 0;

    if (shmem && shmem->attached) {
        if (shmem->in_reservation) {
            /* This range was carved out of a reservation the caller is
             * still holding, and it expects to be able to place something
             * here again. Unmapping would give the address back to the
             * system - and the next thing to ask for one could take it,
             * which is the failure the reservation exists to prevent. Put
             * the placeholder back in the same call that removes us. */
            rc = (PMIX_SUCCESS == pmix_vmem_restore(
                      (uintptr_t)shmem->hdr_address, shmem->size)) ? 0 : -1;
        }
        else {
            rc = munmap(shmem->hdr_address, shmem->size);
        }
        shmem->attached = false;
        shmem->in_reservation = false;
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
pmix_shmem_segment_protect_data(
    pmix_shmem_t *shmem
) {
    if (NULL == shmem || !shmem->attached) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* The header page stays writable - inc/dec_ref_count() live there,
     * and a reader still has to be able to detach. Everything from the
     * data region on is what a reader has no business touching. */
    const size_t header_offset = pmix_shmem_utils_pad_to_page(
        sizeof(pmix_shmem_header_t)
    );
    if (shmem->size <= header_offset) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 != mprotect(shmem->data_address, shmem->size - header_offset, PROT_READ)) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

pmix_status_t
pmix_shmem_segment_chmod(
    pmix_shmem_t *shmem,
    mode_t mode
) {
    pmix_status_t rc = PMIX_SUCCESS;

    /* Through a descriptor rather than by name. chmod() follows a
     * symlink at the final component, so where its neighbour lchown()
     * above deliberately acts on the link itself, this acted on whatever
     * the link named, and the two disagreed about which object they were
     * changing. A descriptor is bound to its inode at open() time, so
     * the object inspected is the object modified. */
    const int fd = pmix_os_dirpath_open_file(shmem->backing_path, O_RDONLY, 0);
    if (0 > fd) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (fchmod(fd, mode) != 0) {
        rc = PMIX_ERROR;
        PMIX_ERROR_LOG(rc);
    }
    close(fd);
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

size_t
pmix_shmem_utils_segment_footprint(
    size_t size
) {
    /* The two paddings are deliberately separate. Rounding
     * "size + sizeof(header)" as a single quantity would under-allocate
     * the data region whenever size is not a page multiple, leaving the
     * tail of the requested range past the end of the mapping. */
    return pmix_shmem_utils_pad_to_page(sizeof(pmix_shmem_header_t))
           + pmix_shmem_utils_pad_to_page(size);
}

static void
shmem_construct(
    pmix_shmem_t *s
) {
    s->attached = false;
    s->in_reservation = false;
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
