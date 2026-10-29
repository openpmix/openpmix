/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS). All Rights Reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_FNV_H
#define PMIX_UTIL_FNV_H

#include "src/include/pmix_config.h"

#include <stddef.h>
#include <stdint.h>

/* 64-bit FNV-1a. Start from PMIX_FNV1A64_INIT and feed the data in as
 * many pieces as convenient. */
#define PMIX_FNV1A64_INIT 14695981039346656037ULL

static inline uint64_t pmix_fnv1a64(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *) data;
    size_t n;

    for (n = 0; n < len; n++) {
        h ^= (uint64_t) p[n];
        h *= 1099511628211ULL;
    }
    return h;
}

#endif
