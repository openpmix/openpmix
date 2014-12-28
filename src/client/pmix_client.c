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
#include "src/include/types.h"

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

#include "pmix_client_hash.h"
#include "pmix_client.h"
#include "usock.h"

// local variables
static int init_cntr = 0;
static char *local_uri = NULL;
static struct sockaddr_un address;
static int server;
static pmix_errhandler_fn_t errhandler = NULL;

// global variables
pmix_client_globals_t pmix_client_globals;

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

static void setup_globals(void)
{
    memset(pmix_client_globals.namespace, 0, PMIX_MAX_VALLEN);
    memset(&pmix_client_globals.address, 0, sizeof(pmix_client_globals.address));
    pmix_client_globals.cache_global = NULL;
    pmix_client_globals.cache_local = NULL;
    pmix_client_globals.cache_remote = NULL;
    pmix_client_globals.debug_level = -1;
    pmix_client_globals.debug_output = -1;
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

static int connect_to_server(void)
{
    int rc;
    pmix_client_globals.sd  = -1;
    rc = pmix_usock_connect((struct sockaddr *)&pmix_client_globals.address, pmix_client_globals.max_retries);
    if( rc < 0 ){
        return rc;
    }
    pmix_client_globals.sd = rc;
    /* setup recv event */
    event_assign(&pmix_client_globals.recv_event,
                 pmix_client_globals.evbase,
                 pmix_client_globals.sd,
                 EV_READ | EV_PERSIST,
                 pmix_usock_recv_handler, NULL);
    event_add(&pmix_client_globals.recv_event, 0);
    pmix_client_globals.recv_ev_active = true;

    /* setup send event */
    event_assign(&pmix_client_globals.send_event,
                 pmix_client_globals.evbase,
                 pmix_client_globals.sd,
                 EV_WRITE|EV_PERSIST,
                 pmix_usock_send_handler, NULL);
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
    return PMIX_SUCCESS;
}

int PMIx_Init(char namespace[], int *rank)
{
    char **uri, *evar;
    int rc;

    ++init_cntr;
    if (1 < init_cntr) {
        return PMIX_SUCCESS;
    }

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }
    
    setup_globals();
    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        pmix_client_globals.debug_level = strtol(evar, NULL, 10);
        pmix_client_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_client_globals.debug_output,
                                  pmix_client_globals.debug_level);
    }
    pmix_bfrop_open();

    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: init called");

    /* if we don't have a path to the daemon rendezvous point,
     * then we need to return an error */
    if (NULL == (evar = getenv("PMIX_SERVER_URI"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_SERVER_NOT_AVAIL;
    }
    pmix_client_globals.uri = strdup(evar);
    
    /* we also require the namespace */
    if (NULL == (evar = getenv("PMIX_NAMESPACE"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_INVALID_NAMESPACE;
    }
    (void)strncpy(namespace, evar, PMIX_MAX_VALLEN);
    (void)strncpy(pmix_client_globals.namespace, evar, PMIX_MAX_VALLEN);

    /* we also require our rank */
    if (NULL == (evar = getenv("PMIX_RANK"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;
    }
    *rank = strtol(evar, NULL, 10);

    /* setup the path to the daemon rendezvous point */
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: constructing component fields with server %s",
                        pmix_client_globals.uri);

    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    uri = pmix_argv_split(pmix_client_globals.uri, ':');
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

    /* connect to the server */
    if (PMIX_SUCCESS != (rc = connect_to_server())){
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

    pmix_output_verbose(2, pmix_client_globals.debug_output,
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

        pmix_output_verbose(2, pmix_client_globals.debug_output,
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

    pmix_bfrop_close();

    return PMIX_SUCCESS;
}

int PMIx_Abort(int flag, const char msg[])
{
    pmix_buffer_t *bfr;
    pmix_cmd_t cmd = PMIX_ABORT_CMD;
    int rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix:native abort called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
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

int PMIx_Put(pmix_scope_t scope, const char key[], pmix_value_t *val)
{
    int rc;
    pmix_kval_t kv, *kp;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: executing put");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* setup to xfer the data */
    OBJ_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = (char*)key;
    kv.value = val;
    kp = &kv;
    
    /* pack the cache that matches the scope */
    if (PMIX_LOCAL == scope) {
        if (NULL == pmix_client_globals.cache_local) {
            pmix_client_globals.cache_local = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "pmix: put local data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_local, &kp, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    } else if (PMIX_REMOTE == scope) {
        if (NULL == pmix_client_globals.cache_remote) {
            pmix_client_globals.cache_remote = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "pmix: put remote data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_remote, &kp, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    } else {
        /* must be global */
        if (NULL == pmix_client_globals.cache_global) {
            pmix_client_globals.cache_global = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "pmix: put global data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_global, &kp, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    }
    /* protect the incoming values */
    kv.key = NULL;
    kv.value = NULL;
    /* destruct the object */
    OBJ_DESTRUCT(&kv);

    return rc;
}

static int unpack_return(pmix_buffer_t *data)
{
    int rc;
    int32_t cnt;
    pmix_buffer_t *msg, *bptr;
    char *nspace;
    uint32_t rank, i, np;
    pmix_scope_t scope;
    pmix_kval_t *kp;
    
    /* get the number of contributors */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &np, &cnt, PMIX_UINT32))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the buffer that contains the data for the next proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &msg, &cnt, PMIX_BUFFER))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* extract the namespace of the contributor from the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(msg, &nspace, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* extract the rank of the contributor from the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(msg, &rank, &cnt, PMIX_UINT32))) {
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
            while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &kp, &cnt, PMIX_KVAL))) {
                if (PMIX_SUCCESS != (rc = pmix_client_hash_store(nspace, rank, kp))) {
                    PMIX_ERROR_LOG(rc);
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

    return rc;
}

static int pack_fence(pmix_buffer_t *msg,
                      pmix_cmd_t cmd,
                      const pmix_range_t ranges[],
                      size_t nranges)
{
    int rc;
    pmix_scope_t scope;
    
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of ranges */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &nranges, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 < nranges) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, ranges, nranges, PMIX_RANGE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* if we haven't already done it, ensure we have committed our values */
    if (NULL != pmix_client_globals.cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_local);
    }
    if (NULL != pmix_client_globals.cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_remote);
    }
    if (NULL != pmix_client_globals.cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &pmix_client_globals.cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        OBJ_RELEASE(pmix_client_globals.cache_global);
    }
    return PMIX_SUCCESS;
}

int PMIx_Fence(const pmix_range_t ranges[], size_t nranges)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCE_CMD;
    pmix_cb_t *cb;
    int rc;
    pmix_range_t rg, *rgs;
    size_t nrg;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: executing fence");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == ranges && 0 != nranges) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we are given a NULL range, then the caller is referencing
     * all procs within our own namespace */
    if (NULL == ranges) {
        (void)strncpy(rg.namespace, pmix_client_globals.namespace, PMIX_MAX_VALLEN);
        rg.ranks = NULL;
        rg.nranks = 0;
        rgs = &rg;
        nrg = 1;
    } else {
        rgs = (pmix_range_t*)ranges;
        nrg = nranges;
    }
    
    msg = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pack_fence(msg, cmd, rgs, nrg))) {
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

    /* wait for the fence to complete */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* unpack and store any returned data */
    rc = unpack_return(&cb->data);
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix:native fence released");

    return rc;
}

static void fencenb_cbfunc(pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: fence_nb callback recvd");

    rc = unpack_return(buf);

    /* if a callback was provided, execute it */
    if (NULL != cb && NULL != cb->cbfunc) {
        cb->cbfunc(rc, NULL, cb->cbdata);
    }
    OBJ_RELEASE(cb);
}

int PMIx_Fence_nb(const pmix_range_t ranges[], size_t nranges, bool barrier,
                  pmix_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCENB_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_range_t rg, *rgs;
    size_t nrg;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: fence_nb called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == ranges && 0 != nranges) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we are given a NULL range, then the caller is referencing
     * all procs within our own namespace */
    if (NULL == ranges) {
        (void)strncpy(rg.namespace, pmix_client_globals.namespace, PMIX_MAX_VALLEN);
        rg.ranks = NULL;
        rg.nranks = 0;
        rgs = &rg;
        nrg = 1;
    } else {
        rgs = (pmix_range_t*)ranges;
        nrg = nranges;
    }
    
    msg = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pack_fence(msg, cmd, rgs, nrg))) {
        OBJ_RELEASE(msg);
        return rc;
    }
    /* add the barrier flag - this tells the server whether or not
     * we wish to delay the callback until a background barrier
     * has been completed. If true, or if the server doesn't support
     * direct-modex operations (i.e., the server requires a barrier
     * as part of the fence operation to ensure data has been distributed
     * prior to anyone calling "get"), then the server should execute a
     * collective barrier across all participating procs before calling
     * us back. Otherwise, the callback should come immediately */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &barrier, 1, PMIX_BOOL))) {
        OBJ_RELEASE(msg);
        return rc;
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

static pmix_buffer_t* pack_get(const char namespace[], int rank,
                               const char key[], pmix_cmd_t cmd)
{
    pmix_buffer_t *msg;
    int rc;
    
    /* nope - see if we can get it */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the get cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return NULL;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &namespace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return NULL;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &rank, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return NULL;
    }
    return msg;
}

static int unpack_get_return(pmix_buffer_t *data, const char *key,
                             const char *namespace, int rank,
                             pmix_value_t **val)
{
    int32_t cnt;
    int ret, rc;
    pmix_kval_t *kp;
    pmix_buffer_t *bptr;

    /* init the return */
    *val = NULL;
    
    /* we have received the entire data blob for this process - unpack
     * and cache all values, keeping the one we requested to return
     * to the caller */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(data, &bptr, &cnt, PMIX_BUFFER))) {
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(bptr, &kp, &cnt, PMIX_KVAL))) {
            pmix_output_verbose(2, pmix_client_globals.debug_output,
                                "pmix: retrieved %s from server for proc %s:%d",
                                kp->key, namespace, rank);
            if (PMIX_SUCCESS != (rc = pmix_client_hash_store(namespace, rank, kp))) {
                PMIX_ERROR_LOG(ret);
            }
            if (0 == strcmp(key, kp->key)) {
                if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)val, &kp->value, PMIX_VALUE))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(kp);
                    return rc;
                }
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
    return rc;
}

int PMIx_Get(const char namespace[], int rank,
             const char key[], pmix_value_t **val)
{
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    int rc;
    char *nm;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: getting value for proc %s:%d key %s",
                        (NULL == namespace) ? "NULL" : namespace, rank,
                        (NULL == key) ? "NULL" : key);

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* protect against bozo input */
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the namespace is NULL, then the caller is referencing
     * our own namespace */
    if (NULL == namespace) {
        nm = pmix_client_globals.namespace;
    } else {
        nm = (char*)namespace;
    }
    
    /* first see if we already have the info in our dstore */
    if (PMIX_SUCCESS == pmix_client_hash_fetch(nm, rank, key, val)) {
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "pmix: value retrieved from dstore");
        return PMIX_SUCCESS;
    }
    
    if (NULL == (msg = pack_get(nm, rank, key, PMIX_GET_CMD))) {
        return PMIX_ERROR;
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

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix:native get completed");

    if (PMIX_SUCCESS == (rc = unpack_get_return(&cb->data, key, nm, rank, val)) &&
        NULL != *val) {
        OBJ_RELEASE(cb);
        return PMIX_SUCCESS;
    }
    OBJ_RELEASE(cb);

    /* we didn't find the requested data - pass back a
     * status that indicates the source of the problem,
     * either during the data fetch, message unpacking,
     * or not found */
    *val = NULL;
    return rc;
}

static void getnb_cbfunc(pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc;
    pmix_value_t *val;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: get_nb callback recvd");

    rc = unpack_get_return(buf, cb->key, cb->namespace, cb->rank, &val);

    /* if a callback was provided, execute it */
    if (NULL != cb && NULL != cb->cbfunc) {
        cb->cbfunc(rc, val, cb->cbdata);
    }
    if (NULL != val) {
        PMIx_free_value_data(val);
        free(val);
    }
    OBJ_RELEASE(cb);
}

static void getnb_shortcut(int fd, short flags, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    pmix_value_t *val;
    int rc;
    int32_t m;

    if (NULL != cb->cbfunc) {
        m=1;
        rc = pmix_bfrop.unpack(&cb->data, &val, &m, PMIX_VALUE);
        cb->cbfunc(rc, val, cb->cbdata);
        if (NULL != val) {
            PMIx_free_value_data(val);
            free(val);
        }
    }
    OBJ_RELEASE(cb);
}

int PMIx_Get_nb(const char *namespace, int rank,
                const char *key,
                pmix_cbfunc_t cbfunc, void *cbdata)
{
    pmix_value_t *val;
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    int rc;
    char *nm;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: get_nb value for proc %s:%d key %s",
                        (NULL == namespace) ? "NULL" : namespace, rank,
                        (NULL == key) ? "NULL" : key);
    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* protect against bozo input */
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* if the namespace is NULL, then the caller is referencing
     * our own namespace */
    if (NULL == namespace) {
        nm = pmix_client_globals.namespace;
    } else {
        nm = (char*)namespace;
    }
    
    /* first see if we already have the info in our dstore */
    if (PMIX_SUCCESS == pmix_client_hash_fetch(nm, rank, key, &val)) {
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "pmix: value retrieved from dstore");
        /* need to push this into the event library to ensure
         * the callback occurs within an event */
        cb = OBJ_NEW(pmix_cb_t);
        cb->namespace = strdup(nm);
        cb->rank = rank;
        cb->key = strdup(key);
        cb->cbfunc = cbfunc;
        cb->cbdata = cbdata;
        /* pack the return data so the unpack routine can get it */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&cb->data, &val, 1, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
        }
        /* cleanup */
        if (NULL != val) {
            PMIx_free_value_data(val);
            free(val);
        }
        /* activate the event */
        event_assign(&(cb->ev), pmix_client_globals.evbase, -1,
                     EV_WRITE, getnb_shortcut, cb);
        event_active(&(cb->ev), EV_WRITE, 1);
        return PMIX_SUCCESS;
    }
    
    if (NULL == (msg = pack_get(nm, rank, key, PMIX_GETNB_CMD))) {
        return PMIX_ERROR;
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->namespace = strdup(nm);
    cb->rank = rank;
    cb->key = strdup(key);
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, getnb_cbfunc, cb);

    return PMIX_SUCCESS;
}

int PMIx_Publish(pmix_scope_t scope,
                 const pmix_info_t info[],
                 size_t ninfo)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_PUBLISH_CMD;
    int rc;
    pmix_cb_t *cb;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: publish called");
    
    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo cases */
    if (NULL == info) {
        /* nothing to publish */
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
    /* pack the info keys that were given */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &info, ninfo, PMIX_INFO))) {
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

    /* wait for the server to ack our request */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);
    
    return PMIX_SUCCESS;
}

int PMIx_Lookup(pmix_info_t info[], size_t ninfo,
                char namespace[])
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_LOOKUP_CMD;
    int rc, ret;
    pmix_cb_t *cb;
    char *key;
    bool found;
    int32_t cnt;
    size_t i;
    pmix_kval_t *kv;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: lookup called");
    
    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo cases */
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
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    for (i=0; i < ninfo; i++) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &info[i].key, 1, PMIX_STRING))) {
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
    /* unpack the namespace of the process that published the info */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &key, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    if (NULL != namespace) {
        strncpy(namespace, key, PMIX_MAX_VALLEN);
    }
    free(key);

    cnt = 1;
    /* unpack the returned values */
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&cb->data, &kv, &cnt, PMIX_KVAL))) {
        /* find the matching key in the provided info array - error if not found */
        found = false;
        for (i=0; i < ninfo; i++) {
            if (0 == strcmp(info[i].key, kv->key)) {
                /* transfer the value to the pmix_info_t */
                info[i].value = kv->value;
                kv->value = NULL;
                found = true;
                break;
            }
        }
        OBJ_RELEASE(kv);
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

int PMIx_Unpublish(const pmix_info_t info[], size_t ninfo)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_UNPUBLISH_CMD;
    int rc;
    pmix_cb_t *cb;
    size_t i;
    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: unpublish called");
    
    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo case */
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
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    for (i=0; i < ninfo; i++) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &info[i].key, 1, PMIX_STRING))) {
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
    OBJ_RELEASE(cb);
    
    return PMIX_SUCCESS;
}

int PMIx_Spawn(const pmix_app_t apps[],
               size_t napps,
               char namespace[])
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_SPAWN_CMD;
    int rc, ret;
    int32_t cnt;
    pmix_cb_t *cb;
    char *nspace;
   
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: spawn called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }

    /* pack the apps */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &napps, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &apps, napps, PMIX_APP))) {
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

    /* unpack the results, which should include the namespace and
     * any errors */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &nspace, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    (void)strncpy(namespace, nspace, PMIX_MAX_VALLEN);
    
    /* get the status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    return ret;
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

int PMIx_Connect(const pmix_range_t ranges[], size_t nranges)
{
    return PMIX_ERR_NOT_IMPLEMENTED;
}

int PMIx_Disconnect(const pmix_range_t ranges[], size_t nranges)
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
    p->namespace = NULL;
    p->rank = -1;
    p->key = NULL;
}
static void cbdes(pmix_cb_t *p)
{
    OBJ_DESTRUCT(&p->data);
    if (NULL != p->namespace) {
        free(p->namespace);
    }
    if (NULL != p->key) {
        free(p->key);
    }
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

void PMIx_free_value_data(pmix_value_t *val)
{
    size_t n;
    char **str;
    
    if (PMIX_STRING == val->type &&
        NULL != val->data.string) {
        free(val->data.string);
        return;
    }
    if (PMIX_ARRAY == val->type) {
        if (NULL == val->data.array.array) {
            return;
        }
        if (PMIX_STRING == val->data.array.type) {
            str = (char**)val->data.array.array;
            for (n=0; n < val->data.array.size; n++) {
                if (NULL != str[n]) {
                    free(str[n]);
                }
            }
        }
        free(val->data.array.array);
    }
    /* all other types have no malloc'd storage */
}

