/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif

#include "pmix_common.h"

#include "src/include/pmix_globals.h"

#include "preg_compress.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/mca/preg/base/base.h"

static pmix_status_t generate_regex(const char *input, pmix_info_t info[], size_t ninfo,
                                    pmix_regex2_t *regex);
static pmix_status_t parse_regex(const pmix_regex2_t *regex, pmix_info_t info[], size_t ninfo,
                                 char **output);

pmix_preg_module_t pmix_preg_compress_module = {
    .name = "compress",
    .generate_regex = generate_regex,
    .parse_regex = parse_regex
};

static pmix_status_t parse_regex(const pmix_regex2_t *regex, pmix_info_t info[], size_t ninfo,
                                 char **output)
{
    char *tmp = NULL;

    // no attributes are currently defined for this function
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    if (NULL == regex || NULL == regex->type ||
        0 != strcmp(regex->type, pmix_preg_compress_module.name)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* a peer is free to declare a length of zero, in which case bfrops
     * unpacked no bytes and left the pointer NULL. Screen that here: what
     * is on the other side of this call is a decompressor that reads the
     * blob's length prefix out of the first four bytes, and not all of
     * them check first */
    if (NULL == regex->bytes || 0 == regex->len) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (!pmix_compress.decompress_string(&tmp, regex->bytes, regex->len)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    *output = tmp;
    return PMIX_SUCCESS;
}

static pmix_status_t generate_regex(const char *input, pmix_info_t info[], size_t ninfo,
                                    pmix_regex2_t *regex)
{
    uint8_t *compressed = NULL;
    size_t len;

    // no attributes are currently defined for this function
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    if (!pmix_compress.compress_string((char *) input, &compressed, &len)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL == compressed) {
        return PMIX_ERR_NOMEM;
    }

    regex->type = strdup(pmix_preg_compress_module.name);
    if (NULL == regex->type) {
        free(compressed);
        return PMIX_ERR_NOMEM;
    }
    regex->bytes = compressed;
    regex->len = len;
    return PMIX_SUCCESS;
}

