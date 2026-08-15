/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Non-functional "test build" shim for the subset of the lz4 frame API
 * used by this component. It is included in place of the real
 * <lz4frame.h> only when the library is configured with
 * --enable-test-build (PMIX_TESTBUILD) so that the component can be
 * compile-checked on a machine that lacks the lz4 development headers.
 * The stubs return placeholder values and do NOT perform any compression
 * - a component built against this shim is not functional.
 */

#ifndef PMIX_PCOMPRESS_LZ4_TESTBUILD_H
#define PMIX_PCOMPRESS_LZ4_TESTBUILD_H

#include "pmix_config.h"

#include <stddef.h>
#include <stdint.h>

#define LZ4F_VERSION 100

typedef struct {
    unsigned long long contentSize;
} LZ4F_frameInfo_t;

typedef struct {
    LZ4F_frameInfo_t frameInfo;
    int compressionLevel;
} LZ4F_preferences_t;

typedef struct LZ4F_dctx_s LZ4F_dctx;

static inline unsigned LZ4F_isError(size_t code)
{
    (void) code;
    return 0;
}

static inline size_t LZ4F_compressFrameBound(size_t srcSize, const LZ4F_preferences_t *prefs)
{
    (void) prefs;
    return srcSize;
}

static inline size_t LZ4F_compressFrame(void *dst, size_t dstCapacity, const void *src,
                                        size_t srcSize, const LZ4F_preferences_t *prefs)
{
    (void) dst;
    (void) dstCapacity;
    (void) src;
    (void) prefs;
    return srcSize;
}

static inline size_t LZ4F_createDecompressionContext(LZ4F_dctx **dctx, unsigned version)
{
    (void) version;
    *dctx = NULL;
    return 0;
}

static inline size_t LZ4F_freeDecompressionContext(LZ4F_dctx *dctx)
{
    (void) dctx;
    return 0;
}

static inline size_t LZ4F_decompress(LZ4F_dctx *dctx, void *dst, size_t *dstSize,
                                     const void *src, size_t *srcSize, const void *dOptPtr)
{
    (void) dctx;
    (void) dst;
    (void) src;
    (void) dOptPtr;
    (void) dstSize;
    (void) srcSize;
    return 0;
}

#endif /* PMIX_PCOMPRESS_LZ4_TESTBUILD_H */
