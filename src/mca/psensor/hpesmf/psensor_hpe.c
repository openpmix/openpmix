/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
  *
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#include <stdio.h>
#include <pthread.h>
#include <jansson.h>
#include PMIX_EVENT_HEADER

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/show_help.h"
#include "src/include/pmix_globals.h"
#include "src/mca/common/sse/sse.h"

#include "src/mca/psensor/base/base.h"
#include "psensor_hpe.h"

/* declare the API functions */
static pmix_status_t hpesmf_start(pmix_peer_t *requestor, pmix_status_t error,
                                     const pmix_info_t *monitor,
                                     const pmix_info_t directives[], size_t ndirs);
static pmix_status_t hpesmf_stop(pmix_peer_t *requestor, char *id);

/* instantiate the module */
pmix_psensor_base_module_t pmix_psensor_hpesmf_module = {
    .start = hpesmf_start,
    .stop = hpesmf_stop
};

static void ondata(const char *ptr, char **result, void *userdata);
static void regcbfunc(long response_code, const char *effective_url, void *cbdata);

static pmix_status_t hpesmf_start(pmix_peer_t *requestor, pmix_status_t error,
                                const pmix_info_t *monitor,
                                const pmix_info_t directives[], size_t ndirs)
{
    pmix_sse_request_t *req;
    pmix_status_t rc;

    PMIX_OUTPUT_VERBOSE((1, pmix_psensor_base_framework.framework_output,
                         "[%s:%d] initiating hpesmf monitoring for requestor %s:%d",
                         pmix_globals.myid.nspace, pmix_globals.myid.rank,
                         requestor->info->pname.nspace, requestor->info->pname.rank));

    req = PMIX_NEW(pmix_sse_request_t);
    req->verb = PMIX_HTTP_GET;
    req->url = "https://10.25.24.156:8080/v1/stream/hpesmf-logs-containers?batchsize=4&count=2&streamID=stream1";
    req->max_retries = 10;
    req->debug = true;
    req->allow_insecure = true;
    req->expected_content_type = "text/event-stream";

    /* register the request */
    rc = pmix_sse_register_request(req, ondata, regcbfunc, req);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(req);
    }
    return rc;
}

static pmix_status_t hpesmf_stop(pmix_peer_t *requestor, char *id)
{

    return PMIX_SUCCESS;
}

static void ondata(const char *data, char **result, void *userdata)
{
    json_t *root = NULL;
    json_error_t error;
    json_t *metrics;
    json_t *messages;
    json_t *msg_array_0;
    json_t *msg;
    const char *msg_str;

    root = json_loads(data, 0, &error);

    metrics  = json_object_get(root, "metrics");
    if(!json_is_object(metrics)) {
        pmix_output(0, "error: metrics is not a json object");
    }

    messages = json_object_get(metrics, "messages");
    if(!json_is_array(messages)) {
        pmix_output(0, "error: messages is not a json array");
    }
    msg_array_0 = json_array_get(messages, 0);

    if(!json_is_object(msg_array_0)) {
        pmix_output(0, "error: first element of messages is not a json object");
    }
    msg = json_object_get(msg_array_0, "message");

    /* now print the first element of the messages array as a string */
    msg_str = json_string_value(msg);
    pmix_output(0, "message: %s", msg_str);

    *result = strdup("success");
}

static void regcbfunc(long response_code, const char *effective_url, void *cbdata)
{
    pmix_output(0, "RESPONSE CODE:  %d", (int)response_code);
    pmix_output(0, "EFF URL: %s", effective_url);
}
