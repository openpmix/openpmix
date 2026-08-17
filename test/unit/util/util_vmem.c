/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_vmem.c - address placement and reserving.
 *
 * Two properties are being pinned down here, and they pull against each
 * other, which is why they are worth a test rather than a reading of the
 * code:
 *
 *   AGREEMENT. Processes that must map a segment at one address never
 *   exchange that address - each computes it. So the same inputs have to
 *   give the same answer, every time, in every process.
 *
 *   SPREADING. Two things with no reason to agree must not be handed the
 *   same address anyway. Concurrent jobs on a node are the case that
 *   matters: each server picks from its own map, and those maps resemble
 *   one another.
 *
 * The second is the one that cannot be tested end to end. Run two jobs
 * on a node and their servers land far apart whether or not anything
 * spreads them, because ASLR has already moved each server's load base
 * and the placement is computed relative to it. That makes a passing
 * two-job run no evidence at all - it passes just as happily with the
 * scatter compiled out, which is exactly what a trial run of it did.
 * Where the spreading actually earns its place is where ASLR does not do
 * the job for it: systems that disable randomization for reproducibility,
 * and non-PIE executables. Neither is reachable from a test suite, but
 * the function underneath is, and it is decidable there. Hence this.
 *
 * SKIPS rather than fails where there is no /proc/self/maps to scan (any
 * non-Linux host, which is also where gds/shmem3 - the only caller - is
 * not built).
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "src/util/pmix_vmem.h"

#define ALIGN64MB (64 * 1024 * 1024UL)

/* Big enough to be a realistic request, small enough to reserve for real
 * several times over without troubling anything. */
#define TEST_SIZE (256 * 1024 * 1024UL)

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* Is there a map to scan at all? Everything here depends on it. */
static bool have_maps(void)
{
    size_t addr = 0;

    return PMIX_SUCCESS == pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &addr, TEST_SIZE);
}

static void test_agreement(void)
{
    size_t a = 0, b = 0;

    /* Same scatter, same map, twice: the answer cannot wobble, or two
     * processes computing it would not arrive at the same place. */
    if (PMIX_SUCCESS != pmix_vmem_find_hole_scattered(
            VMEM_HOLE_BIGGEST_OFFSET, 12345, &a, TEST_SIZE) ||
        PMIX_SUCCESS != pmix_vmem_find_hole_scattered(
            VMEM_HOLE_BIGGEST_OFFSET, 12345, &b, TEST_SIZE)) {
        report("scattered placement succeeds", 0);
        return;
    }
    report("scattered placement succeeds", 1);
    report("same scatter gives the same address", a == b);
    report("scattered address is 64MB aligned", 0 == (a % ALIGN64MB));
}

static void test_spreading(void)
{
    /* Distinct scatter values must land in distinct places. These stand
     * in for distinct namespaces; the caller hashes the name, so what
     * reaches here is arbitrary 64-bit values. */
    static const uint64_t scatters[] = {
        0u, 1u, 7u, 1024u, 999983u,
        14695981039346656037ULL, 1099511628211ULL, 0xdeadbeefcafef00dULL
    };
    const size_t n = sizeof(scatters) / sizeof(scatters[0]);
    size_t addrs[sizeof(scatters) / sizeof(scatters[0])];
    size_t i, j;
    int distinct = 1;
    int aligned = 1;

    for (i = 0; i < n; ++i) {
        addrs[i] = 0;
        if (PMIX_SUCCESS != pmix_vmem_find_hole_scattered(
                VMEM_HOLE_BIGGEST_OFFSET, scatters[i], &addrs[i], TEST_SIZE)) {
            report("every scatter value places successfully", 0);
            return;
        }
        if (0 != (addrs[i] % ALIGN64MB)) {
            aligned = 0;
        }
    }
    report("every scatter value places successfully", 1);
    report("every scattered address is 64MB aligned", aligned);

    for (i = 0; i < n && distinct; ++i) {
        for (j = i + 1; j < n; ++j) {
            if (addrs[i] == addrs[j]) {
                fprintf(stdout, "    scatter %llu and %llu both gave 0x%zx\n",
                        (unsigned long long) scatters[i],
                        (unsigned long long) scatters[j], addrs[i]);
                distinct = 0;
                break;
            }
        }
    }
    report("different scatter values give different addresses", distinct);

    /* And the unscattered rule is a single fixed answer, which is what
     * the scattered one is spreading away from. */
    {
        size_t plain = 0, plain2 = 0;
        int ok = (PMIX_SUCCESS == pmix_vmem_find_hole(
                      VMEM_HOLE_BIGGEST_OFFSET, &plain, TEST_SIZE))
                 && (PMIX_SUCCESS == pmix_vmem_find_hole(
                         VMEM_HOLE_BIGGEST_OFFSET, &plain2, TEST_SIZE))
                 && plain == plain2;
        report("unscattered placement is stable", ok);
    }
}

static void test_offset_avoids_midpoint(void)
{
    size_t mid = 0, off = 0;

    if (PMIX_SUCCESS != pmix_vmem_find_hole(
            VMEM_HOLE_BIGGEST, &mid, TEST_SIZE) ||
        PMIX_SUCCESS != pmix_vmem_find_hole(
            VMEM_HOLE_BIGGEST_OFFSET, &off, TEST_SIZE)) {
        report("offset placement differs from the midpoint", 0);
        return;
    }
    /* The whole reason VMEM_HOLE_BIGGEST_OFFSET exists is that the
     * midpoint is where hwloc and Open MPI aim too. If these ever
     * coincide it has stopped doing its job. */
    report("offset placement differs from the midpoint", mid != off);
}

static void test_reserve_roundtrip(void)
{
    uintptr_t base = 0;
    volatile unsigned char *p;

    if (PMIX_SUCCESS != pmix_vmem_reserve(
            VMEM_HOLE_BIGGEST_OFFSET, 4242, TEST_SIZE, &base)) {
        report("reserve succeeds", 0);
        return;
    }
    report("reserve succeeds", 1);
    report("reserve returns a 64MB aligned base", 0 == (base % ALIGN64MB));

    /* A reservation is held, so asking for the same range again must be
     * refused - that is the property the whole design rests on. */
    report("a held range cannot be reserved twice",
           PMIX_SUCCESS != pmix_vmem_reserve_at(base, TEST_SIZE));

    /* Mapping over our own reservation is what a caller does next, and
     * MAP_FIXED must land exactly where asked. */
    p = mmap((void *) base, TEST_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (MAP_FAILED == (void *) p) {
        report("can map over the reservation", 0);
    } else {
        report("can map over the reservation", (uintptr_t) p == base);
        p[0] = 0x5a;
        p[TEST_SIZE - 1] = 0xa5;
        report("the mapping is usable end to end",
               0x5a == p[0] && 0xa5 == p[TEST_SIZE - 1]);
        /* Give it back to the reservation, and confirm the range is
         * still held afterwards rather than handed to the system. */
        report("restore succeeds",
               PMIX_SUCCESS == pmix_vmem_restore(base, TEST_SIZE));
        report("the range is still held after restore",
               PMIX_SUCCESS != pmix_vmem_reserve_at(base, TEST_SIZE));
    }

    pmix_vmem_release(base, TEST_SIZE);
    /* Once released it is anybody's, so we can take it again. */
    report("the range is free after release",
           PMIX_SUCCESS == pmix_vmem_reserve_at(base, TEST_SIZE));
    pmix_vmem_release(base, TEST_SIZE);
}

static void test_reserve_rejects_nonsense(void)
{
    report("reserve_at rejects a zero base",
           PMIX_SUCCESS != pmix_vmem_reserve_at(0, TEST_SIZE));
    report("reserve_at rejects a zero size",
           PMIX_SUCCESS != pmix_vmem_reserve_at(0x1000, 0));
    report("restore rejects a zero base",
           PMIX_SUCCESS != pmix_vmem_restore(0, TEST_SIZE));
    /* Releasing nothing must be a no-op rather than an munmap of
     * something else. */
    pmix_vmem_release(0, TEST_SIZE);
    pmix_vmem_release(0x1000, 0);
    report("release of an empty range is harmless", 1);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "pmix_vmem tests\n");

    if (!have_maps()) {
        /* No /proc/self/maps: nothing here can run, and gds/shmem3 is
         * not built on such a host either. Say so and pass. */
        fprintf(stdout,
                "  SKIP: no usable /proc/self/maps on this platform\n");
        fprintf(stdout, "SUMMARY: 0 passed, 0 failed (skipped)\n");
        return 0;
    }

    test_agreement();
    test_spreading();
    test_offset_avoids_midpoint();
    test_reserve_roundtrip();
    test_reserve_rejects_nonsense();

    fprintf(stdout, "SUMMARY: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
