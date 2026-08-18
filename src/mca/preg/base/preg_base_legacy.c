/*
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Serialization of a pmix_regex2_t into the deprecated char* form, and
 * back again.
 *
 * PMIx_generate_regex and PMIx_generate_ppn hand back a char* rather
 * than a pmix_regex2_t. Both of those forms have always been the same
 * thing underneath - a type tag, a length, and a payload - so the
 * deprecated interface is a serialization of the current one rather than
 * a separate encoding. Keeping the two layouts here means a component
 * only ever implements generate_regex/parse_regex, and nothing outside
 * this file has to know that the deprecated form exists.
 *
 * The layouts below are FROZEN. They are what a PMIx of any vintage puts
 * on the wire for a PMIX_REGEX value, so they may not be reformatted,
 * padded, or retagged:
 *
 *   compress:  "blob:" NUL "component=zlib:" NUL "size=" <decimal> ":" NUL <payload>
 *   raw:       "raw:" <NUL-terminated string>
 *
 * Note that "component=zlib:" is a fixed label, not a statement about
 * which pcompress component actually ran - it was already being emitted
 * unconditionally when zlib was the only compressor PMIx had, and by the
 * time others were added the string was on the wire and could not be
 * changed. It carries no meaning beyond "a preg component produced this",
 * and nothing selects a decompressor from it; pcompress identifies its
 * own payloads. Do not read it as an abstraction boundary.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/include/pmix_globals.h"
#include "src/mca/preg/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_printf.h"

/* the compress layout, with each NUL written as a space so that the
 * macro's length matches the length of the bytes it stands for */
#define PREG_LEGACY_BLOB_PREFIX "blob: component=zlib: size="
#define PREG_LEGACY_BLOB_TAG    "blob"
#define PREG_LEGACY_BLOB_LABEL  "component=zlib:"
#define PREG_LEGACY_RAW_TAG     "raw:"

/* the type names of the components whose output we know how to
 * serialize. A component that is not named here simply never appears in
 * the deprecated form - the caller falls back to shipping the input
 * uncompressed, exactly as it did when no component claimed the request. */
#define PREG_LEGACY_BLOB_TYPE   "compress"
#define PREG_LEGACY_RAW_TYPE    "raw"

pmix_status_t pmix_preg_base_legacy_encode(const pmix_regex2_t *regex, char **output,
                                           size_t *outlen)
{
    char *result, *slen;
    size_t total;
    int idx;

    if (NULL == regex || NULL == regex->type || NULL == regex->bytes) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (0 == strcmp(regex->type, PREG_LEGACY_RAW_TYPE)) {
        /* the payload is the NULL-terminated list itself */
        if (0 > pmix_asprintf(&result, "%s%s", PREG_LEGACY_RAW_TAG, (const char *) regex->bytes)) {
            return PMIX_ERR_NOMEM;
        }
        *output = result;
        *outlen = strlen(result) + 1;
        return PMIX_SUCCESS;
    }

    if (0 != strcmp(regex->type, PREG_LEGACY_BLOB_TYPE)) {
        /* we have no serialization for this encoding */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* convert the payload length to a string */
    if (0 > pmix_asprintf(&slen, "%lu", (unsigned long) regex->len)) {
        return PMIX_ERR_NOMEM;
    }

    total = regex->len + strlen(PREG_LEGACY_BLOB_PREFIX) + strlen(slen) + strlen(":") + 1;
    result = calloc(total, sizeof(char));
    if (NULL == result) {
        free(slen);
        return PMIX_ERR_NOMEM;
    }

    idx = 0;
    strcpy(result, PREG_LEGACY_BLOB_TAG ":");
    idx += strlen(PREG_LEGACY_BLOB_TAG ":") + 1; // step over NULL terminator
    strcpy(&result[idx], PREG_LEGACY_BLOB_LABEL);
    idx += strlen(PREG_LEGACY_BLOB_LABEL) + 1; // step over NULL terminator
    strcpy(&result[idx], "size=");
    idx += strlen("size=");
    strcpy(&result[idx], slen);
    idx += strlen(slen);
    strcpy(&result[idx], ":");
    idx += strlen(":") + 1; // step over NULL terminator
    memcpy(&result[idx], regex->bytes, regex->len);
    free(slen);

    *output = result;
    *outlen = total;
    return PMIX_SUCCESS;
}

/* Decode in place: the returned pmix_regex2_t points into the caller's
 * buffer and must not be destructed. avail bounds the read - pass
 * SIZE_MAX for a NULL-terminated string the caller owns, or the number
 * of bytes remaining when reading off the wire. */
pmix_status_t pmix_preg_base_legacy_decode(const char *input, size_t avail,
                                           pmix_regex2_t *regex, size_t *total)
{
    size_t idx, len, taglen;
    char *ptr;

    if (NULL == input || 0 == avail) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* every layout begins with a NULL-terminated tag, so refuse anything
     * that does not carry one within reach */
    taglen = strnlen(input, avail);
    if (taglen == avail) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    if (0 == strncmp(input, PREG_LEGACY_RAW_TAG, strlen(PREG_LEGACY_RAW_TAG))) {
        regex->type = (char *) PREG_LEGACY_RAW_TYPE;
        regex->bytes = (uint8_t *) &input[strlen(PREG_LEGACY_RAW_TAG)];
        regex->len = taglen - strlen(PREG_LEGACY_RAW_TAG) + 1; // retain the NULL terminator
        *total = taglen + 1;
        return PMIX_SUCCESS;
    }

    if (0 != strncmp(input, PREG_LEGACY_BLOB_TAG, strlen(PREG_LEGACY_BLOB_TAG))) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    idx = taglen + 1; // step over the NULL terminator

    /* the label is fixed, so this only confirms that a preg component
     * wrote the blob - see the note at the top of this file */
    if (idx + strlen(PREG_LEGACY_BLOB_LABEL) >= avail ||
        0 != strncmp(&input[idx], PREG_LEGACY_BLOB_LABEL, strlen(PREG_LEGACY_BLOB_LABEL))) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    idx += strlen(PREG_LEGACY_BLOB_LABEL) + 1; // step over the NULL terminator

    idx += strlen("size=");
    /* the length is decimal digits followed by a colon and a NULL, so
     * refuse to run strtoul off the end of a truncated buffer */
    if (idx >= avail || strnlen(&input[idx], avail - idx) == avail - idx) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    len = strtoul(&input[idx], &ptr, 10);
    if (ptr == &input[idx]) {
        /* no digits where the length belongs */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    ptr += 2; // step over the colon and its NULL terminator

    if ((size_t) (ptr - input) + len > avail) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    regex->type = (char *) PREG_LEGACY_BLOB_TYPE;
    regex->bytes = (uint8_t *) ptr;
    regex->len = len;
    *total = (size_t) (ptr - input) + len;
    return PMIX_SUCCESS;
}
