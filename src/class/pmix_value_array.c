/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/class/pmix_value_array.h"

static void pmix_value_array_construct(pmix_value_array_t *array)
{
    array->array_items = NULL;
    array->array_size = 0;
    array->array_item_sizeof = 0;
    array->array_alloc_size = 0;
}

static void pmix_value_array_destruct(pmix_value_array_t *array)
{
    if (NULL != array->array_items) {
        free(array->array_items);
    }
    /* Return to the constructed state so a second PMIX_DESTRUCT (or a
     * destruct followed by a re-init) cannot free the same buffer twice */
    array->array_items = NULL;
    array->array_size = 0;
    array->array_alloc_size = 0;
}

PMIX_CLASS_INSTANCE(pmix_value_array_t, pmix_object_t, pmix_value_array_construct,
                    pmix_value_array_destruct);

int pmix_value_array_set_size(pmix_value_array_t *array, size_t size)
{
    size_t new_alloc;
    unsigned char *grown;

    /* The item size is what every offset in here is computed from, so a
     * zero one is fatal, not a debug-only curiosity. It used to be checked
     * only under --enable-debug, which left an optimized build computing
     * every address as items + index*0. */
    if (PMIX_UNLIKELY(0 == array->array_item_sizeof)) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (size > array->array_alloc_size) {
        /* Seed the doubling. array_alloc_size is 0 for an array that was
         * only PMIX_CONSTRUCT'd (or PMIX_VALUE_ARRAY_STATIC_INIT'd, or
         * whose pmix_value_array_reserve() ran out of memory), and
         * "0 <<= 1" is still 0 - so the old loop spun forever. */
        new_alloc = (0 == array->array_alloc_size) ? 1 : array->array_alloc_size;
        while (new_alloc < size) {
            new_alloc <<= 1;
        }
        /* Keep the old buffer until the new one is in hand: assigning the
         * result of a failed realloc() straight back into array_items both
         * leaks the old allocation and leaves the array claiming a size it
         * no longer has storage for. */
        grown = (unsigned char *) realloc(array->array_items,
                                          new_alloc * array->array_item_sizeof);
        if (NULL == grown) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        array->array_items = grown;
        array->array_alloc_size = new_alloc;
    }
    array->array_size = size;
    return PMIX_SUCCESS;
}
