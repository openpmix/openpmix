/*
 * Copyright (c) 2021-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022-2026 Nanook Consulting.  All rights reserved.
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
    PMIX_SHMEM_MUST_MAP_AT_RADDR = 0x01,
    /**
     * Indicates that the caller already owns the requested address range
     * - it holds a reservation covering it (see pmix_vmem_reserve()) -
     * and the mapping is to be placed over that reservation.
     *
     * This turns the attach from "map here if the address happens to be
     * free" into "map here", which is the whole reason a caller goes to
     * the trouble of reserving: the address cannot have been taken by
     * anything else in the meantime, because the reservation is what was
     * holding it.
     *
     * Only pass this for a range the caller genuinely reserved. It maps
     * MAP_FIXED, which silently replaces whatever is at the address,
     * so a wrong address here unmaps something that mattered.
     *
     * Implies PMIX_SHMEM_MUST_MAP_AT_RADDR.
     */
    PMIX_SHMEM_MAP_OVER_RESERVATION = 0x02
} pmix_shmem_flag_t;

typedef struct pmix_shmem_t {
    /** Parent class. */
    pmix_object_t super;
    /** Flag indicating if attached to segment. */
    volatile bool attached;
    /** True if this mapping was placed over a caller-held reservation,
     *  and so must be given back to it rather than simply unmapped. */
    bool in_reservation;
    /** True while this handle holds a reference on the segment's shared
     *  reference count. Only pmix_shmem_segment_attach() takes one - the
     *  internal attach that stamps a freshly created segment does not -
     *  so detach has to know which kind of attachment it is undoing. */
    bool holds_ref;
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
 *
 * A created segment is NOT attached and holds NO reference: the file
 * exists, its header is stamped, and nothing is mapped. The creator has
 * to call pmix_shmem_segment_attach() like anybody else if it means to
 * keep the segment alive, because the reference count is the only thing
 * that does - the first holder to let go of a segment nobody else has
 * taken removes the backing file.
 *
 * On failure nothing is left behind: any file this call created is
 * removed again, and the handle's backing path is cleared.
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
 *
 * Two fields of the handle are INPUTS here, and a process attaching to a
 * segment somebody else created has to fill them in itself: backing_path,
 * and size. "size" is the mapped footprint - the value the CREATOR's
 * handle carried after pmix_shmem_segment_create() returned, which is
 * pmix_shmem_utils_segment_footprint() of what it asked to store, not
 * what it asked to store. Pass the smaller number and the mapping ends a
 * page short of the data region, which nothing here can detect: the tail
 * of the segment is simply not there, and the reader faults on it later.
 * gds/shmem3 puts the creator's shmem->size on the wire for exactly this
 * reason.
 *
 * On success the handle holds a reference on the segment; give it back
 * with pmix_shmem_segment_detach() or by releasing the handle. On
 * failure nothing is mapped, no reference is held, and the address
 * fields read NULL.
 *
 * A handle maps one segment at a time: this refuses a handle that is
 * already attached with PMIX_ERR_BAD_PARAM rather than replacing what
 * it holds. Detach first.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_attach(
    pmix_shmem_t *shmem,
    uintptr_t desired_base_address,
    pmix_shmem_flags_t flags,
    uint32_t expected_layout_id
);

/**
 * Drop this process's mapping of the segment.
 *
 * This releases the reference pmix_shmem_segment_attach() took, so the
 * two are a matched pair and a handle that is detached explicitly leaves
 * the shared count where a handle that is simply released would. Dropping
 * the LAST reference unlinks the backing file - that is what makes a
 * segment disappear once every holder has let go of it, and it means a
 * handle whose detach was the last one cannot attach again: the segment
 * is gone, which is the point.
 *
 * Detaching a handle that never completed a public attach - including
 * one whose attach failed - releases nothing, so it is safe to call on
 * an error path without knowing how far the attach got.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_detach(
    pmix_shmem_t *shmem
);

/**
 * Remove the segment's backing file by name.
 *
 * Only the name goes: a process that has the segment mapped keeps a
 * valid mapping afterwards, which is what lets one generation of a
 * segment be handed off while readers are still on the previous one.
 *
 * Ordinarily nothing calls this - dropping the last reference through
 * pmix_shmem_segment_detach() does it - and the exception is a caller
 * unwinding a segment it created but never managed to attach, which no
 * reference count knows about.
 *
 * The handle's backing path is cleared either way, so the handle no
 * longer names a file after this returns.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_unlink(
    pmix_shmem_t *shmem
);

/**
 * Change ownership of the segment's backing file.
 *
 * Acts on the object at the name without following a symlink there -
 * lchown(2) rather than chown(2) - so that this and its neighbour below
 * cannot end up changing two different objects.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_chown(
    pmix_shmem_t *shmem,
    uid_t owner,
    gid_t group
);

/**
 * Change permissions of the segment's backing file.
 *
 * Through a descriptor rather than by name, for the reason above:
 * chmod(2) follows a symlink at the final component and lchown(2) does
 * not, so the pair applied to a path would disagree about which object
 * they were changing.
 *
 * Note what the mode has to allow. A peer maps the segment MAP_SHARED
 * from a descriptor it opened O_RDWR - it writes the reference count,
 * even when it never writes the data - so a mode that denies write to
 * the processes meant to read the segment does not make it read-only to
 * them, it makes it unopenable.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_chmod(
    pmix_shmem_t *shmem,
    mode_t mode
);

/**
 * Drop write access to the segment's data region.
 *
 * The internal header is deliberately left writable: it carries the
 * reference count that attach and detach maintain, so a reader that
 * could not write it could not let go of the segment either.
 *
 * For a reader this turns "nothing here writes to the segment" from an
 * assumption into something the MMU enforces - a stray write becomes a
 * SIGSEGV at the instruction that did it, rather than corruption
 * another process trips over later. It is one-way: there is no
 * unprotect, because a segment a reader has protected is one it has no
 * business writing again.
 *
 * The geometry lives here rather than at the call site on purpose. The
 * data region does not start at the mapping's base - it begins a
 * page-aligned header in - so its length is not the segment size, and
 * getting that wrong either leaves the tail writable or walks off the
 * end of the mapping.
 */
PMIX_EXPORT pmix_status_t
pmix_shmem_segment_protect_data(
    pmix_shmem_t *shmem
);

/**
 * Returns size padded to page boundary.
 */
PMIX_EXPORT size_t
pmix_shmem_utils_pad_to_page(
    size_t size
);

/**
 * How much address space a segment created for "size" bytes of data
 * actually occupies once mapped.
 *
 * This is NOT "size", and the difference has teeth: a segment carries an
 * internal header ahead of the data region, and the data region begins on
 * the page after it, so the mapping runs a page longer than the caller
 * asked for. A caller that has to know where the segment ENDS - one
 * placing segments next to each other in a range it is managing - must
 * ask, or it will overlap the next one by that page and find out later,
 * somewhere else, as corruption.
 *
 * Kept here, beside pmix_shmem_segment_create()'s own arithmetic, so the
 * two cannot drift.
 */
PMIX_EXPORT size_t
pmix_shmem_utils_segment_footprint(
    size_t size
);

#endif
