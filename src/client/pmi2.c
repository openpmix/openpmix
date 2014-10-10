/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/api/pmix.h"

#include "src/buffer_ops/buffer_ops.h"
#include "event.h"

#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

#include "pmix_client.h"

// local variables
static int init_cntr = 0;
static struct {
    uint32_t jid;
    uint32_t vid;
} native_pname;
static char *local_uri = NULL;
static pmix_errhandler_fn_t errhandler = NULL;

/* callback for wait completion */
static void wait_cbfunc(pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int status=PMIX_SUCCESS;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native recv callback activated with %d bytes",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        (NULL == buf) ? -1 : (int)buf->bytes_used);

    if (NULL != buf) {
        /* transfer the data to the cb */
        pmix_dss.copy_payload(&cb->data, buf);
    }
    if (NULL != cb->cbfunc) {
        cb->cbfunc(status, NULL, cb->cbdata);
    }
    cb->active = false;
}

int PMIx_Init(PMI_BOOL *spawned)
{
    char **uri, *srv;

    ++init_cntr;
    if (1 < init_cntr) {
        return PMIX_SUCCESS;
    }

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native init called",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* if we don't have a path to the daemon rendezvous point,
     * then we need to return an error UNLESS we have been directed
     * to allow init prior to having an identified server. This is
     * needed for singletons as they will start without a server
     * to support them, but may have one assigned at a later time */
    if (NULL == mca_pmix_native_component.uri) {
        if (!pmix_pmix_base_allow_delayed_server) {
            /* not ready yet, so decrement our init_cntr so we can come thru
             * here again */
            --init_cntr;
            /* let the caller know that the server isn't available yet */
            return PMIX_ERR_SERVER_NOT_AVAIL;
        }
        if (NULL == (srv = getenv("PMIX_SERVER_URI"))) {
            /* error out - should have been here, but isn't */
            return PMIX_ERROR;
        }
        mca_pmix_native_component.uri = strdup(srv);
        mca_pmix_native_component.id = PMIX_PROC_MY_NAME;
    }

    /* if we have it, setup the path to the daemon rendezvous point */
    if (NULL != mca_pmix_native_component.uri) {
        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native constructing component fields with server %s",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            mca_pmix_native_component.uri);

        memset(&mca_pmix_native_component.address, 0, sizeof(struct sockaddr_un));
        mca_pmix_native_component.address.sun_family = AF_UNIX;
        uri = pmix_argv_split(mca_pmix_native_component.uri, ':');
        if (2 != pmix_argv_count(uri)) {
            return PMIX_ERROR;
        }
        /* if the rendezvous file doesn't exist, that's an error */
        if (0 != access(uri[1], R_OK)) {
            return PMIX_ERR_NOT_FOUND;
        }
        mca_pmix_native_component.server = strtoull(uri[0], NULL, 10);
        snprintf(mca_pmix_native_component.address.sun_path,
                 sizeof(mca_pmix_native_component.address.sun_path)-1,
                 "%s", uri[1]);
        pmix_argv_free(uri);

        /* create an event base and progress thread for us */
        if (NULL == (mca_pmix_native_component.evbase = pmix_start_progress_thread("pmix_native", true))) {
            return PMIX_ERROR;
        }
    }

    /* we will connect on first send */

    return PMIX_SUCCESS;
}

PMI_BOOL PMIx_Initialized(void)
{
    if (0 < init_cntr) {
        return PMI_TRUE;
    }
    return PMI_FALSE;
}

int PMIx_Finalize(void)
{
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    pmix_cmd_t cmd = PMIX_FINALIZE_CMD;
    int rc;

    if (1 != init_cntr) {
        --init_cntr;
       return PMIX_SUCCESS;
    }
    init_cntr = 0;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native finalize called",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    if (NULL == mca_pmix_native_component.uri) {
        /* nothing was setup, so return */
        return PMIX_SUCCESS;
    }

    if (PMIX_USOCK_CONNECTED == mca_pmix_native_component.state) {
        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        msg = OBJ_NEW(pmix_buffer_t);
        /* pack the cmd */
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &cmd, 1, PMIX_CMD_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }

        /* create a callback object as we need to pass it to the
         * recv routine so we know which callback to use when
         * the return message is recvd */
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;

        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native sending finalize sync to server",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

        /* push the message into our event base to send to the server */
        PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

        /* wait for the ack to return */
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        OBJ_RELEASE(cb);
    }

    if (NULL != mca_pmix_native_component.evbase) {
        pmix_stop_progress_thread("pmix_native", true);
        mca_pmix_native_component.evbase = NULL;
    }

    if (0 <= mca_pmix_native_component.sd) {
        CLOSE_THE_SOCKET(mca_pmix_native_component.sd);
    }

    return PMIX_SUCCESS;
}

int PMIx_Abort(int flag, const char msg[])
{
    pmix_buffer_t *bfr;
    pmix_cmd_t cmd = PMIX_ABORT_CMD;
    int rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native abort called",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    if (NULL == mca_pmix_native_component.uri) {
        /* no server available, so just return */
        return PMIX_SUCCESS;
    }

    /* create a buffer to hold the message */
    bfr = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(bfr, &cmd, 1, PMIX_CMD_T))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }
    /* pack the status flag */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(bfr, &flag, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }
    /* pack the string message - a NULL is okay */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(bfr, &msg, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(bfr, wait_cbfunc, cb);

    /* wait for the release */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);
    return PMIX_SUCCESS;
}

int PMIx_Job_spawn(int count, const char * cmds[],
                   int argcs[], const char ** argvs[],
                   const int maxprocs[],
                   pmix_list_t *info_keyval_vector,
                   pmix_list_t *preput_keyval_vector,
                   char jobId[], int jobIdSize,
                   int errors[])
{
    return PMIX_ERR_NOT_SUPPORTED;
}

int PMIx_Put(pmix_scope_t scope, pmix_value_t *kv)
{
    int rc;

    /* pack the cache that matches the scope */
    if (PMIX_LOCAL == scope) {
        if (NULL == mca_pmix_native_component.cache_local) {
            mca_pmix_native_component.cache_local = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native put local data for key %s",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), kv->key);
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(mca_pmix_native_component.cache_local, &kv, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
    } else if (PMIX_REMOTE == scope) {
        if (NULL == mca_pmix_native_component.cache_remote) {
            mca_pmix_native_component.cache_remote = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native put remote data for key %s",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), kv->key);
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(mca_pmix_native_component.cache_remote, &kv, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
    } else {
        /* must be global */
        if (NULL == mca_pmix_native_component.cache_global) {
            mca_pmix_native_component.cache_global = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native put global data for key %s",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), kv->key);
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(mca_pmix_native_component.cache_global, &kv, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
    }

    /* if this is our uri, save it as we need to send it to our server
     * as a special, separate item */
    if (0 == strcmp(PMIX_DSTORE_URI, kv->key)) {
        local_uri = strdup(kv->data.string);
    }

    /* have to save a copy locally as some of our components will
     * look for it */
    (void)pmix_dstore.store(pmix_dstore_internal, &PMIX_PROC_MY_NAME, kv);
    return rc;
}


int PMIx_Fence(pmix_identifier_t *procs, size_t nprocs)
{
    pmix_buffer_t *msg, *bptr;
    pmix_cmd_t cmd = PMIX_FENCE_CMD;
    pmix_cb_t *cb;
    int rc, ret;
    pmix_pmix_scope_t scope;
    int32_t cnt;
    pmix_value_t *kp;
    pmix_identifier_t id;
    size_t i;
    uint32_t np;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native executing fence on %u procs",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), (unsigned int)nprocs);

    if (NULL == mca_pmix_native_component.uri) {
        /* no server available, so just return */
        return PMIX_SUCCESS;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the fence cmd */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &cmd, 1, PMIX_CMD_T))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack the number of procs */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &nprocs, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (0 < nprocs) {
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, procs, nprocs, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
    }
    /* provide our URI */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &local_uri, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* only do it once */
    if (NULL != local_uri) {
        free(local_uri);
        local_uri = NULL;
    }

    /* if we haven't already done it, ensure we have committed our values */
    if (NULL != mca_pmix_native_component.cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &scope, 1, PMIX_SCOPE_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &mca_pmix_native_component.cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(mca_pmix_native_component.cache_local);
    }
    if (NULL != mca_pmix_native_component.cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &scope, 1, PMIX_SCOPE_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &mca_pmix_native_component.cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(mca_pmix_native_component.cache_remote);
    }
    if (NULL != mca_pmix_native_component.cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &scope, 1, PMIX_SCOPE_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &mca_pmix_native_component.cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(mca_pmix_native_component.cache_global);
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the fence to complete */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* get the number of contributors */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_dss.unpack(&cb->data, &np, &cnt, PMIX_UINT64))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the buffer that contains the data for the next proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_dss.unpack(&cb->data, &msg, &cnt, PMIX_BUFFER))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* extract the id of the contributor from the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_dss.unpack(msg, &id, &cnt, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* extract all blobs from this proc, starting with the scope */
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_dss.unpack(msg, &scope, &cnt, PMIX_SCOPE_T))) {
            /* extract the blob for this scope */
            cnt = 1;
            if (PMIX_SUCCESS != (rc = pmix_dss.unpack(msg, &bptr, &cnt, PMIX_BUFFER))) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            /* now unpack and store the values - everything goes into our internal store */
            cnt = 1;
            while (PMIX_SUCCESS == (rc = pmix_dss.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
                if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, &id, kp))) {
                    PMIX_ERROR_LOG(ret);
                }
                OBJ_RELEASE(kp);
                cnt = 1;
            }
            OBJ_RELEASE(bptr);
            cnt = 1;
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
        OBJ_RELEASE(msg);
    }
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }

    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native fence released",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    return PMIX_SUCCESS;
}

static void fencenb_cbfunc(pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_buffer_t *msg, *bptr;
    int rc, ret;
    pmix_pmix_scope_t scope;
    int32_t cnt;
    pmix_value_t *kp;
    pmix_identifier_t id;
    size_t i;
    uint32_t np;

    /* get the number of contributors */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_dss.unpack(buf, &np, &cnt, PMIX_UINT64))) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the buffer that contains the data for the next proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_dss.unpack(buf, &msg, &cnt, PMIX_BUFFER))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* extract the id of the contributor from the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_dss.unpack(msg, &id, &cnt, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* extract all blobs from this proc, starting with the scope */
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_dss.unpack(msg, &scope, &cnt, PMIX_SCOPE_T))) {
            /* extract the blob for this scope */
            cnt = 1;
            if (PMIX_SUCCESS != (rc = pmix_dss.unpack(msg, &bptr, &cnt, PMIX_BUFFER))) {
                PMIX_ERROR_LOG(rc);
                return;
            }
            /* now unpack and store the values - everything goes into our internal store */
            cnt = 1;
            while (PMIX_SUCCESS == (rc = pmix_dss.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
                if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, &id, kp))) {
                    PMIX_ERROR_LOG(ret);
                }
                OBJ_RELEASE(kp);
                cnt = 1;
            }
            OBJ_RELEASE(bptr);
            cnt = 1;
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
        OBJ_RELEASE(msg);
    }
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }

    /* if a callback was provided, execute it */
    if (NULL != cb && NULL != cb->cbfunc) {
        cb->cbfunc(rc, NULL, cb->cbdata);
    }
    OBJ_RELEASE(cb);
}

int PMIx_Fence_nb(pmix_identifier_t *procs, size_t nprocs,
                  pmix_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCENB_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_pmix_scope_t scope;

    if (NULL == mca_pmix_native_component.uri) {
        /* no server available, so just execute the callback */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, cbdata);
        }
        return PMIX_SUCCESS;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the fence cmd */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &cmd, 1, PMIX_CMD_T))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack the number of procs */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &nprocs, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (0 < nprocs) {
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, procs, nprocs, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
    }
    /* provide our URI */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &local_uri, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* only do it once */
    if (NULL != local_uri) {
        free(local_uri);
        local_uri = NULL;
    }

    /* if we haven't already done it, ensure we have committed our values */
    if (NULL != mca_pmix_native_component.cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &scope, 1, PMIX_SCOPE_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &mca_pmix_native_component.cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(mca_pmix_native_component.cache_local);
    }
    if (NULL != mca_pmix_native_component.cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &scope, 1, PMIX_SCOPE_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &mca_pmix_native_component.cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(mca_pmix_native_component.cache_remote);
    }
    if (NULL != mca_pmix_native_component.cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &scope, 1, PMIX_SCOPE_T))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &mca_pmix_native_component.cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(mca_pmix_native_component.cache_global);
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, fencenb_cbfunc, cb);

    return PMIX_SUCCESS;
}

int PMIx_Get(const pmix_identifier_t *id,
             const char *key,
             pmix_value_t **kv)
{
    pmix_buffer_t *msg, *bptr;
    pmix_cmd_t cmd = PMIX_GET_CMD;
    pmix_cb_t *cb;
    int rc, ret;
    int32_t cnt;
    pmix_list_t vals;
    pmix_value_t *kp;
    bool found;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native getting value for proc %s key %s",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        PMIX_NAME_PRINT(*id), key);

    /* first see if we already have the info in our dstore */
    OBJ_CONSTRUCT(&vals, pmix_list_t);
    if (PMIX_SUCCESS == pmix_dstore.fetch(pmix_dstore_internal, id,
                                          key, &vals)) {
        *kv = (pmix_value_t*)pmix_list_remove_first(&vals);
        PMIX_LIST_DESTRUCT(&vals);
        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native value retrieved from dstore",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        return PMIX_SUCCESS;
    }

    if (NULL == mca_pmix_native_component.uri) {
        /* no server available, so just return */
        return PMIX_ERR_NOT_FOUND;
    }

    /* nope - see if we can get it */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the get cmd */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &cmd, 1, PMIX_CMD_T))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, id, 1, PMIX_UINT64))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the data to return */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* we have received the entire data blob for this process - unpack
     * and cache all values, keeping the one we requested to return
     * to the caller */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_dss.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    found = false;
    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_dss.unpack(&cb->data, &bptr, &cnt, PMIX_BUFFER))) {
        while (PMIX_SUCCESS == (rc = pmix_dss.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
            pmix_output_verbose(2, pmix_debug_output,
                                "%s pmix:native retrieved %s (%s) from server for proc %s",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), kp->key,
                                (PMIX_STRING == kp->type) ? kp->data.string : "NS",
                                PMIX_NAME_PRINT(*id));
            if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, id, kp))) {
                PMIX_ERROR_LOG(ret);
            }
            if (0 == strcmp(key, kp->key)) {
                *kv = kp;
                found = true;
            } else {
                OBJ_RELEASE(kp);
            }
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
        OBJ_RELEASE(bptr);
        cnt = 1;
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native get completed",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
    if (found) {
        return PMIX_SUCCESS;
    }
    /* we didn't find the requested data - pass back a
     * status that indicates the source of the problem,
     * either during the data fetch, message unpacking,
     * or not found */
    *kv = NULL;
    if (PMIX_SUCCESS == rc) {
        if (PMIX_SUCCESS == ret) {
            rc = PMIX_ERR_NOT_FOUND;
        } else {
            rc = ret;
        }
    }
    return rc;
}

void PMIx_Get_nb(const pmix_identifier_t *id,
                 const char *key,
                 pmix_cbfunc_t cbfunc,
                 void *cbdata)
{
    return;
}

int PMIx_Publish(const char service_name[],
                 pmix_list_t *info,
                 const char port[])
{
    return PMIX_SUCCESS;
}

int PMIx_Lookup(const char service_name[],
                pmix_list_t *info,
                char port[], int portLen)
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

int PMIx_Unpublish(const char service_name[], 
                   pmix_list_t *info)
{
    return PMIX_SUCCESS;;
}

PMIx_BOOL PMIx_Get_attr(const char *attr, pmix_value_t **kv)
{
    pmix_buffer_t *msg, *bptr;
    pmix_list_t vals;
    pmix_value_t *kp;
    pmix_cmd_t cmd = PMIX_GETATTR_CMD;
    int rc, ret;
    int32_t cnt;
    bool found=false;
    pmix_cb_t *cb;
    uint32_t i, myrank;
    pmix_identifier_t id;
    char *cpuset;
    pmix_buffer_t buf;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native get_attr called",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* try to retrieve the requested value from the dstore */
    OBJ_CONSTRUCT(&vals, pmix_list_t);
    if (PMIX_SUCCESS == pmix_dstore.fetch(pmix_dstore_internal, &PMIX_PROC_MY_NAME, attr, &vals)) {
        *kv = (pmix_value_t*)pmix_list_remove_first(&vals);
        PMIX_LIST_DESTRUCT(&vals);
        return true;
    }

    if (NULL == mca_pmix_native_component.uri) {
        /* no server available, so just return */
        return PMIX_ERR_NOT_FOUND;
    }

    /* if the value isn't yet available, then we should try to retrieve
     * all the available attributes and store them for future use */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &cmd, 1, PMIX_CMD_T))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return false;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the data to return */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* we have received the entire data blob for this process - unpack
     * and cache all values, keeping the one we requested to return
     * to the caller */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_dss.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return false;
    }
    if (PMIX_SUCCESS == ret) {
        /* unpack the buffer containing the values */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_dss.unpack(&cb->data, &bptr, &cnt, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            return false;
        }
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_dss.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
            pmix_output_verbose(2, pmix_debug_output,
                                "%s unpacked attr %s",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), kp->key);

            if (PMIX_SUCCESS != (rc = pmix_dstore.store(pmix_dstore_internal, &PMIX_PROC_MY_NAME, kp))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(kp);
                cnt = 1;
                continue;
            }
            /* save the list of local peers */
            if (0 == strcmp(PMIX_LOCAL_PEERS, kp->key)) {
                OBJ_RETAIN(kp);
                lclpeers = kp;
                pmix_output_verbose(2, pmix_debug_output,
                                    "%s saving local peers %s",
                                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), lclpeers->data.string);
            } else if (0 == strcmp(PMIX_JOBID, kp->key)) {
                native_pname.jid = kp->data.uint32;
            } else if (0 == strcmp(PMIX_RANK, kp->key)) {
                native_pname.vid = kp->data.uint32;
            }
            if (0 == strcmp(attr, kp->key)) {
                OBJ_RETAIN(kp);
                *kv = kp;
                found = true;
            }
            OBJ_RELEASE(kp);
            cnt = 1;
        }
        OBJ_RELEASE(bptr);
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            return false;
        }
    } else {
        PMIX_ERROR_LOG(ret);
        OBJ_RELEASE(cb);
        return false;
    }
    OBJ_RELEASE(cb);
    pmix_proc_set_name((pmix_process_name_t*)&native_pname);

    return found;
}

int PMIx_Get_attr_nb(const char *attr,
                     pmix_cbfunc_t cbfunc,
                     void *cbdata)
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

void PMIx_Register_errhandler(pmix_errhandler_fn_t errhandler)
{
    errhandler = err;
}

static void pmix_call_errhandler(int error)
{
    if (NULL != errhandler) {
        errhandler(error);
    }
}

void PMIx_Deregister_errhandler(void)
{
    errhandler = NULL;
}


static int native_job_connect(const char jobId[])
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

static int native_job_disconnect(const char jobId[])
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

/***   INSTANTIATE INTERNAL CLASSES   ***/
static void scon(pmix_usock_send_t *p)
{
    p->hdr.type = 0;
    p->hdr.tag = UINT32_MAX;
    p->hdr.nbytes = 0;
    p->data = NULL;
    p->hdr_sent = false;
    p->sdptr = NULL;
    p->sdbytes = 0;
}
OBJ_CLASS_INSTANCE(pmix_usock_send_t,
                   pmix_list_item_t,
                   scon, NULL);

static void rcon(pmix_usock_recv_t *p)
{
    p->hdr.type = 0;
    p->hdr.tag = UINT32_MAX;
    p->hdr.nbytes = 0;
    p->data = NULL;
    p->hdr_recvd = false;
    p->rdptr = NULL;
    p->rdbytes = 0;
}
OBJ_CLASS_INSTANCE(pmix_usock_recv_t,
                   pmix_list_item_t,
                   rcon, NULL);

static void prcon(pmix_usock_posted_recv_t *p)
{
    p->tag = UINT32_MAX;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
OBJ_CLASS_INSTANCE(pmix_usock_posted_recv_t,
                   pmix_list_item_t,
                   prcon, NULL);

static void cbcon(pmix_cb_t *p)
{
    p->active = false;
    OBJ_CONSTRUCT(&p->data, pmix_buffer_t);
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void cbdes(pmix_cb_t *p)
{
    OBJ_DESTRUCT(&p->data);
}
OBJ_CLASS_INSTANCE(pmix_cb_t,
                   pmix_object_t,
                   cbcon, cbdes);


static void srcon(pmix_usock_sr_t *p)
{
    p->bfr = NULL;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
OBJ_CLASS_INSTANCE(pmix_usock_sr_t,
                   pmix_object_t,
                   srcon, NULL);
