/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix.h"
#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/mca/bfrops/bfrops.h"
#include "src/mca/psec/psec.h"
#include "src/mca/ptl/ptl.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_output.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

/* carries the caller's callback across a request the host answers */
typedef struct {
    pmix_credential_cbfunc_t cbfunc;
    void *cbdata;
} pmix_cred_relay_t;

/* Return a credential to the caller of PMIx_Get_credential_nb.
 *
 * What pmix_credential_cbfunc_t transfers is the credential itself - the
 * payload inside the byte object, and its size - not the object that
 * carries it. That object remains the library's, and may therefore be
 * the frame below; what it must not do is destruct, since that would
 * release the payload the receiver now owns. Nothing is transferred
 * alongside a failure - the object handed over carries no payload there,
 * so the receiver has nothing to release. The info array is ours in either case - the same
 * contract says that array remains owned by the library - and is
 * released once the callback returns. */
static void getcbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                      pmix_buffer_t *buf, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_status_t rc, status = PMIX_ERR_UNPACK_FAILURE;
    int cnt;
    pmix_byte_object_t cred;
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:security cback from server with %d bytes", (int) buf->bytes_used);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        /* release the caller */
        if (NULL != cd->credcbfunc) {
            cd->credcbfunc(PMIX_ERR_COMM_FAILURE, NULL, NULL, 0, cd->cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        status = rc;
        goto complete;
    }
    if (PMIX_SUCCESS != status) {
        goto complete;
    }

    /* unpack the credential - the payload allocated here is what is
     * handed on to the caller */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cred, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        /* a status of success alongside nothing to point at would have
         * the caller reading a payload we never unpacked */
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        status = rc;
        goto complete;
    }

    /* unpack any returned info */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        status = rc;
        goto complete;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            status = rc;
            goto complete;
        }
    }

complete:
    pmix_output_verbose(2, pmix_globals.debug_output, "pmix:security cback from server releasing");
    /* release the caller */
    if (NULL != cd->credcbfunc) {
        /* the payload goes with it - the object must NOT be destructed */
        cd->credcbfunc(status, &cred, info, ninfo, cd->cbdata);
    } else {
        /* nobody took the payload, so it is still ours to release */
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    }
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    PMIX_RELEASE(cd);
}

/* Relay a credential the host produced to the caller of
 * PMIx_Get_credential_nb.
 *
 * The two directions of pmix_credential_cbfunc_t do not carry the same
 * ownership, and this is where they meet. Coming down from the library
 * to its caller, the credential is transferred: the caller keeps the
 * payload and releases it when done. Coming up from the host it is only
 * borrowed - the signature has no release function, so the host may
 * reclaim the payload the instant the up-call returns, and nothing tells
 * us whether it was ever malloc'd. Passing the host's payload straight
 * through would hold the caller to both contracts at once, so what goes
 * out is a copy that is genuinely the caller's. The info array is passed
 * along as it stands; it belongs to neither of them to release. */
static void cred_relay(pmix_status_t status, pmix_byte_object_t *credential,
                       pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_cred_relay_t *relay = (pmix_cred_relay_t *) cbdata;
    pmix_byte_object_t cred;

    if (NULL == relay->cbfunc) {
        /* nobody asked to be told - the host's data is still the host's,
         * so there is nothing here to release but ourselves */
        free(relay);
        return;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);

    if (PMIX_SUCCESS == status && NULL != credential && NULL != credential->bytes) {
        cred.bytes = malloc(credential->size);
        if (NULL == cred.bytes) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            status = PMIX_ERR_NOMEM;
        } else {
            memcpy(cred.bytes, credential->bytes, credential->size);
            cred.size = credential->size;
        }
    }

    /* the copy goes to the caller, who releases it - not us */
    relay->cbfunc(status, &cred, info, ninfo, relay->cbdata);
    free(relay);
}

static void mycdcb(pmix_status_t status, pmix_byte_object_t *credential,
                   pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_query_caddy_t *cb = (pmix_query_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    if (PMIX_SUCCESS == status && NULL != credential) {
        /* the payload is ours now - take it rather than copy it, and
         * empty the object we were handed so that nothing frees it twice
         * if a path above us ever grows a destruct */
        cb->bo.bytes = credential->bytes;
        cb->bo.size = credential->size;
        credential->bytes = NULL;
        credential->size = 0;
    }
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* Meet a credential request from a local psec mechanism: for a server
 * whose host offers no credential support, and for a client or tool with
 * no server to ask. The mechanism allocates the credential's payload
 * into the byte object we hand it, and that payload goes on to the
 * caller, who owns it from there - so this frame's object is never
 * destructed once the callback has been given it. The results array is
 * a different matter: the info array on this callback stays owned by the
 * library, so it is released as soon as the callback has read it. */
static pmix_status_t local_credential(const pmix_info_t info[], size_t ninfo,
                                      pmix_credential_cbfunc_t cbfunc, void *cbdata)
{
    pmix_byte_object_t cred;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    PMIX_PSEC_CREATE_CRED(rc, pmix_globals.mypeer, info, ninfo, &results, &nresults, &cred);
    if (PMIX_SUCCESS != rc) {
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        return rc;
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, &cred, results, nresults, cbdata);
    } else {
        /* nobody took the payload, so it is still ours to release */
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    }
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    return PMIX_SUCCESS;
}

PMIX_EXPORT pmix_status_t PMIx_Get_credential(const pmix_info_t info[], size_t ninfo,
                                              pmix_byte_object_t *credential)
{
    pmix_query_caddy_t cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the credential is written through below, so it has to be there */
    if (NULL == credential) {
        return PMIX_ERR_BAD_PARAM;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(credential);

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Get_credential"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    PMIX_CONSTRUCT(&cb, pmix_query_caddy_t);
    rc = PMIx_Get_credential_nb(info, ninfo, mycdcb, &cb);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        if (NULL != cb.bo.bytes) {
            /* the credential mycdcb took is handed on to our caller,
             * who releases it - the caddy must not destruct it here */
            credential->bytes = cb.bo.bytes;
            credential->size = cb.bo.size;
            cb.bo.bytes = NULL;
            cb.bo.size = 0;
        }
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Get_credential_nb(const pmix_info_t info[], size_t ninfo,
                                                 pmix_credential_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_GET_CREDENTIAL_CMD;
    pmix_status_t rc;
    pmix_query_caddy_t *cb;
    pmix_cred_relay_t *relay;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: Get_credential called with %d info",
                        (int) ninfo);

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* if we are the server */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        /* if the host doesn't support this operation,
         * see if we can generate it ourselves */
        if (NULL == pmix_host_server.get_credential) {
            return local_credential(info, ninfo, cbfunc, cbdata);
        }
        /* the host is available, so let them try to create it. What the
         * host hands back stays the host's, so the answer goes out
         * through a relay that gives our caller an object of its own */
        pmix_output_verbose(2, pmix_globals.debug_output, "pmix:get_credential handed to RM");
        relay = (pmix_cred_relay_t *) malloc(sizeof(pmix_cred_relay_t));
        if (NULL == relay) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }
        relay->cbfunc = cbfunc;
        relay->cbdata = cbdata;
        rc = pmix_host_server.get_credential(&pmix_globals.myid, info, ninfo,
                                             cred_relay, relay);
        if (PMIX_SUCCESS != rc) {
            /* the host will not be calling back */
            free(relay);
        }
        return rc;
    }

    /* if we are a client or tool and we aren't connected, see
     * if one of our internal plugins is capable of meeting the request */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return local_credential(info, ninfo, cbfunc, cbdata);
    }

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_query_caddy_t);
    cb->credcbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, getcbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    return rc;
}

static void valid_cbfunc(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_status_t rc, status = PMIX_ERR_UNPACK_FAILURE;
    int cnt;
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    PMIX_HIDE_UNUSED_PARAMS(hdr);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:security cback from server with %d bytes", (int) buf->bytes_used);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        /* release the caller */
        if (NULL != cd->validcbfunc) {
            cd->validcbfunc(PMIX_ERR_COMM_FAILURE, NULL, 0, cd->cbdata);
        }
        PMIX_RELEASE(cd);
        return;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (PMIX_SUCCESS != status) {
        goto complete;
    }

    /* unpack any returned info */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
    }

complete:
    pmix_output_verbose(2, pmix_globals.debug_output, "pmix:security cback from server releasing");
    /* release the caller */
    if (NULL != cd->validcbfunc) {
        cd->validcbfunc(status, info, ninfo, cd->cbdata);
    }
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    PMIX_RELEASE(cd);
}

static void myvalcb(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_query_caddy_t *cb = (pmix_query_caddy_t *) cbdata;
    size_t n;

    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    if (PMIX_SUCCESS == status && NULL != info) {
        cb->ninfo = ninfo;
        PMIX_INFO_CREATE(cb->info, cb->ninfo);
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&cb->info[n], &info[n]);
        }
    }
    PMIX_WAKEUP_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Validate_credential(const pmix_byte_object_t *cred,
                                                   const pmix_info_t directives[], size_t ndirs,
                                                   pmix_info_t *results[], size_t *nresults)
{
    pmix_query_caddy_t cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* set the default response */
    if (NULL != results) {
        *results = NULL;
    }
    if (NULL != nresults) {
        *nresults = 0;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Validate_credential"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    PMIX_CONSTRUCT(&cb, pmix_query_caddy_t);
    rc = PMIx_Validate_credential_nb(cred, directives, ndirs, myvalcb, &cb);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        if (NULL != cb.info && NULL != results && NULL != nresults) {
            *results = cb.info;
            *nresults = cb.ninfo;
            cb.info = NULL;
            cb.ninfo = 0;
        }
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Validate_credential_nb(const pmix_byte_object_t *cred,
                                                      const pmix_info_t directives[], size_t ndirs,
                                                      pmix_validation_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_VALIDATE_CRED_CMD;
    pmix_status_t rc;
    pmix_query_caddy_t *cb;
    pmix_info_t *results = NULL;
    size_t nresults = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: validate credential called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* if we are the server */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        /* if the host doesn't support this operation,
         * see if we can validate it ourselves */
        if (NULL == pmix_host_server.validate_credential) {
            PMIX_PSEC_VALIDATE_CRED(rc, pmix_globals.mypeer, directives, ndirs, &results, &nresults,
                                    cred);
            if (PMIX_SUCCESS == rc) {
                /* pass it back in the callback function - the cbfunc has no
                 * release function, so we retain ownership of the results
                 * and must free them regardless of whether a cbfunc was
                 * provided */
                if (NULL != cbfunc) {
                    cbfunc(PMIX_SUCCESS, results, nresults, cbdata);
                }
                if (NULL != results) {
                    PMIX_INFO_FREE(results, nresults);
                }
            }
            return rc;
        }
        /* the host is available, so let them try to validate it */
        pmix_output_verbose(2, pmix_globals.debug_output, "pmix:get_credential handed to RM");
        rc = pmix_host_server.validate_credential(&pmix_globals.myid, cred, directives, ndirs,
                                                  cbfunc, cbdata);
        return rc;
    }

    /* if we are a client or tool and we aren't connected, see
     * if one of our internal plugins is capable of meeting the request */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        PMIX_PSEC_VALIDATE_CRED(rc, pmix_globals.mypeer, directives, ndirs, &results, &nresults,
                                cred);
        if (PMIX_SUCCESS == rc) {
            /* pass it back in the callback function - the cbfunc has no
             * release function, so we retain ownership of the results
             * and must free them regardless of whether a cbfunc was
             * provided */
            if (NULL != cbfunc) {
                cbfunc(PMIX_SUCCESS, results, nresults, cbdata);
            }
            if (NULL != results) {
                PMIX_INFO_FREE(results, nresults);
            }
        }
        return rc;
    }

    /* if we are a client, then relay this request to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the credential */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cred, 1, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (0 < ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, directives, ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_query_caddy_t);
    cb->validcbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, valid_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    return rc;
}
