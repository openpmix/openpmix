/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Cross-process tests for the TMA path: the src/class object model and the
 * TMA-aware containers built inside a shared-memory segment and used from
 * more than one process.
 *
 * WHY THIS TEST EXISTS
 *
 * Everything else under test/unit/class is single-process, because
 * everything in src/class is a single-process data structure. The one
 * exception is the TMA: pmix_object_t embeds an allocator, and when it is
 * set the object and everything it allocates live in a caller-provided
 * arena -- which is how gds/shmem3 places PMIx objects into an mmap'd
 * segment that a server shares with its local clients. Two claims in
 * src/class/AGENTS.md rest on that path and were asserted rather than
 * tested:
 *
 *   1. The reference count is a C11 atomic specifically so that
 *      PMIX_RETAIN/PMIX_RELEASE are well defined "from any process sharing
 *      a TMA segment". The per-object pthread_mutex_t it replaced was
 *      never PTHREAD_PROCESS_SHARED, so this was undefined before.
 *
 *   2. pmix_hash_table_t and pmix_pointer_array_t allocate through
 *      pmix_obj_get_tma() rather than libc precisely so a second process
 *      can read what the first one built.
 *
 * WHAT THIS DOES AND DOES NOT MODEL
 *
 * The second process here comes from fork() over a MAP_SHARED mapping, so
 * the segment lands at the same address in both and the function pointers
 * inside the embedded pmix_tma_t stay valid. Real gds/shmem3 peers are
 * unrelated processes that map the segment at a negotiated fixed address
 * and whose method pointers are *not* comparable -- which is why
 * pmix_hash_table.c skips its key-type assert for TMA tables. So this
 * covers the memory model and the container layout across a process
 * boundary; it does not cover segment negotiation or attach. That belongs
 * to gds/shmem3 and the swarm group tests.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_object.h"
#include "src/class/pmix_pointer_array.h"

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

/* ------------------------------------------------------------------ */
/* A minimal bump allocator over an mmap'd segment.                     */
/*                                                                      */
/* The cursor lives *inside* the segment, reached through the TMA's     */
/* data_ptr, so an allocation made by either process is seen by both -- */
/* the same arrangement gds/shmem3 uses. Every allocation carries a     */
/* size header so realloc() can copy the right number of bytes; the     */
/* real component keeps a registry for that, which is more than this    */
/* needs.                                                              */
/*                                                                      */
/* Note that every pmix_tma_t member is filled in. A partially           */
/* initialized TMA is a bug: pmix_obj_get_tma() decides "this object     */
/* has a custom allocator" by testing tma_malloc alone.                  */
/* ------------------------------------------------------------------ */

#define SEG_SIZE   (4 * 1024 * 1024)
#define ALIGNMENT  16

typedef struct {
    /* Bump cursor: an offset, not a pointer, so it is meaningful to any
     * process regardless of where the segment landed. */
    size_t cursor;
    size_t capacity;
} arena_hdr_t;

/* Base of the mapping in this process. With fork() it is the same value
 * in both, but keeping the cursor as an offset means nothing here depends
 * on that. */
static unsigned char *seg_base = NULL;

static void *arena_alloc(pmix_tma_t *tma, size_t size)
{
    arena_hdr_t *hdr = (arena_hdr_t *) tma->data_context;
    size_t need, off;
    unsigned char *p;

    if (0 == size) {
        return NULL;
    }
    /* header + payload, rounded up to ALIGNMENT */
    need = (sizeof(size_t) + size + ALIGNMENT - 1) & ~((size_t) ALIGNMENT - 1);
    if (hdr->cursor + need > hdr->capacity) {
        return NULL;
    }
    off = hdr->cursor;
    hdr->cursor += need;

    p = seg_base + off;
    memcpy(p, &size, sizeof(size_t));
    return p + sizeof(size_t);
}

static size_t arena_size_of(void *ptr)
{
    size_t size;
    memcpy(&size, (unsigned char *) ptr - sizeof(size_t), sizeof(size_t));
    return size;
}

static void *tma_shared_malloc(pmix_tma_t *tma, size_t size)
{
    return arena_alloc(tma, size);
}

static void *tma_shared_calloc(pmix_tma_t *tma, size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p;

    if (0 != nmemb && total / nmemb != size) {
        return NULL; /* overflow */
    }
    p = arena_alloc(tma, total);
    if (NULL != p) {
        memset(p, 0, total);
    }
    return p;
}

static void *tma_shared_realloc(pmix_tma_t *tma, void *ptr, size_t size)
{
    void *fresh;
    size_t old;

    if (NULL == ptr) {
        return arena_alloc(tma, size);
    }
    if (0 == size) {
        return NULL;
    }
    old = arena_size_of(ptr);
    if (size <= old) {
        return ptr;
    }
    fresh = arena_alloc(tma, size);
    if (NULL != fresh) {
        memcpy(fresh, ptr, old);
    }
    return fresh;
}

static char *tma_shared_strdup(pmix_tma_t *tma, const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = (char *) arena_alloc(tma, len);

    if (NULL != p) {
        memcpy(p, s, len);
    }
    return p;
}

static void tma_shared_free(pmix_tma_t *tma, void *ptr)
{
    /* A bump allocator does not reclaim, exactly as gds/shmem3's does not.
     * It still has to be non-NULL: PMIX_RELEASE tests tma_free to decide
     * whether to hand the object back to the TMA or to libc free(), and a
     * NULL here would send segment memory to free(). */
    PMIX_HIDE_UNUSED_PARAMS(tma, ptr);
}

static void tma_shared_init(pmix_tma_t *tma, arena_hdr_t *hdr)
{
    tma->tma_malloc = tma_shared_malloc;
    tma->tma_calloc = tma_shared_calloc;
    tma->tma_realloc = tma_shared_realloc;
    tma->tma_strdup = tma_shared_strdup;
    tma->tma_free = tma_shared_free;
    tma->data_context = hdr;
    tma->data_ptr = NULL;
}

/* ------------------------------------------------------------------ */
/* Everything the two processes share lives here, at the front of the   */
/* segment.                                                            */
/* ------------------------------------------------------------------ */

#define NKIDS      6
#define NSPINS     200000
#define NKEYS      64
#define NSLOTS     40

typedef struct {
    arena_hdr_t arena;
    pmix_tma_t tma;
    /* Objects the parent builds before forking. */
    pmix_object_t *shared_obj;
    pmix_hash_table_t *ht;
    pmix_pointer_array_t *pa;
    /* One result slot per child, so output is not interleaved. */
    int kid_ok[NKIDS];
    int kid_detail[NKIDS];
    /* Start barrier. Without it the children tend to run one after another
     * -- each is cheap enough to finish before the next is scheduled -- and
     * a broken (non-atomic) count then survives by luck. Holding them all
     * at the gate until every one is up puts them in the segment at the
     * same time, which is the condition under test. */
    atomic_int ready;
    atomic_int go;
} shared_t;

static shared_t *sh = NULL;

/* ------------------------------------------------------------------ */
/* The reference count across processes                                 */
/* ------------------------------------------------------------------ */

/* Each child hammers retain/release on one object in the segment and then
 * drops exactly one net reference. If the count is a genuine C11 atomic in
 * shared memory, the parent sees exactly 1 left afterwards. A lost update
 * shows up as a count above 1 (leak) or below it (premature destruction).
 *
 * This is the property the move off pthread_mutex_t bought: that mutex was
 * embedded in the object, lived in the segment, and was never initialized
 * PTHREAD_PROCESS_SHARED, so this arithmetic was undefined. */
static void test_refcount_across_processes(void)
{
    pmix_object_t *obj = sh->shared_obj;
    int i, count;
    pid_t kids[NKIDS];

    /* one reference for each child to drop, plus the parent's own */
    for (i = 0; i < NKIDS; i++) {
        PMIX_RETAIN(obj);
    }
    report("refcount: parent staged 1 + NKIDS references",
           (NKIDS + 1) == obj->obj_reference_count);

    for (i = 0; i < NKIDS; i++) {
        kids[i] = fork();
        if (0 == kids[i]) {
            int j;
            /* line up, then start together */
            atomic_fetch_add_explicit(&sh->ready, 1, memory_order_release);
            while (0 == atomic_load_explicit(&sh->go, memory_order_acquire)) {
                /* spin */
            }
            /* balanced traffic first: contention without a net change */
            for (j = 0; j < NSPINS; j++) {
                PMIX_RETAIN(obj);
                pmix_obj_update(obj, -1);
            }
            /* then the one reference this child owns */
            pmix_obj_update(obj, -1);
            _exit(0);
        }
        if (0 > kids[i]) {
            report("refcount: fork failed", 0);
            return;
        }
    }
    while (NKIDS > atomic_load_explicit(&sh->ready, memory_order_acquire)) {
        /* wait for every child to reach the gate */
    }
    atomic_store_explicit(&sh->go, 1, memory_order_release);

    for (i = 0; i < NKIDS; i++) {
        int status = 0;
        waitpid(kids[i], &status, 0);
    }

    count = obj->obj_reference_count;
    report("refcount: exactly the parent's reference survives NKIDS children",
           1 == count);
    if (1 != count) {
        fprintf(stdout, "        (count was %d, expected 1 after %d children x %d spins)\n",
                count, NKIDS, NSPINS);
    }
}

/* ------------------------------------------------------------------ */
/* Containers built in the segment, read from another process           */
/* ------------------------------------------------------------------ */

/* pmix_hash_table_t and pmix_pointer_array_t allocate through the object's
 * TMA rather than libc so that their storage lands in the segment. If any
 * of it came from the heap instead, a second process would read its own
 * unrelated memory at that address -- or nothing mapped at all. */
static void test_containers_read_from_another_process(void)
{
    uint32_t k;
    int i;
    pid_t kids[NKIDS];

    for (i = 0; i < NKIDS; i++) {
        kids[i] = fork();
        if (0 == kids[i]) {
            int ok = 1, detail = 0;
            void *val = NULL;

            /* every uint32 key the parent stored */
            for (k = 0; k < NKEYS; k++) {
                if (PMIX_SUCCESS != pmix_hash_table_get_value_uint32(sh->ht, k, &val)
                    || (uintptr_t) val != (uintptr_t) (k + 1000)) {
                    ok = 0;
                    detail = 1;
                    break;
                }
            }
            /* a key that was never stored still reports not-found */
            if (ok && PMIX_ERR_NOT_FOUND
                          != pmix_hash_table_get_value_uint32(sh->ht, NKEYS + 1, &val)) {
                ok = 0;
                detail = 2;
            }
            /* the ptr-keyed entry, whose key bytes the table copied into
             * the segment through the TMA */
            if (ok) {
                const char *pkey = "shared-ptr-key";
                if (PMIX_SUCCESS
                        != pmix_hash_table_get_value_ptr(sh->ht, pkey, strlen(pkey), &val)
                    || (uintptr_t) val != 0x5150) {
                    ok = 0;
                    detail = 3;
                }
            }
            /* and the pointer array */
            if (ok) {
                int s;
                for (s = 0; s < NSLOTS; s++) {
                    if ((uintptr_t) pmix_pointer_array_get_item(sh->pa, s)
                        != (uintptr_t) (s + 1)) {
                        ok = 0;
                        detail = 4;
                        break;
                    }
                }
            }
            sh->kid_ok[i] = ok;
            sh->kid_detail[i] = detail;
            _exit(ok ? 0 : 1);
        }
        if (0 > kids[i]) {
            report("containers: fork failed", 0);
            return;
        }
    }

    {
        int all = 1, worst = 0;
        for (i = 0; i < NKIDS; i++) {
            int status = 0;
            waitpid(kids[i], &status, 0);
            if (!sh->kid_ok[i]) {
                all = 0;
                worst = sh->kid_detail[i];
            }
        }
        report("containers: every child read the parent's hash table and pointer array",
               all);
        if (!all) {
            static const char *why[] = {"?", "uint32 lookup", "absent-key lookup",
                                        "ptr-key lookup", "pointer array"};
            fprintf(stdout, "        (first failure: %s)\n",
                    why[(worst >= 0 && worst <= 4) ? worst : 0]);
        }
    }
}

/* A child writes, the parent reads afterwards. Only one process mutates at
 * a time: these containers are not internally synchronized, and that is
 * deliberate -- see src/class/AGENTS.md. What is being checked is that the
 * *storage* is genuinely shared in both directions, not that concurrent
 * mutation is safe. It is not. */
static void test_child_writes_visible_to_parent(void)
{
    pid_t kid;
    int status = 0;
    void *val = NULL;
    int idx;

    kid = fork();
    if (0 == kid) {
        pmix_hash_table_set_value_uint32(sh->ht, 9000, (void *) (uintptr_t) 0xabcd);
        sh->kid_detail[0] = pmix_pointer_array_add(sh->pa, (void *) (uintptr_t) 0xfeed);
        _exit(0);
    }
    if (0 > kid) {
        report("child writes: fork failed", 0);
        return;
    }
    waitpid(kid, &status, 0);

    report("child writes: parent finds the key the child stored",
           PMIX_SUCCESS == pmix_hash_table_get_value_uint32(sh->ht, 9000, &val)
               && (uintptr_t) val == 0xabcd);

    idx = sh->kid_detail[0];
    report("child writes: parent finds the pointer the child added",
           0 <= idx && (uintptr_t) pmix_pointer_array_get_item(sh->pa, idx) == 0xfeed);
}

/* An object created with PMIX_NEW(type, tma) must both live in the segment
 * and carry the allocator forward, or a container it builds would quietly
 * allocate from the heap. */
static void test_objects_land_in_the_segment(void)
{
    unsigned char *lo = seg_base;
    unsigned char *hi = seg_base + SEG_SIZE;

    report("segment: the shared object was allocated inside it",
           (unsigned char *) sh->shared_obj >= lo && (unsigned char *) sh->shared_obj < hi);
    report("segment: the hash table was allocated inside it",
           (unsigned char *) sh->ht >= lo && (unsigned char *) sh->ht < hi);
    report("segment: the pointer array was allocated inside it",
           (unsigned char *) sh->pa >= lo && (unsigned char *) sh->pa < hi);

    report("segment: the hash table reports a custom allocator",
           NULL != pmix_obj_get_tma(&sh->ht->super));
    report("segment: the pointer array reports a custom allocator",
           NULL != pmix_obj_get_tma(&sh->pa->super));

    /* the containers' own storage, not just their headers */
    report("segment: the hash table's slot array is inside it",
           (unsigned char *) sh->ht->ht_table >= lo
               && (unsigned char *) sh->ht->ht_table < hi);
    report("segment: the pointer array's slots are inside it",
           (unsigned char *) sh->pa->addr >= lo && (unsigned char *) sh->pa->addr < hi);
    report("segment: the pointer array's free_bits are inside it",
           (unsigned char *) sh->pa->free_bits >= lo
               && (unsigned char *) sh->pa->free_bits < hi);
}

/* ------------------------------------------------------------------ */

static int build_shared_state(void)
{
    uint32_t k;
    int i;
    const char *pkey = "shared-ptr-key";

    seg_base = (unsigned char *) mmap(NULL, SEG_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (MAP_FAILED == (void *) seg_base) {
        perror("mmap");
        return -1;
    }

    /* The shared header sits at the front; the arena is everything after. */
    sh = (shared_t *) seg_base;
    memset(sh, 0, sizeof(*sh));
    atomic_init(&sh->ready, 0);
    atomic_init(&sh->go, 0);
    sh->arena.cursor = (sizeof(shared_t) + ALIGNMENT - 1) & ~((size_t) ALIGNMENT - 1);
    sh->arena.capacity = SEG_SIZE;
    tma_shared_init(&sh->tma, &sh->arena);

    sh->shared_obj = (pmix_object_t *) PMIX_NEW(pmix_list_item_t, &sh->tma);
    if (NULL == sh->shared_obj) {
        fprintf(stderr, "could not allocate the shared object\n");
        return -1;
    }

    sh->ht = PMIX_NEW(pmix_hash_table_t, &sh->tma);
    if (NULL == sh->ht || PMIX_SUCCESS != pmix_hash_table_init(sh->ht, NKEYS * 4)) {
        fprintf(stderr, "could not build the shared hash table\n");
        return -1;
    }
    for (k = 0; k < NKEYS; k++) {
        if (PMIX_SUCCESS
            != pmix_hash_table_set_value_uint32(sh->ht, k, (void *) (uintptr_t) (k + 1000))) {
            fprintf(stderr, "could not populate the shared hash table\n");
            return -1;
        }
    }

    sh->pa = PMIX_NEW(pmix_pointer_array_t, &sh->tma);
    if (NULL == sh->pa
        || PMIX_SUCCESS != pmix_pointer_array_init(sh->pa, NSLOTS * 2, NSLOTS * 8, 8)) {
        fprintf(stderr, "could not build the shared pointer array\n");
        return -1;
    }
    for (i = 0; i < NSLOTS; i++) {
        if (0 > pmix_pointer_array_add(sh->pa, (void *) (uintptr_t) (i + 1))) {
            fprintf(stderr, "could not populate the shared pointer array\n");
            return -1;
        }
    }

    /* The ptr-keyed entry goes in last: it is the one whose *key bytes*
     * the table copies through the TMA, so it only works if set_value_ptr
     * really is allocating from the segment. */
    if (PMIX_SUCCESS
        != pmix_hash_table_set_value_ptr(sh->ht, pkey, strlen(pkey),
                                         (void *) (uintptr_t) 0x5150)) {
        fprintf(stderr, "could not store the shared ptr key\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== src/class cross-process (TMA) tests ===\n\n");

    if (0 != build_shared_state()) {
        fprintf(stdout, "\nResults: could not set up the shared segment\n\n");
        return 1;
    }

    test_objects_land_in_the_segment();
    test_containers_read_from_another_process();
    test_child_writes_visible_to_parent();
    test_refcount_across_processes();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
