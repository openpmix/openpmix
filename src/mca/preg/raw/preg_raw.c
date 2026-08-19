/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#include "preg_raw.h"
#include "src/mca/preg/base/base.h"

static pmix_status_t generate_regex(const char *input, pmix_info_t info[], size_t ninfo,
                                    pmix_regex2_t *regex);
static pmix_status_t parse_regex(const pmix_regex2_t *regex, pmix_info_t info[], size_t ninfo,
                                 char **output);

pmix_preg_module_t pmix_preg_raw_module = {
    .name = "raw",
    .generate_regex = generate_regex,
    .parse_regex = parse_regex
};

static pmix_status_t parse_regex(const pmix_regex2_t *regex, pmix_info_t info[], size_t ninfo,
                                 char **output)
{
    // no attributes are currently defined for this function
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    if (NULL == regex || NULL == regex->type ||
        0 != strcmp(regex->type, pmix_preg_raw_module.name)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* the bytes came off a peer's wire, where a pmix_regex2_t is a length
     * and that many bytes - nothing puts a NULL at the end of them. Our
     * generate counts the terminator into len, so a well-formed raw value
     * carries one; confirm it rather than let strdup go looking, which on
     * a value that does not is a read past the end of the unpacked
     * allocation. A zero length is the same question with no bytes at
     * all: bfrops leaves the pointer NULL when the peer declared none. */
    if (NULL == regex->bytes || 0 == regex->len ||
        '\0' != (char) regex->bytes[regex->len - 1]) {
        return PMIX_ERR_BAD_PARAM;
    }

    *output = strdup((const char *) regex->bytes);
    if (NULL == *output) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t generate_regex(const char *input, pmix_info_t info[], size_t ninfo,
                                    pmix_regex2_t *regex)
{
    // no attributes are currently defined for this function
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    regex->type = strdup(pmix_preg_raw_module.name);
    if (NULL == regex->type) {
        return PMIX_ERR_NOMEM;
    }
    regex->bytes = (uint8_t *) strdup(input);
    if (NULL == regex->bytes) {
        free(regex->type);
        regex->type = NULL;
        return PMIX_ERR_NOMEM;
    }
    regex->len = strlen(input) + 1;
    return PMIX_SUCCESS;
}
