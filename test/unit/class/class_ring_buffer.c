/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_ring_buffer_t:
 *   init, push, pop (FIFO order), wraparound displacement, poke.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/class/pmix_ring_buffer.h"

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
/* init                                                                 */
/* ------------------------------------------------------------------ */

static void test_init(void)
{
    pmix_ring_buffer_t rb;
    PMIX_CONSTRUCT(&rb, pmix_ring_buffer_t);
    int rc = pmix_ring_buffer_init(&rb, 4);
    report("init: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("init: size is 4", 4 == rb.size);
    PMIX_DESTRUCT(&rb);
}

/* ------------------------------------------------------------------ */
/* push / pop (not full, FIFO order)                                    */
/* ------------------------------------------------------------------ */

static void test_push_pop_basic(void)
{
    pmix_ring_buffer_t rb;
    PMIX_CONSTRUCT(&rb, pmix_ring_buffer_t);
    pmix_ring_buffer_init(&rb, 4);

    char *a = "alpha";
    char *b = "beta";

    void *displaced = pmix_ring_buffer_push(&rb, a);
    report("push first: no displacement (returns NULL)", NULL == displaced);
    displaced = pmix_ring_buffer_push(&rb, b);
    report("push second: no displacement (returns NULL)", NULL == displaced);

    void *out = pmix_ring_buffer_pop(&rb);
    report("pop first: returns 'alpha' (oldest)", out == (void *) a);
    out = pmix_ring_buffer_pop(&rb);
    report("pop second: returns 'beta'", out == (void *) b);
    out = pmix_ring_buffer_pop(&rb);
    report("pop empty: returns NULL", NULL == out);

    PMIX_DESTRUCT(&rb);
}

/* ------------------------------------------------------------------ */
/* wraparound / displacement when ring is full                          */
/* ------------------------------------------------------------------ */

static void test_wraparound(void)
{
    pmix_ring_buffer_t rb;
    PMIX_CONSTRUCT(&rb, pmix_ring_buffer_t);
    pmix_ring_buffer_init(&rb, 3);

    char *a = "a", *b = "b", *c = "c", *d = "d";

    pmix_ring_buffer_push(&rb, a);
    pmix_ring_buffer_push(&rb, b);
    pmix_ring_buffer_push(&rb, c);

    /* Ring is full; pushing 'd' displaces the oldest ('a') */
    void *displaced = pmix_ring_buffer_push(&rb, d);
    report("wraparound: displaced element is 'a' (oldest)", displaced == (void *) a);

    /* Remaining elements dequeue in FIFO order: b, c, d */
    void *out = pmix_ring_buffer_pop(&rb);
    report("wraparound pop 1: 'b'", out == (void *) b);
    out = pmix_ring_buffer_pop(&rb);
    report("wraparound pop 2: 'c'", out == (void *) c);
    out = pmix_ring_buffer_pop(&rb);
    report("wraparound pop 3: 'd'", out == (void *) d);
    out = pmix_ring_buffer_pop(&rb);
    report("wraparound pop 4: empty -> NULL", NULL == out);

    PMIX_DESTRUCT(&rb);
}

/* ------------------------------------------------------------------ */
/* Multiple wraparounds                                                  */
/* ------------------------------------------------------------------ */

static void test_multiple_wraps(void)
{
    pmix_ring_buffer_t rb;
    PMIX_CONSTRUCT(&rb, pmix_ring_buffer_t);
    pmix_ring_buffer_init(&rb, 2);

    char *a = "a", *b = "b", *c = "c", *d = "d";

    pmix_ring_buffer_push(&rb, a);
    pmix_ring_buffer_push(&rb, b);

    void *dis1 = pmix_ring_buffer_push(&rb, c);
    report("multi_wrap: push c displaces a", dis1 == (void *) a);

    void *dis2 = pmix_ring_buffer_push(&rb, d);
    report("multi_wrap: push d displaces b", dis2 == (void *) b);

    void *out = pmix_ring_buffer_pop(&rb);
    report("multi_wrap: pop returns c", out == (void *) c);
    out = pmix_ring_buffer_pop(&rb);
    report("multi_wrap: pop returns d", out == (void *) d);

    PMIX_DESTRUCT(&rb);
}

/* ------------------------------------------------------------------ */
/* poke                                                                 */
/*                                                                      */
/* poke(i >= 0): element at offset i from tail (oldest).               */
/*   poke(0) returns the oldest item (= what pop would return next).   */
/* poke(i < 0): the most recently pushed element (at head - 1).        */
/* ------------------------------------------------------------------ */

static void test_poke(void)
{
    pmix_ring_buffer_t rb;
    PMIX_CONSTRUCT(&rb, pmix_ring_buffer_t);
    pmix_ring_buffer_init(&rb, 4);

    char *a = "a", *b = "b", *c = "c";
    pmix_ring_buffer_push(&rb, a);
    pmix_ring_buffer_push(&rb, b);
    pmix_ring_buffer_push(&rb, c);

    /* poke(0) -> oldest (what pop would return first) = 'a' */
    void *p = pmix_ring_buffer_poke(&rb, 0);
    report("poke(0): returns oldest 'a'", p == (void *) a);

    /* poke(1) -> second oldest = 'b' */
    p = pmix_ring_buffer_poke(&rb, 1);
    report("poke(1): returns second 'b'", p == (void *) b);

    /* poke(-1) -> most recently pushed = 'c' */
    p = pmix_ring_buffer_poke(&rb, -1);
    report("poke(-1): returns most recent 'c'", p == (void *) c);

    /* poke() does not remove items */
    void *out = pmix_ring_buffer_pop(&rb);
    report("poke does not remove: pop still returns 'a'", out == (void *) a);

    PMIX_DESTRUCT(&rb);
}

/* ------------------------------------------------------------------ */
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* init() used to check only for a NULL ring. A non-positive size sailed
 * through to calloc(), which returned a zero-byte block, and the first
 * push() wrote through addr[0]. */
static void test_init_bad_params(void)
{
    pmix_ring_buffer_t ring;

    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);

    report("init NULL ring: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_ring_buffer_init(NULL, 4));
    report("init size 0: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_ring_buffer_init(&ring, 0));
    report("init negative size: PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_ring_buffer_init(&ring, -1));
    report("rejected init leaves no allocation", NULL == ring.addr);
    report("rejected init leaves size 0", 0 == ring.size);

    /* and a good size still works afterwards */
    report("init size 4 after rejections: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_ring_buffer_init(&ring, 4));
    report("size 4 ring accepts a push", NULL == pmix_ring_buffer_push(&ring, (void *) 0x1));

    PMIX_DESTRUCT(&ring);
}

/* A ring of one is the degenerate case: every push displaces the previous
 * occupant. */
static void test_size_one(void)
{
    pmix_ring_buffer_t ring;
    void *evicted;

    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);
    report("size 1: init succeeds", PMIX_SUCCESS == pmix_ring_buffer_init(&ring, 1));

    report("size 1: first push displaces nothing",
           NULL == pmix_ring_buffer_push(&ring, (void *) 0x11));
    evicted = pmix_ring_buffer_push(&ring, (void *) 0x22);
    report("size 1: second push displaces the first", (void *) 0x11 == evicted);
    report("size 1: pop returns the survivor", (void *) 0x22 == pmix_ring_buffer_pop(&ring));
    report("size 1: pop of an empty ring is NULL", NULL == pmix_ring_buffer_pop(&ring));

    PMIX_DESTRUCT(&ring);
}

/* The destructor has to leave nothing addressing the freed ring. Calling
 * PMIX_DESTRUCT twice is a contract violation the object system's
 * magic-ID assert catches under --enable-debug, so it is not exercised;
 * what is checked is that no dangling state survives. */
static void test_destruct_leaves_constructed_state(void)
{
    pmix_ring_buffer_t ring;

    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);
    pmix_ring_buffer_init(&ring, 4);
    pmix_ring_buffer_push(&ring, (void *) 0x1);

    PMIX_DESTRUCT(&ring);
    report("destruct: size reset to 0", 0 == ring.size);
    report("destruct: addr reset to NULL", NULL == ring.addr);

    /* re-usable afterwards */
    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);
    report("re-init after destruct: PMIX_SUCCESS", PMIX_SUCCESS == pmix_ring_buffer_init(&ring, 4));
    report("re-init after destruct: push works", NULL == pmix_ring_buffer_push(&ring, (void *) 0x2));
    report("re-init after destruct: pop returns it", (void *) 0x2 == pmix_ring_buffer_pop(&ring));
    PMIX_DESTRUCT(&ring);
}

/* The destructor freed addr and zeroed size but left head and tail where
 * the last push put them. tail is this class's "is anything on the ring"
 * flag, so a destructed ring still answered "yes" and pop() dereferenced
 * addr[tail] -- through the NULL the destructor had just installed.
 *
 * (all builds -- there is no assert on this path in either configuration) */
static void test_destruct_resets_head_and_tail(void)
{
    pmix_ring_buffer_t ring;

    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);
    pmix_ring_buffer_init(&ring, 4);
    pmix_ring_buffer_push(&ring, (void *) 0xa1);
    pmix_ring_buffer_push(&ring, (void *) 0xa2);

    PMIX_DESTRUCT(&ring);
    report("destruct: head back to 0", 0 == ring.head);
    report("destruct: tail back to -1 (ring reads as empty)", -1 == ring.tail);

    /* The payoff: this used to segfault rather than return NULL. */
    report("pop on a destructed ring returns NULL", NULL == pmix_ring_buffer_pop(&ring));
    report("poke on a destructed ring returns NULL", NULL == pmix_ring_buffer_poke(&ring, 0));
}

/* pmix_ring_buffer_init() never touched head or tail, so re-initializing a
 * ring that had been used before leaked the old storage and handed back a
 * "fresh" ring that still claimed the previous one's occupancy: the first
 * pop() returned a slot that had never been written.
 *
 * Note that this deliberately does *not* re-CONSTRUCT between the two
 * inits -- re-constructing resets head/tail and hides the defect, which is
 * why the neighbouring teardown case does not catch it. */
static void test_reinit_without_reconstruct(void)
{
    pmix_ring_buffer_t ring;
    void *out;

    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);

    pmix_ring_buffer_init(&ring, 4);
    pmix_ring_buffer_push(&ring, (void *) 0xb1);
    pmix_ring_buffer_push(&ring, (void *) 0xb2);
    pmix_ring_buffer_push(&ring, (void *) 0xb3);

    report("re-init over a used ring: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_ring_buffer_init(&ring, 4));
    report("re-init: head back to 0", 0 == ring.head);
    report("re-init: tail back to -1", -1 == ring.tail);

    out = pmix_ring_buffer_pop(&ring);
    report("re-init: the ring really is empty", NULL == out);

    /* and it behaves normally from there */
    report("re-init: push then pop round-trips",
           NULL == pmix_ring_buffer_push(&ring, (void *) 0xb9));
    report("re-init: pop returns what was pushed", (void *) 0xb9 == pmix_ring_buffer_pop(&ring));

    PMIX_DESTRUCT(&ring);
}

/* Every other class here carries a PMIX_*_STATIC_INIT; this one did not.
 * The value of the macro is entirely in agreeing with the constructor --
 * in particular tail must be -1, not the 0 a plain zero-initializer gives,
 * or the "empty" ring reports an occupant. */
static void test_static_init_matches_constructor(void)
{
    pmix_ring_buffer_t stat = PMIX_RING_BUFFER_STATIC_INIT;
    pmix_ring_buffer_t ctor;

    PMIX_CONSTRUCT(&ctor, pmix_ring_buffer_t);

    report("static init: head matches the constructor", stat.head == ctor.head);
    report("static init: tail matches the constructor", stat.tail == ctor.tail);
    report("static init: size matches the constructor", stat.size == ctor.size);
    report("static init: addr matches the constructor", stat.addr == ctor.addr);

    /* usable without a construct: reads answer "empty" rather than faulting */
    report("static init: pop returns NULL", NULL == pmix_ring_buffer_pop(&stat));
    report("static init: poke returns NULL", NULL == pmix_ring_buffer_poke(&stat, 0));

    /* and init makes it a working ring */
    report("static init: init succeeds", PMIX_SUCCESS == pmix_ring_buffer_init(&stat, 2));
    report("static init: push works", NULL == pmix_ring_buffer_push(&stat, (void *) 0xc1));
    report("static init: pop returns it", (void *) 0xc1 == pmix_ring_buffer_pop(&stat));

    /* A STATIC_INIT object carries the BASE class, so PMIX_DESTRUCT on
     * it runs pmix_object_t's (empty) chain and never the derived
     * destructor - which means everything the calls above made it
     * allocate would leak. That is the whole point of the invariant in
     * src/class/AGENTS.md: STATIC_INIT gives a defined state, and a
     * PMIX_CONSTRUCT is what makes an object a live instance of its own
     * class. Here the object has been used without one, deliberately, so
     * name its class before tearing it down. */
    stat.super.obj_class = PMIX_CLASS(pmix_ring_buffer_t);
    PMIX_DESTRUCT(&stat);
    PMIX_DESTRUCT(&ctor);
}

/* Regression (all builds): push() was the one accessor with no guard at
 * all. pop() has always tested "tail == -1" and poke() "size <= i", but
 * push() went straight to addr[head] -- so a ring that had only been
 * PMIX_NEW'd/PMIX_CONSTRUCT'd, or one already destructed, dereferenced the
 * NULL addr instead of declining. That is the easiest state in this class
 * to reach by accident: a teardown path that pushes one last item after
 * the ring is gone.
 *
 * Backing the fix out segfaults here in every configuration -- there is no
 * assert on this path in either one. */
static void test_push_without_storage(void)
{
    pmix_ring_buffer_t ring;
    pmix_ring_buffer_t stat = PMIX_RING_BUFFER_STATIC_INIT;

    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);
    report("no storage: push on a constructed-but-un-init'd ring declines",
           NULL == pmix_ring_buffer_push(&ring, (void *) 0x1));
    report("no storage: nothing was recorded", NULL == ring.addr && 0 == ring.size);
    report("no storage: pop still says empty", NULL == pmix_ring_buffer_pop(&ring));

    report("no storage: push on a STATIC_INIT ring declines",
           NULL == pmix_ring_buffer_push(&stat, (void *) 0x2));

    /* and after a destruct, which is the same state by a different route */
    report("no storage: init succeeds", PMIX_SUCCESS == pmix_ring_buffer_init(&ring, 4));
    pmix_ring_buffer_push(&ring, (void *) 0x3);
    PMIX_DESTRUCT(&ring);
    report("no storage: push on a destructed ring declines",
           NULL == pmix_ring_buffer_push(&ring, (void *) 0x4));
    report("no storage: destructed ring still reports empty",
           NULL == pmix_ring_buffer_pop(&ring));

    /* a NULL ring is answered rather than dereferenced, in all three */
    report("NULL ring: push returns NULL", NULL == pmix_ring_buffer_push(NULL, (void *) 0x5));
    report("NULL ring: pop returns NULL", NULL == pmix_ring_buffer_pop(NULL));
    report("NULL ring: poke returns NULL", NULL == pmix_ring_buffer_poke(NULL, 0));

    /* the ring is still usable once it really has storage. Reconstruct
     * first -- PMIX_DESTRUCT twice on one object is a contract violation
     * the object system's magic-ID assert catches, and this suite does
     * not do it. */
    PMIX_CONSTRUCT(&ring, pmix_ring_buffer_t);
    report("no storage: re-init succeeds", PMIX_SUCCESS == pmix_ring_buffer_init(&ring, 2));
    report("no storage: push then works", NULL == pmix_ring_buffer_push(&ring, (void *) 0x6));
    report("no storage: pop returns it", (void *) 0x6 == pmix_ring_buffer_pop(&ring));
    PMIX_DESTRUCT(&ring);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_ring_buffer_t unit tests ===\n\n");

    test_init();
    test_push_pop_basic();
    test_wraparound();
    test_multiple_wraps();
    test_poke();
    test_init_bad_params();
    test_size_one();
    test_destruct_leaves_constructed_state();
    test_destruct_resets_head_and_tail();
    test_reinit_without_reconstruct();
    test_static_init_matches_constructor();
    test_push_without_storage();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
