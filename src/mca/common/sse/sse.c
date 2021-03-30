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
#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/include/types.h"

#include "sse.h"
#include "sse_internal.h"

pmix_common_sse_globals_t pmix_common_sse_globals = {0};

#define PMIX_SSE_CLIENT_VERSION   "0.2"
#define PMIX_SSE_CLIENT_USERAGENT "sse/" PMIX_SSE_CLIENT_VERSION

static const char *headers[] = {"Accept: text/event-stream", NULL};

typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    bool event_active;
    int max_retries; // max number of times to try to connect
    CURL *curl;
    struct curl_slist *myheaders;
    char curl_error_buf[CURL_ERROR_SIZE];
    char *expected_content_type;
    bool allow_insecure;
    char *ssl_cert;
    char *ca_info;
    pmix_sse_register_cbfunc_fn_t regcbfunc; // fn to call upon request completion
    void *regcbdata;                         // handle to pass back to regcbfunc
    pmix_sse_on_data_cbfunc_fn_t ondata;     // fn to call when data arrives
    void *datcbdata;                         // handle to pass back to ondata
} pmix_sse_curl_handle_t;

static void clcons(pmix_sse_curl_handle_t *p)
{
    p->curl = curl_easy_init();
    memset(p->curl_error_buf, 0, CURL_ERROR_SIZE);
    /* === set defaults ============================================== */
    curl_easy_setopt(p->curl, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(p->curl, CURLOPT_USERAGENT, PMIX_SSE_CLIENT_USERAGENT);
    curl_easy_setopt(p->curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(p->curl, CURLOPT_MAXREDIRS, 10);
    curl_easy_setopt(p->curl, CURLOPT_ERRORBUFFER, p->curl_error_buf);
    p->myheaders = NULL;
    // -- enable all supported built-in compressions  -------------------
    curl_easy_setopt(p->curl, CURLOPT_ACCEPT_ENCODING, "");
    p->event_active = false;
    p->allow_insecure = false;
    p->ssl_cert = NULL;
    p->ca_info = NULL;
    p->regcbfunc = NULL;
    p->regcbdata = NULL;
    p->ondata = NULL;
    p->datcbdata = NULL;
}
static void cldes(pmix_sse_curl_handle_t *p)
{
    if (p->event_active) {
        pmix_event_evtimer_del(&p->ev);
        p->event_active = false;
    }
    curl_slist_free_all(p->myheaders);
    curl_easy_cleanup(p->curl);
    if (NULL != p->ondata) {
        /* return a NULL stream so they know this has closed */
        p->ondata(NULL, NULL, p->datcbdata);
    }
    if (NULL != p->expected_content_type) {
        free(p->expected_content_type);
    }
    if (NULL != p->ssl_cert) {
        free(p->ssl_cert);
    }
    if (NULL != p->ca_info) {
        free(p->ca_info);
    }
}
static PMIX_CLASS_INSTANCE(pmix_sse_curl_handle_t, pmix_list_item_t, clcons, cldes);

static void curl_perform(int sd, short args, void *cbdata)
{
    pmix_sse_curl_handle_t *pcl = (pmix_sse_curl_handle_t *) cbdata;
    int retries = 0;
    long response_code;
    const char *effective_url = NULL;
    char *content_type;

    while (1) {
        CURLcode res = curl_easy_perform(pcl->curl);

        switch (res) {
        case CURLE_OK:
            /* get the effective URL */
            curl_easy_getinfo(pcl->curl, CURLINFO_EFFECTIVE_URL, &effective_url);
            /* get the response code */
            curl_easy_getinfo(pcl->curl, CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code < 200 || response_code >= 300) {
                pcl->regcbfunc(response_code, effective_url, pcl->regcbdata);
                PMIX_RELEASE(pcl);
                return;
            }
            if (NULL != pcl->expected_content_type) {
                /* check the expected content type */
                curl_easy_getinfo(pcl->curl, CURLINFO_CONTENT_TYPE, &content_type);
                if (NULL == content_type
                    || 0
                           != strncmp(content_type, pcl->expected_content_type,
                                      strlen(pcl->expected_content_type))) {
                    pcl->regcbfunc(PMIX_ERR_TYPE_MISMATCH, content_type, pcl->regcbdata);
                    PMIX_RELEASE(pcl);
                    return;
                }
            }
            pcl->regcbfunc(PMIX_SUCCESS, effective_url, pcl->regcbdata);
            return;

        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
            pmix_output_verbose(1, pmix_common_sse_globals.output, "curl: %s\n",
                                pcl->curl_error_buf);
            retries++;
            if (pcl->max_retries <= retries) {
                pcl->regcbfunc(PMIX_ERR_UNREACH, pcl->curl_error_buf, pcl->regcbdata);
                return;
            }
            /* delay and retry */
            PMIX_THREADSHIFT_DELAY(pcl, curl_perform, 3);
            pcl->event_active = true;
            return;

        default:
            pmix_output_verbose(1, pmix_common_sse_globals.output, "curl: %s\n",
                                pcl->curl_error_buf);
            pcl->regcbfunc(PMIX_ERR_UNREACH, pcl->curl_error_buf, pcl->regcbdata);
            return;
        }
    }
}

static void ondataprocessing(char *ptr, size_t size, size_t nmemb, void *cbdata)
{
    /* have lex parse the input data - it will callback to
     * pmix_common_on_sse_event when done */
    pmix_common_sse_parse_sse(ptr, size * nmemb, cbdata);
}

/* provide a hook for the curl reply if it is just to be ignored */
static size_t http_ignore_data(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return size * nmemb;
}

void pmix_common_on_sse_event(char **inheaders, const char *data, const char *reply_url,
                              void *cbdata)
{
    pmix_sse_curl_handle_t *pcl = (pmix_sse_curl_handle_t *) cbdata;
    pmix_sse_curl_handle_t *reply;
    char *result = NULL;
    int n;

    /* pass to the user for processing */
    if (NULL != pcl->ondata) {
        pcl->ondata(data, &result, pcl->datcbdata);
    }

    if (NULL != reply_url) {
        printf("REPLY URL\n");
        char *body = result ? result : "";
        const char *reply_headers[] = {"Content-Type:", NULL};

        fprintf(stderr, "REPLY %s (%d byte)\n", reply_url, (int) strlen(body));
        /* create a CURL handle for this request */
        reply = PMIX_NEW(pmix_sse_curl_handle_t);
        reply->max_retries = 0;
        curl_easy_setopt(reply->curl, CURLOPT_URL, reply_url);
        curl_easy_setopt(reply->curl, CURLOPT_WRITEFUNCTION, http_ignore_data);
        for (n = 0; NULL != reply_headers[n]; n++) {
            reply->myheaders = curl_slist_append(reply->myheaders, reply_headers[n]);
        }
        reply->myheaders = curl_slist_append(pcl->myheaders, "Expect:");
        curl_easy_setopt(reply->curl, CURLOPT_HTTPHEADER, reply->myheaders);
        curl_easy_setopt(reply->curl, CURLOPT_POST, 1L);         /* set POST */
        curl_easy_setopt(reply->curl, CURLOPT_POSTFIELDS, body); /* set POST data */
        curl_easy_setopt(reply->curl, CURLOPT_POSTFIELDSIZE, strlen(body));
        curl_easy_setopt(reply->curl, CURLOPT_SSL_VERIFYPEER, pcl->allow_insecure ? 0L : 1L);
        curl_easy_setopt(reply->curl, CURLOPT_SSL_VERIFYHOST, pcl->allow_insecure ? 0L : 1L);
        if (NULL != pcl->ssl_cert) {
            curl_easy_setopt(reply->curl, CURLOPT_SSLCERT, pcl->ssl_cert);
        }
        if (NULL != pcl->ca_info) {
            curl_easy_setopt(reply->curl, CURLOPT_CAINFO, pcl->ca_info);
        }
        PMIX_THREADSHIFT(pcl, curl_perform);
    }

    free(result);
}

static bool curl_initialised = false;

void pmix_sse_common_init(void)
{
    if (!curl_initialised) {
        curl_initialised = true;
        curl_global_init(CURL_GLOBAL_ALL);
#ifdef HAVE_ATEXIT
        atexit(curl_global_cleanup);
#endif
    }
}

pmix_status_t pmix_sse_register_request(pmix_sse_request_t *req,
                                        pmix_sse_register_cbfunc_fn_t regcbfunc, void *regcbdata,
                                        pmix_sse_on_data_cbfunc_fn_t ondata, void *datcbdata)
{
    pmix_sse_curl_handle_t *pcl;
    int n;

    if (!curl_initialised) {
        return PMIX_ERR_INIT;
    }
    if (PMIX_HTTP_UNDEF == req->verb) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create a CURL handle for this request */
    pcl = PMIX_NEW(pmix_sse_curl_handle_t);
    pcl->max_retries = req->max_retries;
    if (NULL != req->expected_content_type) {
        pcl->expected_content_type = strdup(req->expected_content_type);
    }
    pcl->ondata = ondata;
    pcl->datcbdata = datcbdata;
    pcl->regcbfunc = regcbfunc;
    pcl->regcbdata = regcbdata;
    pcl->allow_insecure = req->allow_insecure;
    if (NULL != req->ssl_cert) {
        pcl->ssl_cert = strdup(req->ssl_cert);
    }
    if (NULL != req->ca_info) {
        pcl->ca_info = strdup(req->ca_info);
    }

    // -- set URL -------------------------------------------------------
    curl_easy_setopt(pcl->curl, CURLOPT_URL, req->url);
    // -- set write function --------------------------------------------
    curl_easy_setopt(pcl->curl, CURLOPT_WRITEFUNCTION, ondataprocessing);
    curl_easy_setopt(pcl->curl, CURLOPT_WRITEDATA, pcl);

    /* setup headers */
    for (n = 0; NULL != headers[n]; n++) {
        pcl->myheaders = curl_slist_append(pcl->myheaders, headers[n]);
    }

    if (PMIX_HTTP_POST == req->verb) {
        /*
         * HTTP 1.1 specifies that a POST should use a "Expect: 100-continue"
         * header. This should prepare the server for a potentially large
         * upload.
         *
         * Our bodies are not that big anyways, and some servers - notably thin
         * running standalone - do not send these, leading to a one or two-seconds
         * delay.
         *
         * Therefore we disable this behavior.
         */
        pcl->myheaders = curl_slist_append(pcl->myheaders, "Expect:");
    }
    curl_easy_setopt(pcl->curl, CURLOPT_HTTPHEADER, pcl->myheaders);

    // -- set body ------------------------------------------------------

    if (PMIX_HTTP_POST == req->verb) {
        curl_easy_setopt(pcl->curl, CURLOPT_POST, 1L);              /* set POST */
        curl_easy_setopt(pcl->curl, CURLOPT_POSTFIELDS, req->body); /* set POST data */
        curl_easy_setopt(pcl->curl, CURLOPT_POSTFIELDSIZE, req->len);
    }

    /*
     * If you want to connect to a site who isn't using a certificate
     * that is signed by one of the certs in the CA bundle you have, you
     * can skip the verification of the server's certificate. This makes
     * the connection A LOT LESS SECURE.
     *
     * If you have a CA cert for the server stored someplace else than
     * in the default bundle, then the CURLOPT_CAPATH option might come
     * handy for you.
     */
    curl_easy_setopt(pcl->curl, CURLOPT_SSL_VERIFYPEER, req->allow_insecure ? 0L : 1L);

    /*
     * If the site you're connecting to uses a different host name than
     * what they have mentioned in their server certificate's commonName
     * (or subjectAltName) fields, libcurl will refuse to connect. You can
     * skip this check, but this will make the connection less secure.
     */
    curl_easy_setopt(pcl->curl, CURLOPT_SSL_VERIFYHOST, req->allow_insecure ? 0L : 1L);

    /*
     * Did the user request a specific set of certifications?
     */
    if (NULL != req->ssl_cert) {
        curl_easy_setopt(pcl->curl, CURLOPT_SSLCERT, req->ssl_cert);
    }
    if (NULL != req->ca_info) {
        curl_easy_setopt(pcl->curl, CURLOPT_CAINFO, req->ca_info);
    }

    // -- queue the request ---------------------------------------------
    PMIX_THREADSHIFT(pcl, curl_perform);

    return PMIX_SUCCESS;
}

/****  CLASS INSTANTIATION  ****/
static void rqcon(pmix_sse_request_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->status = PMIX_ERR_NOT_SUPPORTED;
    p->verb = PMIX_HTTP_UNDEF;
    p->url = NULL;
    p->max_retries = 5;
    p->debug = false;
    p->allow_insecure = false;
    p->ssl_cert = NULL;
    p->ca_info = NULL;
    p->expected_content_type = NULL;
    p->body = NULL;
    p->len = 0;
}
static void rqdes(pmix_sse_request_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_sse_request_t, pmix_object_t, rqcon, rqdes);
