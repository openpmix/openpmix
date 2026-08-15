/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <string.h>

#if PMIX_TESTBUILD
#    include "testbuild_lz4.h"
#else
#    include <lz4frame.h>
#endif

#include "src/include/pmix_stdint.h"
#include "src/util/pmix_output.h"

#include "pmix_common.h"

#include "src/mca/pcompress/base/base.h"

#include "compress_lz4.h"

static bool lz4_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen);

static bool lz4_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen);

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes);

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len);

static size_t get_decompressed_size(const pmix_byte_object_t *bo);

static size_t get_decompressed_strlen(const pmix_byte_object_t *bo);

pmix_compress_base_module_t pmix_pcompress_lz4_module = {
    .compress = lz4_compress,
    .decompress = lz4_decompress,
    .get_decompressed_size = get_decompressed_size,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
    .get_decompressed_strlen = get_decompressed_strlen,
};

/* The LZ4 frame magic, as it appears on the wire: 0x184D2204 is defined to be
 * serialized little-endian, so the first four bytes of any frame are this
 * sequence regardless of host byte order.
 *
 * Same reasoning as zstd's equivalent, and the same limitation.  The
 * framework's blob format puts a 4-byte raw length in front of a payload it
 * does not describe, so a blob this component produced is unreadable by a peer
 * whose PMIx selected any of the other three.  Sniffing the magic cannot make
 * that work; it turns "silently inflate garbage" into a clean refusal.  See
 * ../AGENTS.md - every node in a job must run the same component.
 *
 * This is also why the component uses the FRAME api rather than the raw block
 * api.  The block format has no header at all, so a DEFLATE blob handed to it
 * would be decoded as far as it happened to parse; a frame refuses at the
 * first byte. */
static const uint8_t lz4_magic[4] = {0x04, 0x22, 0x4D, 0x18};

static bool is_lz4_frame(const uint8_t *p, size_t len)
{
    return (sizeof(lz4_magic) <= len) && (0 == memcmp(p, lz4_magic, sizeof(lz4_magic)));
}

static bool lz4_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen)
{
    LZ4F_preferences_t prefs;
    size_t bound, produced, total;
    uint8_t *tmp, *ptr;
    uint32_t rawlen;

    /* set default output */
    *outbytes = NULL;
    *outlen = 0;

    /* the same two economy rules every component in this framework applies:
     * too small to be worth it, or too big for the 4-byte length prefix */
    if (inlen < pmix_compress_base.compress_limit || inlen >= UINT32_MAX) {
        return false;
    }
    rawlen = inlen;

    memset(&prefs, 0, sizeof(prefs));
    prefs.compressionLevel = pmix_pcompress_lz4_level;
    /* Tell the frame header how much is coming.  It costs 8 bytes and lets a
     * reader size its output buffer from the frame alone - which this
     * component does not need, having the prefix, but a third party reading a
     * captured blob does. */
    prefs.frameInfo.contentSize = inlen;

    /* an upper bound on the compressed size, so the compress call cannot
     * fail for want of room */
    bound = LZ4F_compressFrameBound(inlen, &prefs);
    if (LZ4F_isError(bound)) {
        return false;
    }
    /* allocate the 4-byte uncompressed-length prefix up front, so the payload
     * lands in its final position and no second copy is needed */
    if (NULL == (tmp = (uint8_t *) malloc(bound + sizeof(uint32_t)))) {
        return false;
    }
    memcpy(tmp, &rawlen, sizeof(uint32_t));

    produced = LZ4F_compressFrame(tmp + sizeof(uint32_t), bound, inbytes, inlen, &prefs);
    if (LZ4F_isError(produced)) {
        free(tmp);
        return false;
    }
    total = produced + sizeof(uint32_t);

    /* if this isn't going to result in a smaller footprint, then don't do it.
     * A caller can therefore always read a true return as "space was saved". */
    if (total >= inlen) {
        free(tmp);
        return false;
    }

    /* hand back a buffer sized to what was actually produced rather than to
     * the bound, which for an incompressible input is larger than the input */
    ptr = (uint8_t *) realloc(tmp, total);
    if (NULL == ptr) {
        /* the oversized buffer is still perfectly valid; only the trim failed */
        ptr = tmp;
    }
    *outbytes = ptr;
    *outlen = total;

    pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                        "COMPRESS INPUT BLOCK OF LEN %" PRIsize_t " OUTPUT SIZE %" PRIsize_t "",
                        inlen, produced);
    return true; // we did the compression
}

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes)
{
    /* the stored length is the string's length WITHOUT its NUL, matching the
     * other components; decompress_string adds the terminator back */
    return lz4_compress((uint8_t *) instring, strlen(instring), outbytes, nbytes);
}

/* Inflate `inlen` bytes of frame into a freshly allocated buffer.
 *
 * `capacity` and `expected` are separate on purpose and are NOT always equal:
 * the string path allocates one byte more than the blob's stored length so it
 * has room to append the NUL that length deliberately does not count.  Folding
 * them into one argument makes the strict length check below reject every
 * string it ever compressed. */
static bool doit(uint8_t **outbytes, size_t capacity, size_t expected,
                 const uint8_t *inbytes, size_t inlen)
{
    LZ4F_dctx *dctx = NULL;
    uint8_t *dest;
    size_t rc, dstsize, srcsize;

    /* set the default error answer */
    *outbytes = NULL;

    if (!is_lz4_frame(inbytes, inlen)) {
        /* Not ours.  Produced by a peer whose PMIx selected a different
         * pcompress component - which this one cannot read, and must not
         * pretend to. */
        pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                            "DECOMPRESS: blob is not an lz4 frame - it was produced "
                            "by a peer using a different pcompress component");
        return false;
    }

    dest = (uint8_t *) malloc(capacity);
    if (NULL == dest) {
        return false;
    }

    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) {
        free(dest);
        return false;
    }

    /* The whole frame is in hand and the destination is big enough for all of
     * it, so this decodes in one call.  A non-zero return means the decoder
     * wants more input, which for a complete frame means the blob is
     * truncated - refuse it rather than hand back a partial answer. */
    dstsize = capacity;
    srcsize = inlen;
    rc = LZ4F_decompress(dctx, dest, &dstsize, inbytes, &srcsize, NULL);
    LZ4F_freeDecompressionContext(dctx);

    if (LZ4F_isError(rc) || 0 != rc || dstsize != expected || srcsize != inlen) {
        free(dest);
        return false;
    }

    *outbytes = dest;
    return true;
}

static bool lz4_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen)
{
    uint32_t len2;

    /* set the default error answer */
    *outlen = 0;

    if (NULL == inbytes || inlen < sizeof(uint32_t)) {
        return false;
    }

    /* the first 4 bytes contains the uncompressed size */
    memcpy(&len2, inbytes, sizeof(uint32_t));

    pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                        "DECOMPRESSING INPUT OF LEN %" PRIsize_t " OUTPUT %u", inlen, len2);

    if (!doit(outbytes, len2, len2, inbytes + sizeof(uint32_t), inlen - sizeof(uint32_t))) {
        return false;
    }
    *outlen = len2;
    return true;
}

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len)
{
    uint32_t len2;

    /* set the default error answer */
    *outstring = NULL;

    if (NULL == inbytes || len < sizeof(uint32_t)) {
        return false;
    }

    /* the first 4 bytes contains the uncompressed size */
    memcpy(&len2, inbytes, sizeof(uint32_t));
    if (UINT32_MAX == len2) {
        /* the error sentinel the format reserves */
        return false;
    }

    /* inflate into exactly the promised length, then add the terminator the
     * stored length deliberately does not count */
    if (!doit((uint8_t **) outstring, len2 + 1, len2, inbytes + sizeof(uint32_t),
              len - sizeof(uint32_t))) {
        return false;
    }
    (*outstring)[len2] = '\0';
    return true;
}

/* A blob produced by lz4_compress / compress_string carries the uncompressed
 * length in its leading 4 bytes (see the shared blob format). These helpers
 * return that length by reading the prefix, without inflating the payload, so
 * callers computing the in-memory footprint of a PMIX_COMPRESSED_BYTE_OBJECT /
 * PMIX_COMPRESSED_STRING need not decompress it first. */
static size_t get_decompressed_size(const pmix_byte_object_t *bo)
{
    uint32_t len = 0;

    if (NULL == bo || NULL == bo->bytes || bo->size < sizeof(uint32_t)) {
        return 0;
    }
    /* the first 4 bytes contain the uncompressed size */
    memcpy(&len, bo->bytes, sizeof(uint32_t));
    return (size_t) len;
}

static size_t get_decompressed_strlen(const pmix_byte_object_t *bo)
{
    uint32_t len = 0;

    if (NULL == bo || NULL == bo->bytes || bo->size < sizeof(uint32_t)) {
        return 0;
    }
    memcpy(&len, bo->bytes, sizeof(uint32_t));
    /* +1 for the NUL terminator, mirroring how a plain PMIX_STRING is sized */
    return (size_t) len + 1;
}
