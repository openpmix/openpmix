/*
 * This file is derived from the sse package - per the BSD license, the following
 * info is copied here:
 ***********************
 * This file is part of the sse package, copyright (c) 2011, 2012, @radiospiel.
 * It is copyrighted under the terms of the modified BSD license, see LICENSE.BSD.
 *
 * For more information see https://https://github.com/radiospiel/sse.
 ***********************
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_SSE_INTERNAL_H
#define PMIX_SSE_INTERNAL_H

#include "src/include/pmix_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PMIX_COMMON_SSE_MAX_HEADERS 100

/* response limit: 128 kByte */
#define PMIX_COMMON_SSE_RESPONSE_LIMIT 128 * 1024

PMIX_EXPORT void pmix_common_sse_parse_sse(char *ptr, size_t size, void *cbdata);

PMIX_EXPORT void pmix_common_on_sse_event(char **headers, const char *data, const char *reply_url,
                                          void *cbdata);

#endif /* PMIX_SSE_INTERNAL_H */
