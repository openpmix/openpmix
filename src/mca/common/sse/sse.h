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

#ifndef PMIX_SSE_H
#define PMIX_SSE_H

#include "src/include/pmix_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/class/pmix_object.h"
#include "src/threads/threads.h"

typedef enum { PMIX_HTTP_UNDEF = -1, PMIX_HTTP_GET = 0, PMIX_HTTP_POST = 1 } pmix_sse_verb_t;

/* define a function to be called to verify the connection */
typedef void (*pmix_sse_register_cbfunc_fn_t)(long response_code, const char *effective_url,
                                              void *cbdata);

/* define a function to be called when data becomes available
 * from the request */
typedef void (*pmix_sse_on_data_cbfunc_fn_t)(const char *ptr, char **result, void *userdata);

/*
 * Application options
 */
typedef struct {
    pmix_object_t super;
    pmix_lock_t lock;                  // thread lock so requesters can wait for completion
    pmix_status_t status;              // return status of operation
    pmix_sse_verb_t verb;              // operation to perform
    const char *url;                   // URL to get
    int max_retries;                   // max number of times to try to connect
    bool debug;                        // output debug messages
    bool allow_insecure;               // allow insecure connections
    const char *ssl_cert;              // SSL cert file
    const char *ca_info;               // CA cert file
    const char *expected_content_type; // curl-response string for expected content type
    const char *body;                  // payload to be posted
    unsigned len;                      // number of bytes in body
} pmix_sse_request_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_sse_request_t);

typedef struct {
    int output; // verbose output channel for debugging
} pmix_common_sse_globals_t;
PMIX_EXPORT extern pmix_common_sse_globals_t pmix_common_sse_globals;

struct MemoryStruct {
    char *memory;
    size_t size;
};

/* initialize the sse system */
PMIX_EXPORT void pmix_sse_common_init(void);

/* register for an sse-based data stream, specifying the details of the
 * request and providing a callback function to be executed whenever data
 * becomes available on the specified channel */
PMIX_EXPORT pmix_status_t pmix_sse_register_request(pmix_sse_request_t *req,
                                                    pmix_sse_register_cbfunc_fn_t regcbfunc,
                                                    void *reqcbdata,
                                                    pmix_sse_on_data_cbfunc_fn_t ondata,
                                                    void *datcbdata);

#endif /* PMIX_SSE_H */
