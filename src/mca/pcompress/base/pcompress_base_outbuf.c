/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_output.h"

#include "src/mca/pcompress/base/base.h"

/* The smallest buffer a decompressor starts with, so a tiny blob is not
 * grown a few bytes at a time. */
#define PMIX_COMPRESS_BASE_OUTBUF_FLOOR 4096

/* The first guess at the output size, as a multiple of the compressed
 * input.  It is only a starting point: the buffer doubles from here, so a
 * poor guess costs a few reallocs, never a wrong answer. */
#define PMIX_COMPRESS_BASE_OUTBUF_RATIO 4

bool pmix_compress_base_outbuf_start(pmix_compress_base_outbuf_t *buf, size_t inlen,
                                     uint32_t claim)
{
    size_t size;

    memset(buf, 0, sizeof(*buf));

    /* UINT32_MAX is the error sentinel the blob format reserves, and no
     * compressor emits it - each one declines an input that large. Refusing
     * it here also keeps claim + 1 from wrapping where size_t is 32 bits. */
    if (UINT32_MAX == claim) {
        return false;
    }

    /* One byte beyond the claim. A decoder that fills it has produced more
     * than the blob promised, which is how an overrun is caught without
     * asking the decoder to run past the end of its buffer. The string path
     * also needs that byte for its NUL. */
    buf->ceiling = (size_t) claim + 1;

    if (inlen >= buf->ceiling / PMIX_COMPRESS_BASE_OUTBUF_RATIO) {
        size = buf->ceiling;
    } else {
        size = inlen * PMIX_COMPRESS_BASE_OUTBUF_RATIO;
        if (size < PMIX_COMPRESS_BASE_OUTBUF_FLOOR) {
            size = PMIX_COMPRESS_BASE_OUTBUF_FLOOR;
        }
        if (size > buf->ceiling) {
            size = buf->ceiling;
        }
    }

    buf->bytes = (uint8_t *) malloc(size);
    if (NULL == buf->bytes) {
        return false;
    }
    buf->size = size;
    return true;
}

bool pmix_compress_base_outbuf_grow(pmix_compress_base_outbuf_t *buf)
{
    size_t size;
    uint8_t *tmp;

    if (buf->size >= buf->ceiling) {
        pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                            "DECOMPRESS: payload inflates beyond the %" PRIsize_t
                            " bytes its length prefix claims",
                            buf->ceiling - 1);
        return false;
    }
    if (buf->size > buf->ceiling / 2) {
        size = buf->ceiling;
    } else {
        size = buf->size * 2;
    }

    tmp = (uint8_t *) realloc(buf->bytes, size);
    if (NULL == tmp) {
        /* the existing buffer is still valid and still owned by buf */
        return false;
    }
    buf->bytes = tmp;
    buf->size = size;
    return true;
}

bool pmix_compress_base_outbuf_finish(pmix_compress_base_outbuf_t *buf, size_t produced,
                                      uint32_t claim, uint8_t **outbytes)
{
    uint8_t *tmp;

    *outbytes = NULL;

    if (produced != (size_t) claim) {
        pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                            "DECOMPRESS: payload inflated to %" PRIsize_t
                            " bytes but its length prefix claims %u",
                            produced, claim);
        return false;
    }

    /* the decoder may have finished in a buffer exactly the claim's size,
     * leaving no room for the terminator */
    if (buf->size < buf->ceiling) {
        tmp = (uint8_t *) realloc(buf->bytes, buf->ceiling);
        if (NULL == tmp) {
            return false;
        }
        buf->bytes = tmp;
        buf->size = buf->ceiling;
    }
    buf->bytes[claim] = '\0';

    *outbytes = buf->bytes;
    buf->bytes = NULL;
    buf->size = 0;
    return true;
}

void pmix_compress_base_outbuf_release(pmix_compress_base_outbuf_t *buf)
{
    if (NULL != buf->bytes) {
        free(buf->bytes);
    }
    buf->bytes = NULL;
    buf->size = 0;
}
