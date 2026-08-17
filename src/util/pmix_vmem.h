/*
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_VMEM_H
#define PMIX_UTIL_VMEM_H

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

typedef enum {
    VMEM_HOLE_NONE = -1,
    /** Use hole at the very beginning. */
    VMEM_HOLE_BEGIN = 0,
    /** Use hole right after the heap. */
    VMEM_HOLE_AFTER_HEAP = 1,
    /* Use hole right before stack. */
    VMEM_HOLE_BEFORE_STACK = 2,
    /* Use the biggest hole. */
    VMEM_HOLE_BIGGEST = 3,
    /* Use the biggest hole between heap and stack. */
    VMEM_HOLE_IN_LIBS = 4,
    /* Use given address, if available. */
    VMEM_HOLE_CUSTOM = 5,
    /* Use the biggest hole, but placed a quarter of the way into it
     * rather than at its midpoint. See the note on use_hole_offset() in
     * pmix_vmem.c: everything else here - and in hwloc and Open MPI -
     * aims for the midpoint, which is precisely why an address picked
     * that way in one process tends to be taken already in another that
     * has to map at the same place. */
    VMEM_HOLE_BIGGEST_OFFSET = 6
} pmix_vmem_hole_kind_t;

PMIX_EXPORT pmix_status_t
pmix_vmem_find_hole(
    pmix_vmem_hole_kind_t hkind,
    size_t *addrp,
    size_t size
);

/**
 * As pmix_vmem_find_hole(), but spread over the hole by "scatter".
 *
 * Picking the same rule in two processes is what lets them agree on an
 * address without talking. The cost is that it also makes two UNRELATED
 * things agree, and then collide: concurrent jobs on one node each have
 * a server computing an address from its own map, and those maps look
 * alike, so they arrive at the same answer and only the first can have
 * it. A process that has to hold ranges from both - a client that was
 * spawned by, or connected to, another namespace - is the one that then
 * cannot.
 *
 * "scatter" moves the result deterministically within a window around
 * the placement "hkind" would otherwise choose. Callers should derive it
 * from something that differs between the things that must not collide
 * and is identical everywhere within one of them - for a PMIx job, its
 * namespace. Two processes given the same scatter still agree; two jobs
 * do not have to.
 *
 * Only VMEM_HOLE_BIGGEST_OFFSET spreads. The other kinds name one
 * position each, so there is nothing to spread over and scatter is
 * ignored.
 */
PMIX_EXPORT pmix_status_t
pmix_vmem_find_hole_scattered(
    pmix_vmem_hole_kind_t hkind,
    uint64_t scatter,
    size_t *addrp,
    size_t size
);

/**
 * Reserve a range of this process's address space.
 *
 * Finds a hole of the requested size using "hkind" and claims it with an
 * inaccessible (PROT_NONE) anonymous mapping. Nothing is committed - the
 * kernel charges no memory for it - so the only resource consumed is the
 * virtual address range itself and one VMA.
 *
 * The point is to hold an address range *before* something else takes
 * it, so that a later mapping can be placed there with certainty. A
 * caller that must map the same address in several processes (gds/shmem3
 * stores absolute pointers, so every reader has to) reserves the range in
 * each of them at a moment when the address space is still quiet, and
 * thereafter maps into its own reservation, which cannot fail for want of
 * the address.
 *
 * "scatter" spreads the choice as described on
 * pmix_vmem_find_hole_scattered(), so that two callers whose address
 * spaces look alike do not both settle on the same range. It is also
 * perturbed between retries, because a deterministic rule that has just
 * lost the address it named will otherwise name it again.
 *
 * Returns PMIX_ERR_NOT_AVAILABLE if no hole of that size could be found
 * or claimed.
 */
PMIX_EXPORT pmix_status_t
pmix_vmem_reserve(
    pmix_vmem_hole_kind_t hkind,
    uint64_t scatter,
    size_t size,
    uintptr_t *base
);

/**
 * Reserve one specific range, or fail.
 *
 * The peer half of pmix_vmem_reserve(): a process that was told which
 * range to hold claims exactly that one. Returns PMIX_ERR_NOT_AVAILABLE
 * if any part of it is already occupied - never a different range, since
 * a different range would be useless to the caller.
 */
PMIX_EXPORT pmix_status_t
pmix_vmem_reserve_at(
    uintptr_t base,
    size_t size
);

/**
 * Put a sub-range of a reservation back the way pmix_vmem_reserve() left
 * it, after something was mapped over it.
 *
 * A mapping placed inside a reservation replaces that part of it, so
 * simply unmapping when done would punch a hole in the reservation and
 * hand the address back to the next thing that asks for one. This
 * restores the PROT_NONE placeholder in a single step, leaving no window
 * in which the range is unclaimed.
 */
PMIX_EXPORT pmix_status_t
pmix_vmem_restore(
    uintptr_t base,
    size_t size
);

/**
 * Release a reservation obtained from pmix_vmem_reserve()/_reserve_at().
 *
 * Every mapping the caller placed inside it goes away too, so this must
 * not run until the caller is done with all of them.
 */
PMIX_EXPORT void
pmix_vmem_release(
    uintptr_t base,
    size_t size
);

#endif
