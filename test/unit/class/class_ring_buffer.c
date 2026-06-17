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

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_ring_buffer_t unit tests ===\n\n");

    test_init();
    test_push_pop_basic();
    test_wraparound();
    test_multiple_wraps();
    test_poke();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
