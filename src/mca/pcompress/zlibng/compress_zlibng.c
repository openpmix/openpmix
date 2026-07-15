/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 *
 * Copyright (c) 2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#if HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#if PMIX_TESTBUILD
#    include "testbuild_zlibng.h"
#else
#    include <zlib-ng.h>
#endif

#include "src/include/pmix_stdint.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include "pmix_common.h"
#include "src/util/pmix_basename.h"

#include "src/mca/pcompress/base/base.h"

#include "compress_zlibng.h"

static bool zlibng_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen);

static bool zlibng_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen);

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes);

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len);

static size_t get_decompressed_size(const pmix_byte_object_t *bo);

static size_t get_decompressed_strlen(const pmix_byte_object_t *bo);

pmix_compress_base_module_t pmix_pcompress_zlibng_module = {
    .compress = zlibng_compress,
    .decompress = zlibng_decompress,
    .get_decompressed_size = get_decompressed_size,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
    .get_decompressed_strlen = get_decompressed_strlen,
};

static bool zlibng_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen)
{
    zng_stream strm;
    size_t len, len2;
    uint8_t *tmp, *ptr;
    uint32_t len3;
    int rc;

    /* set default output */
    *outbytes = NULL;
    *outlen = 0;

    if (inlen < pmix_compress_base.compress_limit || inlen >= UINT32_MAX) {
        return false;
    }
    len3 = inlen;

    /* setup the stream */
    memset(&strm, 0, sizeof(strm));
    if (Z_OK != zng_deflateInit(&strm, 9)) {
        return false;
    }

    /* get an upper bound on the required output storage */
    len = zng_deflateBound(&strm, inlen);
    // it's okay if this len > inlen because it includes
    // working space for the compression library
    if (NULL == (tmp = (uint8_t *) malloc(len))) {
        (void) zng_deflateEnd(&strm);
        return false;
    }
    strm.next_in = (uint8_t*)inbytes;
    strm.avail_in = inlen;

    /* allocating the upper bound guarantees zlibng will
     * always successfully compress into the available space */
    strm.avail_out = len;
    strm.next_out = tmp;

    rc = zng_deflate(&strm, Z_FINISH);
    (void) zng_deflateEnd(&strm);
    if (Z_STREAM_END != rc) {
        free(tmp);
        return false;
    }

    /* allocate 4 bytes beyond the size reqd by zlibng so we
     * can pass the size of the uncompressed block to the
     * decompress side */
    len2 = len - strm.avail_out + sizeof(uint32_t);
    /* if this isn't going to result in a smaller footprint,
     * then don't do it */
    if (len2 >= inlen) {
        free(tmp);
        return false;
    }

    ptr = (uint8_t *) malloc(len2);
    if (NULL == ptr) {
        free(tmp);
        return false;
    }
    *outbytes = ptr;
    *outlen = len2;

    /* fold the uncompressed length into the buffer */
    memcpy(ptr, &len3, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    /* bring over the compressed data */
    memcpy(ptr, tmp, len2 - sizeof(uint32_t));
    free(tmp);
    pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                        "COMPRESS INPUT BLOCK OF LEN %" PRIsize_t " OUTPUT SIZE %" PRIsize_t "",
                        inlen, len2 - sizeof(uint32_t));
    return true; // we did the compression
}

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes)
{
    uint32_t inlen;

    /* setup the stream */
    inlen = strlen(instring);

    /* compress the string */
    return zlibng_compress((uint8_t *) instring, inlen, outbytes, nbytes);
}

static bool doit(uint8_t **outbytes, size_t len2, const uint8_t *inbytes, size_t inlen)
{
    uint8_t *dest;
    zng_stream strm;
    int rc;

    /* set the default error answer */
    *outbytes = NULL;

    /* setting destination to the fully decompressed size */
    dest = (uint8_t *) malloc(len2);
    if (NULL == dest) {
        return false;
    }
    memset(dest, 0, len2);

    memset(&strm, 0, sizeof(strm));
    if (Z_OK != zng_inflateInit(&strm)) {
        free(dest);
        return false;
    }
    strm.avail_in = inlen;
    strm.next_in = (uint8_t*)inbytes;
    strm.avail_out = len2;
    strm.next_out = dest;

    rc = zng_inflate(&strm, Z_FINISH);
    zng_inflateEnd(&strm);
    if (Z_STREAM_END == rc) {
        *outbytes = dest;
        return true;
    }
    free(dest);
    return false;
}
static bool zlibng_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen)
{
    uint32_t len2, input_len;
    bool rc;
    uint8_t *input;

    /* set the default error answer */
    *outlen = 0;

    /* the first 4 bytes contains the uncompressed size */
    memcpy(&len2, inbytes, sizeof(uint32_t));

    pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                        "DECOMPRESSING INPUT OF LEN %" PRIsize_t " OUTPUT %u", inlen, len2);

    input = (uint8_t *) (inbytes + sizeof(uint32_t)); // step over the size
    input_len = inlen - sizeof(uint32_t);
    rc = doit(outbytes, len2, input, input_len);
    if (rc) {
        *outlen = len2;
        return true;
    }
    return false;
}

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len)
{
    uint32_t len2, input_len;
    bool rc;
    uint8_t *input;

    /* the first 4 bytes contains the uncompressed size */
    memcpy(&len2, inbytes, sizeof(uint32_t));
    if (len2 == UINT32_MAX) {
        /* set the default error answer */
        *outstring = NULL;
        return false;
    }
    /* add one to hold the NUL terminator */
    ++len2;

    /* decompress the bytes */
    input = (uint8_t *) (inbytes + sizeof(uint32_t)); // step over the size
    input_len = len - sizeof(uint32_t);
    rc = doit((uint8_t **) outstring, len2, input, input_len);

    if (rc) {
        /* ensure this is NUL terminated! */
        (*outstring)[len2 - 1] = '\0';
        return true;
    }

    /* set the default error answer */
    *outstring = NULL;
    return false;
}

/* A blob produced by zlibng_compress / compress_string carries the
 * uncompressed length in its leading 4 bytes (see the shared blob
 * format). These helpers return that length by reading the prefix,
 * without inflating the payload, so callers computing the in-memory
 * footprint of a PMIX_COMPRESSED_BYTE_OBJECT / PMIX_COMPRESSED_STRING
 * need not decompress it first. */
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

    /* a compressed string stores its strlen (without the NUL) in the
     * prefix; add one for the terminator so the reported size matches
     * the uncompressed PMIX_STRING accounting */
    len = get_decompressed_size(bo);
    if (0 == len) {
        return 0;
    }
    return len + 1;
}
