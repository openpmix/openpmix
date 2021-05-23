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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include <zlib.h>

#include "src/include/pmix_stdint.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/printf.h"

#include "include/pmix_common.h"
#include "src/util/basename.h"

#include "src/mca/pcompress/base/base.h"

#include "compress_zlib.h"

static bool zlib_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen);

static bool zlib_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen);

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes);

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len);

pmix_compress_base_module_t pmix_pcompress_zlib_module = {
    .compress = zlib_compress,
    .decompress = zlib_decompress,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
};

static bool zlib_compress(const uint8_t *inbytes, size_t inlen, uint8_t **outbytes, size_t *outlen)
{
    z_stream strm;
    size_t len, len2;
    uint8_t *tmp, *ptr;
    int rc;

    /* set default output */
    *outbytes = NULL;
    *outlen = 0;

    if (inlen < pmix_compress_base.compress_limit) {
        return false;
    }

    /* setup the stream */
    memset(&strm, 0, sizeof(strm));
    deflateInit(&strm, 9);

    /* get an upper bound on the required output storage */
    len = deflateBound(&strm, inlen);
    /* if this isn't going to result in a smaller footprint,
     * then don't do it */
    if (len >= inlen) {
        (void) deflateEnd(&strm);
        return false;
    }

    if (NULL == (tmp = (uint8_t *) malloc(len))) {
        (void) deflateEnd(&strm);
        return false;
    }
    strm.next_in = (uint8_t*)inbytes;
    strm.avail_in = inlen;

    /* allocating the upper bound guarantees zlib will
     * always successfully compress into the available space */
    strm.avail_out = len;
    strm.next_out = tmp;

    rc = deflate(&strm, Z_FINISH);
    (void) deflateEnd(&strm);
    if (Z_OK != rc && Z_STREAM_END != rc) {
        free(tmp);
        return false;
    }

    /* allocate 4 bytes beyond the size reqd by zlib so we
     * can pass the size of the uncompressed block to the
     * decompress side */
    len2 = len - strm.avail_out + sizeof(uint32_t);
    ptr = (uint8_t *) malloc(len2);
    if (NULL == ptr) {
        free(tmp);
        return false;
    }
    *outbytes = ptr;
    *outlen = len2;

    /* fold the uncompressed length into the buffer */
    memcpy(ptr, &inlen, sizeof(uint32_t));
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
    return zlib_compress((uint8_t *) instring, inlen, outbytes, nbytes);
}

static bool doit(uint8_t **outbytes, size_t len2, const uint8_t *inbytes, size_t inlen)
{
    uint8_t *dest;
    z_stream strm;
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
    if (Z_OK != inflateInit(&strm)) {
        free(dest);
        return false;
    }
    strm.avail_in = inlen;
    strm.next_in = (uint8_t*)inbytes;
    strm.avail_out = len2;
    strm.next_out = dest;

    rc = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (Z_OK == rc) {
        *outbytes = dest;
        return true;
    }
    free(dest);
    return false;
}
static bool zlib_decompress(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t inlen)
{
    int32_t len2;
    bool rc;
    uint8_t *input;

    /* set the default error answer */
    *outlen = 0;

    /* the first 4 bytes contains the uncompressed size */
    memcpy(&len2, inbytes, sizeof(uint32_t));

    pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                        "DECOMPRESSING INPUT OF LEN %" PRIsize_t " OUTPUT %d", inlen, len2);

    input = (uint8_t *) (inbytes + sizeof(uint32_t)); // step over the size
    rc = doit(outbytes, len2, input, inlen);
    if (rc) {
        *outlen = len2;
        return true;
    }
    return false;
}

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len)
{
    int32_t len2;
    bool rc;
    uint8_t *input;

    /* the first 4 bytes contains the uncompressed size */
    memcpy(&len2, inbytes, sizeof(uint32_t));
    /* add one to hold the NULL terminator */
    ++len2;

    /* decompress the bytes */
    input = (uint8_t *) (inbytes + sizeof(uint32_t)); // step over the size
    rc = doit((uint8_t **) outstring, len2, input, len);

    if (rc) {
        /* ensure this is NULL terminated! */
        outstring[len2 - 1] = NULL;
        return true;
    }

    /* set the default error answer */
    *outstring = NULL;
    return false;
}
