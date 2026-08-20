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
#    include "testbuild_zstd.h"
#else
#    include <zstd.h>
#endif

#include "src/include/pmix_stdint.h"
#include "src/util/pmix_output.h"

#include "pmix_common.h"

#include "src/mca/pcompress/base/base.h"

#include "compress_zstd.h"

static bool zstd_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen);

static bool zstd_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen);

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes);

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len);

static size_t get_decompressed_size(const pmix_byte_object_t *bo);

static size_t get_decompressed_strlen(const pmix_byte_object_t *bo);

pmix_compress_base_module_t pmix_pcompress_zstd_module = {
    .compress = zstd_compress,
    .decompress = zstd_decompress,
    .get_decompressed_size = get_decompressed_size,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
    .get_decompressed_strlen = get_decompressed_strlen,
};

/* The zstd frame magic, as it appears on the wire: ZSTD_MAGICNUMBER
 * (0xFD2FB528) is defined to be serialized little-endian, so the first four
 * bytes of any frame are this sequence regardless of host byte order.
 *
 * This is load-bearing rather than decorative.  The framework's blob format
 * (see ../AGENTS.md) puts a 4-byte raw length in front of the compressed
 * payload and says nothing about what the payload is, because until now every
 * component emitted DEFLATE and the two were interchangeable.  A zstd frame is
 * not DEFLATE, so a blob this component produced is unreadable by a zlib-only
 * peer.  Sniffing the magic on the way in does not fix that - nothing here
 * can - but it turns "silently inflate garbage" into a clean refusal, which is
 * the difference between a diagnosable configuration error and a corrupted
 * modex. */
static const uint8_t zstd_magic[4] = {0x28, 0xB5, 0x2F, 0xFD};

static bool is_zstd_frame(const uint8_t *p, size_t len)
{
    return (sizeof(zstd_magic) <= len) && (0 == memcmp(p, zstd_magic, sizeof(zstd_magic)));
}

static bool zstd_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen)
{
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

    /* an upper bound on the compressed size, so the compress call cannot
     * fail for want of room */
    bound = ZSTD_compressBound(inlen);
    if (ZSTD_isError(bound)) {
        return false;
    }
    /* allocate the 4-byte uncompressed-length prefix up front, so the payload
     * lands in its final position and no second copy is needed */
    if (NULL == (tmp = (uint8_t *) malloc(bound + sizeof(uint32_t)))) {
        return false;
    }
    memcpy(tmp, &rawlen, sizeof(uint32_t));

    produced = ZSTD_compress(tmp + sizeof(uint32_t), bound, inbytes, inlen,
                             pmix_pcompress_zstd_level);
    if (ZSTD_isError(produced)) {
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
    return zstd_compress((uint8_t *) instring, strlen(instring), outbytes, nbytes);
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
    uint8_t *dest;
    size_t produced;

    /* set the default error answer */
    *outbytes = NULL;

    if (!is_zstd_frame(inbytes, inlen)) {
        /* Not ours.  Almost certainly a DEFLATE blob from a peer whose PMIx
         * selected zlib or zlib-ng - which this component cannot read, and
         * must not pretend to. */
        pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                            "DECOMPRESS: blob is not a zstd frame - it was produced "
                            "by a peer using a different pcompress component");
        return false;
    }

    dest = (uint8_t *) malloc(capacity);
    if (NULL == dest) {
        return false;
    }

    produced = ZSTD_decompress(dest, capacity, inbytes, inlen);
    if (ZSTD_isError(produced) || produced != expected) {
        free(dest);
        return false;
    }

    *outbytes = dest;
    return true;
}

static bool zstd_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen)
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

/* A blob produced by zstd_compress / compress_string carries the uncompressed
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
    size_t len;

    /* 0 is this entry point's "I cannot answer" sentinel - it is what the
     * base default returns for a blob it cannot read, and what bfrops'
     * decompressed_strlen() falls back to for a module that does not
     * implement the slot at all. A stored length of zero is not a real
     * compressed string (compress declines anything below compress_limit),
     * so returning 0 + 1 here would report a one-byte string where the
     * other components report "unknown". Answer as they do. */
    len = get_decompressed_size(bo);
    if (0 == len) {
        return 0;
    }
    /* +1 for the NUL terminator, mirroring how a plain PMIX_STRING is sized */
    return len + 1;
}
