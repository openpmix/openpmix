/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Non-functional "test build" shim for the subset of the zstd API used
 * by this component. It is included in place of the real <zstd.h> only
 * when the library is configured with --enable-test-build (PMIX_TESTBUILD)
 * so that the component can be compile-checked on a machine that lacks
 * the zstd development headers. The stubs return placeholder values and
 * do NOT perform any compression - a component built against this shim is
 * not functional.
 */

#ifndef PMIX_PCOMPRESS_ZSTD_TESTBUILD_H
#define PMIX_PCOMPRESS_ZSTD_TESTBUILD_H

#include "pmix_config.h"

#include <stddef.h>
#include <stdint.h>

static inline size_t ZSTD_compressBound(size_t srcSize)
{
    return srcSize;
}

static inline unsigned ZSTD_isError(size_t code)
{
    (void) code;
    return 0;
}

static inline size_t ZSTD_compress(void *dst, size_t dstCapacity, const void *src,
                                   size_t srcSize, int level)
{
    (void) dst;
    (void) dstCapacity;
    (void) src;
    (void) level;
    return srcSize;
}

static inline size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src,
                                     size_t compressedSize)
{
    (void) dst;
    (void) src;
    (void) compressedSize;
    return dstCapacity;
}

typedef struct ZSTD_DCtx_s ZSTD_DCtx;

typedef struct {
    const void *src;
    size_t size;
    size_t pos;
} ZSTD_inBuffer;

typedef struct {
    void *dst;
    size_t size;
    size_t pos;
} ZSTD_outBuffer;

static inline ZSTD_DCtx *ZSTD_createDCtx(void)
{
    return NULL;
}

static inline size_t ZSTD_freeDCtx(ZSTD_DCtx *dctx)
{
    (void) dctx;
    return 0;
}

static inline size_t ZSTD_decompressStream(ZSTD_DCtx *dctx, ZSTD_outBuffer *output,
                                           ZSTD_inBuffer *input)
{
    (void) dctx;
    (void) output;
    (void) input;
    return 0;
}

#endif /* PMIX_PCOMPRESS_ZSTD_TESTBUILD_H */
