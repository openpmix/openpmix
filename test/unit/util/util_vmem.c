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
 * The cases that need this process's own map SKIP rather than fail where
 * there is no /proc/self/maps to scan (any non-Linux host, which is also
 * where gds/shmem3 - the only caller - is not built). The scan itself is
 * held against maps written by the test, through
 * pmix_vmem_find_hole_in_map(), and those cases run everywhere: what the
 * scan has to get right is a property of the map it is handed, and a
 * live /proc/self/maps is neither reproducible nor steerable.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "src/util/pmix_string_copy.h"
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


/* ------------------------------------------------------------------ */
/* Scanning a map we wrote ourselves.                                  */

/* A synthetic /proc/self/maps.  The columns after the address range are
 * what the kernel prints and what the parser skips past; only the range
 * and the trailing name matter here. */
#define MAP_COLS "r-xp 00000000 08:02 1234567 "

static char mapdir[256];

static const char *write_map(const char *name, const char *contents)
{
    static char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", mapdir, name);
    f = fopen(path, "w");
    if (NULL == f) {
        return NULL;
    }
    fputs(contents, f);
    fclose(f);
    return path;
}

static bool scan(const char *path, pmix_vmem_hole_kind_t hkind, size_t size,
                 size_t *addr)
{
    *addr = 0;
    return PMIX_SUCCESS == pmix_vmem_find_hole_in_map(
        path, hkind, 0, false, addr, size);
}

static void test_biggest_gap(void)
{
    /* Three mappings, two gaps: 0x1000_0000 and 0x8000_0000.  The
     * placement rule aims for the middle of the winner, so the answer
     * has to land inside the second gap and nowhere else. */
    const char *path = write_map("plain",
        "10000000-20000000 " MAP_COLS "/lib/one.so\n"
        "30000000-40000000 " MAP_COLS "/lib/two.so\n"
        "c0000000-d0000000 " MAP_COLS "/lib/three.so\n");
    size_t addr = 0;

    if (NULL == path) {
        report("biggest: wrote a map (skipped)", 1);
        return;
    }
    if (!scan(path, VMEM_HOLE_BIGGEST, 0x1000000UL, &addr)) {
        report("biggest: finds the larger of two gaps", 0);
        return;
    }
    report("biggest: finds the larger of two gaps",
           0x40000000UL <= addr && addr + 0x1000000UL <= 0xc0000000UL);
}

static void test_tagged_entry_does_not_swallow_the_next(void)
{
    /* [vdso] is neither [heap] nor [stack], so it takes the parser's
     * "some other tag" arm.  That arm used to truncate the line at its
     * newline, which made the scan read the NEXT entry as the tail of
     * this one and never account for it - so the gap after [vdso] was
     * measured to the entry after the one that was swallowed, and
     * covered a range that is mapped.
     *
     * Here the swallowed entry is the 0x40000000-0xb0000000 library.
     * Against the old code the biggest gap is 0x20001000-0xc0000000,
     * whose midpoint sits squarely inside it. */
    const char *path = write_map("tagged",
        "10000000-20000000 " MAP_COLS "/lib/one.so\n"
        "20000000-20001000 " MAP_COLS "[vdso]\n"
        "40000000-b0000000 " MAP_COLS "/lib/big.so\n"
        "c0000000-d0000000 " MAP_COLS "/lib/three.so\n");
    size_t addr = 0;

    if (NULL == path) {
        report("tagged: wrote a map (skipped)", 1);
        return;
    }
    if (!scan(path, VMEM_HOLE_BIGGEST, 0x1000000UL, &addr)) {
        report("tagged: an entry after a tagged one is still accounted for", 0);
        return;
    }
    report("tagged: an entry after a tagged one is still accounted for",
           addr + 0x1000000UL <= 0x40000000UL
               || 0xb0000000UL <= addr);
}

static void test_unparseable_entry_does_not_reach_back_to_zero(void)
{
    /* A line the parser cannot read tells us nothing about what is
     * mapped.  Carrying its zeroed end forward as "the previous entry
     * ended here" puts the next gap's start at address 0, and the hole
     * that manufactures spans everything below the following entry -
     * mappings included.
     *
     * The geometry matters, or the case proves nothing.  The bogus hole
     * has to WIN, and the midpoint the placement rule then aims for has
     * to land inside a range this map says is occupied.  Here the bogus
     * hole is [0, 0x70000000), whose 64MB-aligned midpoint is
     * 0x3c000000 - inside one.so.  With the entry skipped properly the
     * winner is [0, 0x30000000) and the answer is 0x1c000000, which is
     * free. */
    const char *path = write_map("garbage",
        "30000000-40000000 " MAP_COLS "/lib/one.so\n"
        "not a map line at all\n"
        "70000000-71000000 " MAP_COLS "/lib/two.so\n");
    const size_t want = 0x1000000UL;
    size_t addr = 0;

    if (NULL == path) {
        report("garbage: wrote a map (skipped)", 1);
        return;
    }
    if (!scan(path, VMEM_HOLE_BIGGEST, want, &addr)) {
        report("garbage: an unreadable entry does not manufacture a hole at 0",
               0);
        return;
    }
    report("garbage: an unreadable entry does not manufacture a hole at 0",
           addr + want <= 0x30000000UL
               || (0x40000000UL <= addr && addr + want <= 0x70000000UL)
               || 0x71000000UL <= addr);
}

static void test_long_entry_is_one_entry(void)
{
    /* An entry longer than the scan's 96-byte read buffer arrives in
     * pieces; the remainder must be drained rather than parsed as an
     * entry of its own.  The long name here is what a deeply nested
     * install prefix looks like. */
    const char *path = write_map("longline",
        "10000000-20000000 " MAP_COLS
        "/a-very-long-directory-name/that-keeps-going/and-going/and-going"
        "/and-going/and-going/and-going/libsomething.so.1.2.3\n"
        "c0000000-d0000000 " MAP_COLS "/lib/three.so\n");
    size_t addr = 0;

    if (NULL == path) {
        report("long: wrote a map (skipped)", 1);
        return;
    }
    if (!scan(path, VMEM_HOLE_BIGGEST, 0x1000000UL, &addr)) {
        report("long: an over-long entry is still one entry", 0);
        return;
    }
    report("long: an over-long entry is still one entry",
           0x20000000UL <= addr && addr + 0x1000000UL <= 0xc0000000UL);
}

static void test_heap_and_stack_bound_the_search(void)
{
    const char *path = write_map("heapstack",
        "10000000-20000000 " MAP_COLS "/bin/prog\n"
        "20000000-21000000 " MAP_COLS "[heap]\n"
        "40000000-41000000 " MAP_COLS "/lib/one.so\n"
        "50000000-51000000 " MAP_COLS "[stack]\n"
        "60000000-70000000 " MAP_COLS "[vsyscall]\n");
    size_t addr = 0;

    if (NULL == path) {
        report("heap/stack: wrote a map (skipped)", 1);
        return;
    }

    if (scan(path, VMEM_HOLE_AFTER_HEAP, 0x1000000UL, &addr)) {
        report("after-heap: places in the gap that follows the heap",
               0x21000000UL <= addr && addr + 0x1000000UL <= 0x40000000UL);
    } else {
        report("after-heap: places in the gap that follows the heap", 0);
    }

    if (scan(path, VMEM_HOLE_BEFORE_STACK, 0x1000000UL, &addr)) {
        report("before-stack: places in the gap that precedes the stack",
               0x41000000UL <= addr && addr + 0x1000000UL <= 0x50000000UL);
    } else {
        report("before-stack: places in the gap that precedes the stack", 0);
    }

    if (scan(path, VMEM_HOLE_BEGIN, 0x1000000UL, &addr)) {
        report("begin: places below the first mapping",
               addr + 0x1000000UL <= 0x10000000UL);
    } else {
        report("begin: places below the first mapping", 0);
    }

    /* The scan stops at the stack, so the 0x51000000-0x60000000 gap
     * beyond it is not a candidate however big it is. */
    if (scan(path, VMEM_HOLE_BIGGEST, 0x1000000UL, &addr)) {
        report("biggest: does not look past the stack",
               addr + 0x1000000UL <= 0x50000000UL);
    } else {
        report("biggest: does not look past the stack", 0);
    }
}

static void test_no_room_is_reported(void)
{
    /* The span below the first mapping counts as a gap - prevend starts
     * at 0 - so the request has to be larger than that one too. */
    const char *path = write_map("tight",
        "10000000-20000000 " MAP_COLS "/lib/one.so\n"
        "20001000-30000000 " MAP_COLS "/lib/two.so\n");
    size_t addr = 0;

    if (NULL == path) {
        report("tight: wrote a map (skipped)", 1);
        return;
    }
    report("a request larger than every gap is refused",
           !scan(path, VMEM_HOLE_BIGGEST, 0x20000000UL, &addr));
    report("the span below the first mapping is itself a gap",
           scan(path, VMEM_HOLE_BIGGEST, 0x8000000UL, &addr)
               && addr + 0x8000000UL <= 0x10000000UL);
}

static void test_bad_arguments(void)
{
    size_t addr = 0;

    /* VMEM_HOLE_NONE and VMEM_HOLE_CUSTOM are in the enum but have no
     * search behind them.  This used to be an assert(0) inside the scan
     * loop, so a caller naming one took the process down. */
    report("an unimplemented hole kind is refused, not asserted",
           PMIX_ERR_BAD_PARAM == pmix_vmem_find_hole(
               VMEM_HOLE_NONE, &addr, 0x1000UL));
    report("VMEM_HOLE_CUSTOM is refused the same way",
           PMIX_ERR_BAD_PARAM == pmix_vmem_find_hole(
               VMEM_HOLE_CUSTOM, &addr, 0x1000UL));
    report("a NULL out-parameter is refused",
           PMIX_ERR_BAD_PARAM == pmix_vmem_find_hole(
               VMEM_HOLE_BIGGEST, NULL, 0x1000UL));
    report("a map that cannot be opened is reported",
           PMIX_SUCCESS != pmix_vmem_find_hole_in_map(
               "/nonexistent/no/such/maps", VMEM_HOLE_BIGGEST, 0, false,
               &addr, 0x1000UL));
    {
        uintptr_t base = 1;
        report("reserve refuses a zero size",
               PMIX_ERR_BAD_PARAM == pmix_vmem_reserve(
                   VMEM_HOLE_BIGGEST, 0, 0, &base));
        report("reserve clears its out-parameter", 0 == base);
        report("reserve refuses a NULL out-parameter",
               PMIX_ERR_BAD_PARAM == pmix_vmem_reserve(
                   VMEM_HOLE_BIGGEST, 0, 0x1000UL, NULL));
    }
}

static void test_scan_a_map_we_wrote(void)
{
    char tmpl[256];
    const char *tmp = getenv("TMPDIR");
    size_t i;
    static const char *const names[] = {
        "plain", "tagged", "garbage", "longline", "heapstack", "tight"
    };
    char victim[512];

    snprintf(tmpl, sizeof(tmpl), "%s/pmix_vmem_test_XXXXXX",
             (NULL == tmp || '\0' == tmp[0]) ? "/tmp" : tmp);
    if (NULL == mkdtemp(tmpl)) {
        report("synthetic maps: made a scratch directory (skipped)", 1);
        return;
    }
    pmix_string_copy(mapdir, tmpl, sizeof(mapdir));

    test_biggest_gap();
    test_tagged_entry_does_not_swallow_the_next();
    test_unparseable_entry_does_not_reach_back_to_zero();
    test_long_entry_is_one_entry();
    test_heap_and_stack_bound_the_search();
    test_no_room_is_reported();

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        snprintf(victim, sizeof(victim), "%s/%s", mapdir, names[i]);
        (void) unlink(victim);
    }
    (void) rmdir(mapdir);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "pmix_vmem tests\n");

    /* These need no /proc, so they run on every platform. */
    test_bad_arguments();
    test_scan_a_map_we_wrote();

    if (!have_maps()) {
        /* No /proc/self/maps: nothing here can run, and gds/shmem3 is
         * not built on such a host either. Say so and pass. */
        fprintf(stdout,
                "  SKIP: no usable /proc/self/maps on this platform - the\n"
                "        cases needing this process's own map are skipped\n");
        fprintf(stdout, "SUMMARY: %d passed, %d failed\n", npass, nfail);
        return (0 == nfail) ? 0 : 1;
    }

    test_agreement();
    test_spreading();
    test_offset_avoids_midpoint();
    test_reserve_roundtrip();
    test_reserve_rejects_nonsense();

    fprintf(stdout, "SUMMARY: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
