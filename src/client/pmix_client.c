/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
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
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include "src/api/pmix.h"

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "event.h"

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

#include "pmix_client.h"
#include "pmix_event.h"

// local variables
static int init_cntr = 0;
static char *local_uri = NULL;
static struct sockaddr_un address;
static int server;
static pmix_errhandler_fn_t errhandler = NULL;

// global variables
pmix_client_globals_t pmix_client_globals = { 0 };

/* callback for wait completion */
/*****  RHC: need to extend this callback function to
 *****  allow for getting a return status so we can
 *****  tell the cb->cbfunc whether or not the request
 *****  was successful, and pass it back in the cb object */
static void wait_cbfunc(pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int)buf->bytes_used);

    if (NULL != buf) {
        /* transfer the data to the cb */
        pmix_bfrop.copy_payload(&cb->data, buf);
    }
    if (NULL != cb->cbfunc) {
        cb->cbfunc(PMIX_SUCCESS, NULL, cb->cbdata);
    }
    cb->active = false;
}

static void setup_globals()
{
    memset(&pmix_client_globals.address, 0, sizeof(pmix_client_globals.address));
    pmix_client_globals.cache_global = NULL;
    pmix_client_globals.cache_local = NULL;
    pmix_client_globals.cache_remote = NULL;
    pmix_client_globals.debug_level = 10; // ??
    pmix_client_globals.id = 0;
    pmix_client_globals.sd = -1;
    pmix_client_globals.evbase = NULL;
    pmix_client_globals.max_retries = 10; // TODO: Use the macro instead
    pmix_client_globals.tag = 0; // ??
    OBJ_CONSTRUCT(&pmix_client_globals.posted_recvs, pmix_list_t );
    pmix_client_globals.recv_ev_active = false;
    OBJ_CONSTRUCT(&pmix_client_globals.send_queue, pmix_list_t );
    pmix_client_globals.recv_msg = NULL;
    pmix_client_globals.send_ev_active = false;
    pmix_client_globals.server = 0;
    pmix_client_globals.timer_ev_active = false;
    pmix_client_globals.uri = NULL;
}

static int connect_to_server()
{
    int rc;
    pmix_client_globals.sd  = -1;
    rc = pmix_usock_connect(&pmix_client_globals.address, pmix_client_globals.max_retries);
    if( rc < 0 ){
        return rc;
    }
    pmix_client_globals.sd = rc;
    /* setup recv event */
    pmix_event_set(pmix_client_globals.evbase,
                   &pmix_client_globals.recv_event,
                   pmix_client_globals.sd,
                   PMIX_EV_READ | PMIX_EV_PERSIST,
                   pmix_usock_recv_handler, NULL);
    pmix_event_set_priority(&pmix_client_globals.recv_event, PMIX_EV_MSG_LO_PRI);
    pmix_event_add(&pmix_client_globals.recv_event, 0);
    pmix_client_globals.recv_ev_active = true;

    /* setup send event */
    pmix_event_set(pmix_client_globals.evbase,
                   &pmix_client_globals.send_event,
                   pmix_client_globals.sd,
                   PMIX_EV_WRITE|PMIX_EV_PERSIST,
                   pmix_usock_send_handler, NULL);
    pmix_event_set_priority(&pmix_client_globals.send_event, PMIX_EV_MSG_LO_PRI);
    pmix_client_globals.send_ev_active = false;

//    /* initiate send of first message on queue */
//    if (NULL == pmix_client_globals.send_msg) {
//        pmix_client_globals.send_msg = (pmix_usock_send_t*)
//            pmix_list_remove_first(&pmix_client_globals.send_queue);
//    }
//    if (NULL != pmix_client_globals.send_msg && !pmix_client_globals.send_ev_active) {
//        pmix_event_add(&pmix_client_globals.send_event, 0);
//        pmix_client_globals.send_ev_active = true;
//    }

}

int PMIx_Init(char **namespace, int *rank)
{
    char **uri, *srv;
    int rc;

    ++init_cntr;
    if (1 < init_cntr) {
        return PMIX_SUCCESS;
    }

    setup_globals();

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native init called");

    /* if we don't have a path to the daxemon rendezvous point,
     * then we need to return an error */
    if (NULL == local_uri) {
        /* see if we can find it */
        if (NULL == (srv = getenv("PMIX_SERVER_URI"))) {
            /* not ready yet, so decrement our init_cntr so we can come thru
             * here again */
            --init_cntr;
            /* let the caller know that the server isn't available yet */
            return PMIX_ERR_SERVER_NOT_AVAIL;
        }
        local_uri = strdup(srv);
    }

    /* setup the path to the daemon rendezvous point */
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native constructing component fields with server %s",
                        local_uri);

    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    uri = pmix_argv_split(local_uri, ':');
    if (2 != pmix_argv_count(uri)) {
        return PMIX_ERROR;
    }
    /* if the rendezvous file doesn't exist, that's an error */
    if (0 != access(uri[1], R_OK)) {
        return PMIX_ERR_NOT_FOUND;
    }
    server = strtoull(uri[0], NULL, 10);
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "%s", uri[1]);
    pmix_argv_free(uri);

    pmix_client_globals.address = address;

    /* create an event base and progress thread for us */
    if (NULL == (pmix_client_globals.evbase = pmix_start_progress_thread())) {
        return PMIX_ERROR;
    }

    /* get our namespace and rank */
    if( PMIX_SUCCESS != (rc = connect_to_server()) ){
        return rc;
    }
    
    return PMIX_SUCCESS;
}

bool PMIx_Initialized(void)
{
    if (0 < init_cntr) {
        return true;
    }
    return false;
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

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native finalize called");

    if (NULL == local_uri) {
        /* nothing was setup, so return */
        return PMIX_SUCCESS;
    }

    if ( 0 <= pmix_client_globals.sd ) {
        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        msg = OBJ_NEW(pmix_buffer_t);
        /* pack the cmd */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }

        /* create a callback object as we need to pass it to the
         * recv routine so we know which callback to use when
         * the return message is recvd */
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;

        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "pmix:native sending finalize sync to server");

        /* push the message into our event base to send to the server */
        PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

        /* wait for the ack to return */
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        OBJ_RELEASE(cb);
    }

    if (NULL != pmix_client_globals.evbase) {
        pmix_stop_progress_thread(pmix_client_globals.evbase);
        pmix_client_globals.evbase = NULL;
    }

    if (0 <= pmix_client_globals.sd) {
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
    }

    return PMIX_SUCCESS;
}

int PMIx_Abort(int flag, const char msg[])
{
    pmix_buffer_t *bfr;
    pmix_cmd_t cmd = PMIX_ABORT_CMD;
    int rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native abort called");

    if (NULL == local_uri) {
        /* no server available, so just return */
        return PMIX_SUCCESS;
    }

    /* create a buffer to hold the message */
    bfr = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(bfr, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }
    /* pack the status flag */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(bfr, &flag, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }
    /* pack the string message - a NULL is okay */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(bfr, &msg, 1, PMIX_STRING))) {
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

int PMIx_Put(pmix_scope_t scope, pmix_value_t *kv)
{
    int rc;

    /* pack the cache that matches the scope */
    if (PMIX_LOCAL == scope) {
        if (NULL == pmix_client_globals.cache_local) {
            pmix_client_globals.cache_local = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "pmix:native put local data for key %s", kv->key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_local, &kv, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
    } else if (PMIX_REMOTE == scope) {
        if (NULL == pmix_client_globals.cache_remote) {
            pmix_client_globals.cache_remote = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "pmix:native put remote data for key %s", kv->key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_remote, &kv, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
    } else {
        /* must be global */
        if (NULL == pmix_client_globals.cache_global) {
            pmix_client_globals.cache_global = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "pmix:native put global data for key %s", kv->key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_global, &kv, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
    }

#if 0
    /* if this is our uri, save it as we need to send it to our server
     * as a special, separate item */
    if (0 == strcmp(PMIX_DSTORE_URI, kv->key)) {
        local_uri = strdup(kv->data.string);
    }
    
    /* have to save a copy locally as some of our components will
     * look for it */
    (void)pmix_dstore.store(pmix_dstore_internal, &PMIX_PROC_MY_NAME, kv);
#endif
    return rc;
}


int PMIx_Fence(pmix_list_t *ranges)
{
    pmix_buffer_t *msg, *bptr;
    pmix_cmd_t cmd = PMIX_FENCE_CMD;
    pmix_cb_t *cb;
    int rc;
    pmix_scope_t scope;
    int32_t cnt;
    pmix_value_t *kp;
    pmix_identifier_t id;
    size_t i;
    uint32_t np;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native executing fence");

    if (NULL == local_uri) {
        /* no server available, so just return */
        return PMIX_SUCCESS;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the fence cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
#if 0
    /* pack the number of procs */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &nprocs, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (0 < nprocs) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, procs, nprocs, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
    }
#endif
    /* provide our URI */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &local_uri, 1, PMIX_STRING))) {
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
    if (NULL != pmix_client_globals.cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_local);
    }
    if (NULL != pmix_client_globals.cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_remote);
    }
    if (NULL != pmix_client_globals.cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_global);
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
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &np, &cnt, PMIX_UINT64))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the buffer that contains the data for the next proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &msg, &cnt, PMIX_BUFFER))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* extract the id of the contributor from the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(msg, &id, &cnt, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* extract all blobs from this proc, starting with the scope */
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(msg, &scope, &cnt, PMIX_SCOPE))) {
            /* extract the blob for this scope */
            cnt = 1;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(msg, &bptr, &cnt, PMIX_BUFFER))) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            /* now unpack and store the values - everything goes into our internal store */
            cnt = 1;
            while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
#if 0
                if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, &id, kp))) {
                    PMIX_ERROR_LOG(ret);
                }
#endif
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

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native fence released");

    return PMIX_SUCCESS;
}

static void fencenb_cbfunc(pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_buffer_t *msg, *bptr;
    int rc;
    pmix_scope_t scope;
    int32_t cnt;
    pmix_value_t *kp;
    pmix_identifier_t id;
    size_t i;
    uint32_t np;

    /* get the number of contributors */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &np, &cnt, PMIX_UINT64))) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the buffer that contains the data for the next proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &msg, &cnt, PMIX_BUFFER))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* extract the id of the contributor from the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(msg, &id, &cnt, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* extract all blobs from this proc, starting with the scope */
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(msg, &scope, &cnt, PMIX_SCOPE))) {
            /* extract the blob for this scope */
            cnt = 1;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(msg, &bptr, &cnt, PMIX_BUFFER))) {
                PMIX_ERROR_LOG(rc);
                return;
            }
            /* now unpack and store the values - everything goes into our internal store */
            cnt = 1;
            while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
#if 0
                if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, &id, kp))) {
                    PMIX_ERROR_LOG(ret);
                }
#endif
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

int PMIx_Fence_nb(pmix_list_t *ranges, bool barrier,
                  pmix_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCENB_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_scope_t scope;

    if (NULL == local_uri) {
        /* no server available, so just execute the callback */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, cbdata);
        }
        return PMIX_SUCCESS;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the fence cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
#if 0
    /* pack the number of procs */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &nprocs, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (0 < nprocs) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, procs, nprocs, PMIX_UINT64))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
    }
#endif
    /* provide our URI */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &local_uri, 1, PMIX_STRING))) {
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
    if (NULL != pmix_client_globals.cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_local);
    }
    if (NULL != pmix_client_globals.cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_remote);
    }
    if (NULL != pmix_client_globals.cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_global);
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

int PMIx_Get(const char *namespace, int rank,
             const char *key, pmix_value_t **kv)
{
    pmix_buffer_t *msg, *bptr;
    pmix_cmd_t cmd = PMIX_GET_CMD;
    pmix_cb_t *cb;
    int rc, ret;
    int32_t cnt;
    pmix_list_t vals;
    pmix_value_t *kp;
    bool found;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native getting value for proc %s:%d key %s",
                        (NULL == namespace) ? "NULL" : namespace, rank, key);

    /* first see if we already have the info in our dstore */
    OBJ_CONSTRUCT(&vals, pmix_list_t);
#if 0
    if (PMIX_SUCCESS == pmix_dstore.fetch(pmix_dstore_internal, id,
                                          key, &vals)) {
        *kv = (pmix_value_t*)pmix_list_remove_first(&vals);
        PMIX_LIST_DESTRUCT(&vals);
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "pmix:native value retrieved from dstore");
        return PMIX_SUCCESS;
    }
#endif
    
    if (NULL == local_uri) {
        /* no server available, so just return */
        return PMIX_ERR_NOT_FOUND;
    }

    /* nope - see if we can get it */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the get cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &namespace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &rank, 1, PMIX_INT))) {
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
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    found = false;
    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&cb->data, &bptr, &cnt, PMIX_BUFFER))) {
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "pmix:native retrieved %s (%s) from server for proc %s:%d",
                                kp->key, (PMIX_STRING == kp->type) ? kp->data.string : "NS",
                                (NULL == namespace) ? "NULL" : namespace, rank);
#if 0
            if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, id, kp))) {
                PMIX_ERROR_LOG(ret);
            }
#endif
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

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native get completed");
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

void PMIx_Get_nb(const char *namespace, int rank,
                 const char *key,
                 pmix_cbfunc_t cbfunc, void *cbdata)
{
    return;
}

int PMIx_Publish(pmix_scope_t scope,
                 pmix_list_t *info)
{
    pmix_buffer_t *msg;
    pmix_info_t *iptr;
    pmix_cmd_t cmd = PMIX_PUBLISH_CMD;
    int rc;
    pmix_cb_t *cb;
    
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native publish called");
    
    /* check for bozo cases */
    if (NULL == local_uri) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    if (NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* create the publish cmd */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack the scope */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack any info keys that were given */
    if (NULL != info) {
        PMIX_LIST_FOREACH(iptr, info, pmix_info_t) {
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &iptr->key, 1, PMIX_STRING))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(msg);
                return rc;
            }
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &iptr->value, 1, PMIX_STRING))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(msg);
                return rc;
            }
        }
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the server to ack our request */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);
    
    return PMIX_SUCCESS;
}

int PMIx_Lookup(pmix_list_t *info,
                char **namespace)
{
    pmix_buffer_t *msg;
    pmix_info_t *iptr;
    pmix_cmd_t cmd = PMIX_LOOKUP_CMD;
    int rc, ret;
    pmix_cb_t *cb;
    char *key, *value;
    bool found;
    int32_t cnt;
    
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native lookup called");
    
    /* check for bozo cases */
    if (NULL == local_uri) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    if (NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* create the lookup cmd */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack any info keys that were given - no need to send the value
     * fields as they are empty */
    PMIX_LIST_FOREACH(iptr, info, pmix_info_t) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &iptr->key, 1, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the server to ack our request */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* we have received the entire data blob for this request - unpack
     * and return all values, starting with the return status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    /* unpack the namespace of the process that published the service */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &value, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    if (NULL != namespace) {
        *namespace = value;  // the string was already malloc'd when unpacked
    } else if (NULL != value) {
        free(value);
    }
    cnt = 1;
    /* the returned data is in the form of a key followed by its value, so we
     * unpack the strings in pairs */
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&cb->data, &key, &cnt, PMIX_STRING))) {
        /* unpack the value */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &value, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            return rc;
        }
        /* find the matching key in the provided info list - error if not found */
        found = false;
        PMIX_LIST_FOREACH(iptr, info, pmix_info_t) {
            if (0 == strcmp(iptr->key, key)) {
                /* store the value in the pmix_info_t */
                (void)strncpy(iptr->value, value, PMIX_MAX_VALLEN);
                found = true;
                break;
            }
        }
        free(key);
        free(value);
        if (!found) {
            rc = PMIX_ERR_NOT_FOUND;
            break;
        }
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    OBJ_RELEASE(cb);
    
    return rc;
}

int PMIx_Unpublish(pmix_list_t *info)
{
    pmix_buffer_t *msg;
    pmix_info_t *iptr;
    pmix_cmd_t cmd = PMIX_UNPUBLISH_CMD;
    int rc;
    pmix_cb_t *cb;
    
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native unpublish called");
    
    /* check for bozo cases */
    if (NULL == local_uri) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    if (NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* create the unpublish cmd */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack any info keys that were given - no need for values */
    if (NULL != info) {
        PMIX_LIST_FOREACH(iptr, info, pmix_info_t) {
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &iptr->key, 1, PMIX_STRING))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(msg);
                return rc;
            }
        }
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the server to ack our request */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);
    
    return PMIX_SUCCESS;
}

bool PMIx_Get_attr(const char *namespace,
                   const char *attr, pmix_value_t **kv)
{
    pmix_buffer_t *msg, *bptr;
    pmix_value_t *kp;
    pmix_cmd_t cmd = PMIX_GETATTR_CMD;
    int rc, ret;
    int32_t cnt;
    bool found=false;
    pmix_cb_t *cb;
    pmix_value_t *lclpeers;
    
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native get_attr called");

#if 0
    /* try to retrieve the requested value from the dstore */
    OBJ_CONSTRUCT(&vals, pmix_list_t);
    if (PMIX_SUCCESS == pmix_dstore.fetch(pmix_dstore_internal, &PMIX_PROC_MY_NAME, attr, &vals)) {
        *kv = (pmix_value_t*)pmix_list_remove_first(&vals);
        PMIX_LIST_DESTRUCT(&vals);
        return true;
    }
#endif
    
    if (NULL == local_uri) {
        /* no server available, so just return */
        return PMIX_ERR_NOT_FOUND;
    }

    /* if the value isn't yet available, then we should try to retrieve
     * all the available attributes and store them for future use */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
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
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return false;
    }
    if (PMIX_SUCCESS == ret) {
        /* unpack the buffer containing the values */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &bptr, &cnt, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            return false;
        }
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "unpacked attr %s", kp->key);
#if 0
            if (PMIX_SUCCESS != (rc = pmix_dstore.store(pmix_dstore_internal, &PMIX_PROC_MY_NAME, kp))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(kp);
                cnt = 1;
                continue;
            }
#endif
            /* save the list of local peers */
            if (0 == strcmp(PMIX_LOCAL_PEERS, kp->key)) {
                OBJ_RETAIN(kp);
                lclpeers = kp;
                pmix_output_verbose(2, pmix_client_globals.debug_level,
                                    "saving local peers %s", lclpeers->data.string);
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

    return found;
}

int PMIx_Get_attr_nb(const char *namespace,
                     const char *attr,
                     pmix_cbfunc_t cbfunc, void *cbdata)
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

int PMIx_Spawn(pmix_list_t *apps,
               char jobId[], int jobIdSize,
               int errors[])
{
    pmix_app_t *ap;
    pmix_buffer_t *msg, *bptr;
    pmix_list_t vals;
    pmix_value_t *kp;
    pmix_cmd_t cmd = PMIX_SPAWN_CMD;
    int rc, ret;
    int32_t cnt;
    bool found=false;
    pmix_cb_t *cb;
    uint32_t myrank;
    char *job;
    
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "pmix:native get_attr called");

    if (NULL == local_uri) {
        /* no server available, so just return */
        return PMIX_ERR_NOT_FOUND;
    }

    /* if the value isn't yet available, then we should try to retrieve
     * all the available attributes and store them for future use */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }

    /* pack each app */
    PMIX_LIST_FOREACH(ap, apps, pmix_app_t) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &ap, 1, PMIX_APP))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }
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

    /* unpack the results, which should include the jobId and
     * any errors */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &job, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    (void)strncpy(jobId, job, jobIdSize);
    /* unpack the number of statuses returned */
    
}

void PMIx_Register_errhandler(pmix_errhandler_fn_t err)
{
    errhandler = err;
}

void pmix_client_call_errhandler(int error)
{
    if (NULL != errhandler) {
        errhandler(error);
    }
}

void PMIx_Deregister_errhandler(void)
{
    errhandler = NULL;
}

int PMIx_Connect(pmix_list_t *ranges)
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

int PMIx_Disconnect(pmix_list_t *ranges)
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
