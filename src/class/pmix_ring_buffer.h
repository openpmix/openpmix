/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
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
/** @file
 *
 */

#ifndef PMIX_RING_BUFFER_H
#define PMIX_RING_BUFFER_H

#include "src/include/pmix_config.h"

#include "src/class/pmix_object.h"
#include "src/include/pmix_prefetch.h"
#include "src/util/pmix_output.h"

BEGIN_C_DECLS

/**
 * dynamic pointer ring
 */
struct pmix_ring_buffer_t {
    /** base class */
    pmix_object_t super;
    /* head/tail indices */
    int head;
    int tail;
    /** size of list, i.e. number of elements in addr */
    int size;
    /** pointer to ring */
    char **addr;
};
/**
 * Convenience typedef
 */
typedef struct pmix_ring_buffer_t pmix_ring_buffer_t;
/**
 * Class declaration
 */
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_ring_buffer_t);

/* Static initializer, matching pmix_ring_buffer_construct() field for
 * field. Note that tail is -1, not 0: -1 is how this class spells "nothing
 * has been pushed yet", and a zero there would make pop() return addr[0]
 * off a NULL ring. Unlike PMIX_LIST_STATIC_INIT this does produce a usable
 * (empty) object, but pmix_ring_buffer_init() is still required before any
 * push -- there is no storage until then. */
#define PMIX_RING_BUFFER_STATIC_INIT                \
{                                                   \
    .super = PMIX_OBJ_STATIC_INIT(pmix_object_t),   \
    .head = 0,                                      \
    .tail = -1,                                     \
    .size = 0,                                      \
    .addr = NULL                                    \
}

/**
 * Initialize the ring buffer, defining its size.
 *
 * @param ring Pointer to a ring buffer (IN/OUT)
 * @param size The number of elements in the ring (IN)
 *
 * @return PMIX_SUCCESS if all initializations were successful. Otherwise,
 *  the error indicate what went wrong in the function.
 */
PMIX_EXPORT int pmix_ring_buffer_init(pmix_ring_buffer_t *ring, int size);

/**
 * Push an item onto the ring buffer, displacing the oldest
 * item on the ring if the ring is full
 *
 * @param ring Pointer to ring (IN)
 * @param ptr Pointer value (IN)
 *
 * @return Pointer to displaced item, NULL if ring
 *         is not yet full
 *
 * NULL is also returned, and nothing is stored, if the ring has no
 * storage -- it was never pmix_ring_buffer_init()'ed, or it has been
 * destructed. The two cases are indistinguishable in the return value;
 * this signature has nowhere else to put the distinction.
 */
PMIX_EXPORT void *pmix_ring_buffer_push(pmix_ring_buffer_t *ring, void *ptr);

/**
 * Pop an item off of the ring. The oldest entry on the ring will be
 * returned. If nothing on the ring, NULL is returned.
 *
 * @param ring          Pointer to ring (IN)
 *
 * @return Error code.  NULL indicates an error.
 */

PMIX_EXPORT void *pmix_ring_buffer_pop(pmix_ring_buffer_t *ring);

/*
 * Access an element of the ring, without removing it, indexed
 * starting at the tail - a value of -1 will return the element
 * at the head of the ring
 */
PMIX_EXPORT void *pmix_ring_buffer_poke(pmix_ring_buffer_t *ring, int i);

END_C_DECLS

#endif /* PMIX_RING_BUFFER_H */
