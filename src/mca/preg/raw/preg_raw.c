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
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <ctype.h>

#include "include/pmix.h"
#include "pmix_common.h"

#include "src/mca/bfrops/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_printf.h"

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

    *output = strdup((const char *) regex->bytes);
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
