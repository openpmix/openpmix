/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Non-functional "test build" shim for the subset of the zlib API used
 * by this component. It is included in place of the real <zlib.h> only
 * when the library is configured with --enable-test-build (PMIX_TESTBUILD)
 * so that the component can be compile-checked on a machine that lacks
 * the zlib development headers. The stubs return placeholder values and
 * do NOT perform any compression - a component built against this shim is
 * not functional.
 */

#ifndef PMIX_PCOMPRESS_ZLIB_TESTBUILD_H
#define PMIX_PCOMPRESS_ZLIB_TESTBUILD_H

#include "pmix_config.h"

#include <stdint.h>

#define Z_OK         0
#define Z_STREAM_END 1
#define Z_FINISH     4

typedef struct {
    uint8_t *next_in;
    unsigned int avail_in;
    uint8_t *next_out;
    unsigned int avail_out;
} z_stream;

static inline int deflateInit(z_stream *strm, int level)
{
    (void) strm;
    (void) level;
    return Z_OK;
}

static inline unsigned long deflateBound(z_stream *strm, unsigned long sourceLen)
{
    (void) strm;
    return sourceLen;
}

static inline int deflate(z_stream *strm, int flush)
{
    (void) strm;
    (void) flush;
    return Z_STREAM_END;
}

static inline int deflateEnd(z_stream *strm)
{
    (void) strm;
    return Z_OK;
}

static inline int inflateInit(z_stream *strm)
{
    (void) strm;
    return Z_OK;
}

static inline int inflate(z_stream *strm, int flush)
{
    (void) strm;
    (void) flush;
    return Z_STREAM_END;
}

static inline int inflateEnd(z_stream *strm)
{
    (void) strm;
    return Z_OK;
}

#endif /* PMIX_PCOMPRESS_ZLIB_TESTBUILD_H */
