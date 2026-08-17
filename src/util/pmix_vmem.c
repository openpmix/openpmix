/*
 * Copyright (c) 2021-2022 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2022-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "src/util/pmix_vmem.h"

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

#define PMIX_VMEM_ALIGN2MB  (2  * 1024 * 1024UL)
#define PMIX_VMEM_ALIGN64MB (64 * 1024 * 1024UL)

/* MAP_FIXED_NOREPLACE (Linux 4.17+) may be unavailable; fall back to a
 * plain address hint and verify what we got, exactly as pmix_shmem.c
 * does. MAP_NORESERVE is likewise not universal - where it is missing
 * the reservation simply is not marked as uncommitted, which costs
 * nothing on a PROT_NONE mapping. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

typedef enum {
    VMEM_MAP_FILE = 0,
    VMEM_MAP_ANONYMOUS = 1,
    VMEM_MAP_HEAP = 2,
    VMEM_MAP_STACK = 3,
    /** vsyscall/vdso/vvar shouldn't occur since we stop after stack. */
    VMEM_MAP_OTHER = 4
} pmix_vmem_map_kind_t;

static int
parse_map_line(
    const char *line,
    unsigned long *beginp,
    unsigned long *endp,
    pmix_vmem_map_kind_t *kindp
) {
    const char *tmp = line, *next;
    unsigned long value;

    /* "beginaddr-endaddr " */
    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PMIX_ERROR;
    }

    *beginp = (unsigned long) value;

    if (*next != '-') {
        return PMIX_ERROR;
    }

    tmp = next + 1;

    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PMIX_ERROR;
    }
    *endp = (unsigned long) value;
    tmp = next;

    if (*next != ' ') {
        return PMIX_ERROR;
    }
    tmp = next + 1;

    /* look for ending absolute path */
    next = strchr(tmp, '/');
    if (next) {
        *kindp = VMEM_MAP_FILE;
    } else {
        /* look for ending special tag [foo] */
        next = strchr(tmp, '[');
        if (next) {
            if (!strncmp(next, "[heap]", 6)) {
                *kindp = VMEM_MAP_HEAP;
            } else if (!strncmp(next, "[stack]", 7)) {
                *kindp = VMEM_MAP_STACK;
            } else {
                char *end;
                if ((end = strchr(next, '\n')) != NULL) {
                    *end = '\0';
                }
                *kindp = VMEM_MAP_OTHER;
            }
        } else {
            *kindp = VMEM_MAP_ANONYMOUS;
        }
    }

    return PMIX_SUCCESS;
}

static pmix_status_t
use_hole(
    unsigned long holebegin,
    unsigned long holesize,
    unsigned long *addrp,
    unsigned long size
) {
    unsigned long aligned;
    unsigned long middle = holebegin + holesize / 2;

    if (holesize < size) {
        return PMIX_ERROR;
    }

    /* try to align the middle of the hole on 64MB for POWER's 64k-page PMD */
    aligned = (middle + PMIX_VMEM_ALIGN64MB) & ~(PMIX_VMEM_ALIGN64MB - 1);
    if (aligned + size <= holebegin + holesize) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* try to align the middle of the hole on 2MB for x86 PMD */
    aligned = (middle + PMIX_VMEM_ALIGN2MB) & ~(PMIX_VMEM_ALIGN2MB - 1);
    if (aligned + size <= holebegin + holesize) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* just use the end of the hole */
    *addrp = holebegin + holesize - size;
    return PMIX_SUCCESS;
}

/**
 * Place a quarter of the way into the hole rather than halfway.
 *
 * use_hole() above aims for the middle, and so does every other
 * implementation of this idea in the HPC stack - the routine it is
 * derived from is shared, in spirit and largely in code, with hwloc and
 * Open MPI. That convergence is the problem. Two processes with quite
 * different address spaces still compute a similar "middle of the biggest
 * hole", because on a 64-bit process the biggest hole is the enormous span
 * below the executable and its midpoint moves only with the load base. So
 * a server picks an address by that rule, and the client it hands the
 * address to has often already had something else placed there by the same
 * rule.
 *
 * The fix is to be somewhere else, deterministically. Which "somewhere
 * else" is not arbitrary, and the obvious answer is wrong: aiming at the
 * TOP of the hole was tried and is worse than the midpoint, because the
 * top of that hole is the underside of the executable's load address.
 * That is the one part of the range where processes differ most - ASLR
 * moves a PIE base over tens of gigabytes, and the heap grows there - so
 * a range placed just below one process's binary routinely lands on
 * another's. It was measurable immediately: on aarch64, where PIE bases
 * cluster around 0xaaaa..., clients on a second node could not reserve
 * the range their own server had picked.
 *
 * A quarter of the way in has the property both of those lack. It is far
 * from the midpoint every other implementation converges on, far from the
 * populated region near the executable, and still deep inside a span that
 * is empty in every process - on x86-64 it lands around 0x1555_xxxx_xxxx,
 * on aarch64 around 0x2aaa_xxxx_xxxx. The hole is measured in tens of
 * terabytes, so a quarter of it is nowhere near either end.
 */
static pmix_status_t
use_hole_offset(
    unsigned long holebegin,
    unsigned long holesize,
    unsigned long *addrp,
    unsigned long size
) {
    unsigned long quarter, aligned;

    if (holesize < size) {
        return PMIX_ERROR;
    }

    quarter = holebegin + (holesize / 4);
    /* Same 64MB alignment as the midpoint placement, for the same reason
     * (POWER's 64k-page PMD). */
    aligned = (quarter + PMIX_VMEM_ALIGN64MB) & ~(PMIX_VMEM_ALIGN64MB - 1);
    if (aligned >= holebegin && size <= (holebegin + holesize) - aligned) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* The request is a large fraction of the hole, so there is no room to
     * be choosy. Take the ordinary placement rather than nothing. */
    return use_hole(holebegin, holesize, addrp, size);
}

pmix_status_t
pmix_vmem_find_hole(
    pmix_vmem_hole_kind_t hkind,
    size_t *addrp,
    size_t size
) {
    unsigned long biggestbegin = 0;
    unsigned long biggestsize = 0;
    unsigned long prevend = 0;
    pmix_vmem_map_kind_t prevmkind = VMEM_MAP_OTHER;
    int in_libs = 0;
    FILE *file;
    char line[96];

    file = fopen("/proc/self/maps", "r");
    if (!file) {
        return PMIX_ERROR;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long begin = 0, end = 0;
        pmix_vmem_map_kind_t mkind = VMEM_MAP_OTHER;

        if (!parse_map_line(line, &begin, &end, &mkind)) {
            switch (hkind) {
                case VMEM_HOLE_BEGIN:
                    fclose(file);
                    return use_hole(0, begin, addrp, size);

                case VMEM_HOLE_AFTER_HEAP:
                    if (prevmkind == VMEM_MAP_HEAP && mkind != VMEM_MAP_HEAP) {
                        /* only use HEAP when there's no other HEAP after it
                         * (there can be several of them consecutively).
                         */
                        fclose(file);
                        return use_hole(prevend, begin - prevend, addrp, size);
                    }
                    break;

                case VMEM_HOLE_BEFORE_STACK:
                    if (mkind == VMEM_MAP_STACK) {
                        fclose(file);
                        return use_hole(prevend, begin - prevend, addrp, size);
                    }
                    break;

                case VMEM_HOLE_IN_LIBS:
                    /* see if we are between heap and stack */
                    if (prevmkind == VMEM_MAP_HEAP) {
                        in_libs = 1;
                    }
                    if (mkind == VMEM_MAP_STACK) {
                        in_libs = 0;
                    }
                    if (!in_libs) {
                        /* we're not in libs, ignore this entry */
                        break;
                    }
                    /* we're in libs, consider this entry for searching the biggest hole below */
                    /* fallthrough */

                case VMEM_HOLE_BIGGEST:
                case VMEM_HOLE_BIGGEST_OFFSET:
                    if (begin - prevend > biggestsize) {
                        biggestbegin = prevend;
                        biggestsize = begin - prevend;
                    }
                    break;

                default:
                    assert(0);
            }
        }

        while (!strchr(line, '\n')) {
            if (!fgets(line, sizeof(line), file)) {
                goto done;
            }
        }

        if (mkind == VMEM_MAP_STACK) {
            /* Don't go beyond the stack. Other VMAs are special (vsyscall, vvar, vdso, etc),
             * There's no spare room there. And vsyscall is even above the userspace limit.
             */
            break;
        }

        prevend = end;
        prevmkind = mkind;
    }

done:
    fclose(file);
    if (hkind == VMEM_HOLE_BIGGEST_OFFSET) {
        return use_hole_offset(biggestbegin, biggestsize, addrp, size);
    }
    if (hkind == VMEM_HOLE_IN_LIBS || hkind == VMEM_HOLE_BIGGEST) {
        return use_hole(biggestbegin, biggestsize, addrp, size);
    }

    return PMIX_ERROR;
}

/**
 * Claim "size" bytes at "base", or report that we could not.
 *
 * The mapping is PROT_NONE: it exists to occupy the range, and touching
 * it is a bug we would rather see as a fault than as silent corruption.
 */
static pmix_status_t
reserve_at(
    uintptr_t base,
    size_t size
) {
    void *got;

    got = mmap(
        (void *)base, size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
        -1, 0
    );
    if (MAP_FAILED == got) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    /* Where MAP_FIXED_NOREPLACE is unavailable the address was only a
     * hint and the kernel may have honored it elsewhere. A reservation
     * somewhere else is no reservation at all to our caller. */
    if ((uintptr_t)got != base) {
        (void)munmap(got, size);
        return PMIX_ERR_NOT_AVAILABLE;
    }
    return PMIX_SUCCESS;
}

pmix_status_t
pmix_vmem_reserve(
    pmix_vmem_hole_kind_t hkind,
    size_t size,
    uintptr_t *base
) {
    /* The scan of /proc/self/maps and the mmap that acts on it are not
     * one operation, so the hole can be taken in between - by another
     * thread, the dynamic loader, or the allocator growing an arena.
     * Rescanning after a loss finds the now-occupied range and moves on,
     * so a small number of attempts is enough; a persistent failure
     * means we are out of usable holes, not that we are unlucky. */
    enum { MAX_RESERVE_ATTEMPTS = 8 };

    *base = 0;
    for (int attempt = 1; attempt <= MAX_RESERVE_ATTEMPTS; ++attempt) {
        size_t addr = 0;
        pmix_status_t rc = pmix_vmem_find_hole(hkind, &addr, size);
        if (PMIX_SUCCESS != rc) {
            return PMIX_ERR_NOT_AVAILABLE;
        }
        if (PMIX_SUCCESS == reserve_at((uintptr_t)addr, size)) {
            *base = (uintptr_t)addr;
            return PMIX_SUCCESS;
        }
    }
    return PMIX_ERR_NOT_AVAILABLE;
}

pmix_status_t
pmix_vmem_reserve_at(
    uintptr_t base,
    size_t size
) {
    if (0 == base || 0 == size) {
        return PMIX_ERR_BAD_PARAM;
    }
    return reserve_at(base, size);
}

pmix_status_t
pmix_vmem_restore(
    uintptr_t base,
    size_t size
) {
    void *got;

    if (0 == base || 0 == size) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* MAP_FIXED, not MAP_FIXED_NOREPLACE: the range is ours and is
     * expected to be occupied - by whatever we mapped over the
     * reservation - and replacing it in one call is what leaves no
     * window in which the address is unclaimed. */
    got = mmap(
        (void *)base, size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
        -1, 0
    );
    if (MAP_FAILED == got) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

void
pmix_vmem_release(
    uintptr_t base,
    size_t size
) {
    if (0 == base || 0 == size) {
        return;
    }
    (void)munmap((void *)base, size);
}
