/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "pmix_common.h"
#include "src/class/pmix_ring_buffer.h"
#include "src/util/pmix_output.h"

static void pmix_ring_buffer_construct(pmix_ring_buffer_t *);
static void pmix_ring_buffer_destruct(pmix_ring_buffer_t *);

PMIX_CLASS_INSTANCE(pmix_ring_buffer_t, pmix_object_t, pmix_ring_buffer_construct,
                    pmix_ring_buffer_destruct);

/*
 * pmix_ring_buffer constructor
 */
static void pmix_ring_buffer_construct(pmix_ring_buffer_t *ring)
{
    ring->head = 0;
    ring->tail = -1;
    ring->size = 0;
    ring->addr = NULL;
}

/*
 * pmix_ring_buffer destructor
 */
static void pmix_ring_buffer_destruct(pmix_ring_buffer_t *ring)
{
    if (NULL != ring->addr) {
        free(ring->addr);
        ring->addr = NULL;
    }

    /* Return to the constructed state, head and tail included. Clearing
     * only addr and size left tail holding a live index over a NULL addr,
     * so pmix_ring_buffer_pop() on a destructed ring took its "something is
     * on the ring" branch and dereferenced addr[tail]. */
    ring->head = 0;
    ring->tail = -1;
    ring->size = 0;
}

/**
 * initialize a ring object
 */
int pmix_ring_buffer_init(pmix_ring_buffer_t *ring, int size)
{
    /* Check for errors. A non-positive size used to be accepted: calloc()
     * would hand back a zero-byte (or wildly mis-sized) block and the very
     * first pmix_ring_buffer_push() would write through addr[0]. */
    if (NULL == ring || size <= 0) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Re-initializing a ring that has been used before leaked its old
     * storage and, worse, kept the old head/tail, so the fresh ring started
     * out claiming entries that were not in it. */
    if (NULL != ring->addr) {
        free(ring->addr);
        ring->addr = NULL;
    }
    ring->size = 0;

    /* Allocate and set the ring to NULL. Pass the count and the element
     * size as calloc()'s two arguments, in that order, so that calloc()
     * can do the multiplication overflow check it exists to do. */
    ring->addr = (char **) calloc((size_t) size, sizeof(char *));
    if (NULL == ring->addr) { /* out of memory */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    ring->size = size;
    /* An empty ring: nothing has been pushed, so there is no oldest entry. */
    ring->head = 0;
    ring->tail = -1;

    return PMIX_SUCCESS;
}

void *pmix_ring_buffer_push(pmix_ring_buffer_t *ring, void *ptr)
{
    char *p = NULL;

    /* There is no storage until pmix_ring_buffer_init() has run, and none
     * again after the destructor has run. Both of the other accessors
     * already survive that state -- pop() tests "tail == -1" and poke()
     * tests "size <= i" -- but this one went straight to addr[head] and
     * dereferenced NULL. It is the easiest of the three to reach: a ring
     * that was only PMIX_NEW'd, or one being pushed to on a teardown path
     * that has already destructed it.
     *
     * Answering NULL is the same answer as "the ring was not full", which
     * is unfortunate but is the only value this signature has to give. It
     * is still strictly better than the fault, and a caller that pushes
     * into an un-init'd ring has a bug either way. */
    if (PMIX_UNLIKELY(NULL == ring || NULL == ring->addr || 0 >= ring->size)) {
        return NULL;
    }

    if (NULL != ring->addr[ring->head]) {
        p = (char *) ring->addr[ring->head];
        if (ring->tail == ring->size - 1) {
            ring->tail = 0;
        } else {
            ring->tail = ring->head + 1;
        }
    }
    ring->addr[ring->head] = (char *) ptr;
    if (ring->tail < 0) {
        ring->tail = ring->head;
    }
    if (ring->head == ring->size - 1) {
        ring->head = 0;
    } else {
        ring->head++;
    }
    return (void *) p;
}

void *pmix_ring_buffer_pop(pmix_ring_buffer_t *ring)
{
    char *p = NULL;

    if (PMIX_UNLIKELY(NULL == ring)) {
        return NULL;
    }
    if (-1 == ring->tail) {
        /* nothing has been put on the ring yet */
        p = NULL;
    } else {
        p = (char *) ring->addr[ring->tail];
        ring->addr[ring->tail] = NULL;
        if (ring->tail == ring->size - 1) {
            ring->tail = 0;
        } else {
            ring->tail++;
        }
        /* see if the ring is empty */
        if (ring->tail == ring->head) {
            ring->tail = -1;
        }
    }
    return (void *) p;
}

void *pmix_ring_buffer_poke(pmix_ring_buffer_t *ring, int i)
{
    char *p = NULL;
    int offset;

    if (PMIX_UNLIKELY(NULL == ring)) {
        return NULL;
    }
    if (ring->size <= i || -1 == ring->tail) {
        p = NULL;
    } else if (i < 0) {
        /* return the value at the head of the ring */
        if (ring->head == 0) {
            p = ring->addr[ring->size - 1];
        } else {
            p = ring->addr[ring->head - 1];
        }
    } else {
        /* calculate the offset of the tail in the ring */
        offset = ring->tail + i;
        /* correct for wrap-around */
        if (ring->size <= offset) {
            offset -= ring->size;
        }
        p = ring->addr[offset];
    }
    return (void *) p;
}
