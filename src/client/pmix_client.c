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
#include "src/api/pmix.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"
#include "pmix_stdint.h"
#include "pmix_socket_errno.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"

#include "pmix_client_hash.h"

#define PMIX_MAX_RETRIES 10

static int usock_connect(struct sockaddr *address);
static int get_jobinfo(void);

// local variables
static int init_cntr = 0;
// TODO: remove static struct sockaddr_un address;
static int server;
static bool local_evbase = false;
static pmix_peer_t myserver;
static pmix_buffer_t *cache_local;
static pmix_buffer_t *cache_remote;
static pmix_buffer_t *cache_global;

// global variables
pmix_globals_t pmix_globals;

/* callback for wait completion */
/*****  RHC: need to extend this callback function to
 *****  allow for getting a return status so we can
 *****  tell the cb->cbfunc whether or not the request
 *****  was successful, and pass it back in the cb object */
static void wait_cbfunc(int sd, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc = PMIX_ERROR;
    int32_t cnt;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int)buf->bytes_used);

    if (NULL != buf) {
        /* unpack the returned status */
        cnt=1;
        pmix_bfrop.unpack(buf, &rc, &cnt, PMIX_INT);
        /* transfer the rest of the data to the cb */
        pmix_bfrop.copy_payload(&cb->data, buf);
    }
    if (NULL != cb->cbfunc) {
        cb->cbfunc(rc, NULL, cb->cbdata);
    }
    cb->active = false;
}

static void setup_globals(void)
{
    OBJ_CONSTRUCT(&myserver, pmix_peer_t);
    cache_global = NULL;
    cache_local = NULL;
    cache_remote = NULL;
    /* setup our copy of the pmix globals object */
    memset(&pmix_globals, 0, sizeof(pmix_globals));
    pmix_globals.debug_output = -1;
}

static int connect_to_server(struct sockaddr_un *address)
{
    int rc;

    rc = usock_connect((struct sockaddr *)address);
    if( rc < 0 ){
        return rc;
    }
    myserver.sd = rc;
    /* setup recv event */
    event_assign(&myserver.recv_event,
                 pmix_globals.evbase,
                 myserver.sd,
                 EV_READ | EV_PERSIST,
                 pmix_usock_recv_handler, &myserver);
    event_add(&myserver.recv_event, 0);
    myserver.recv_ev_active = true;

    /* setup send event */
    event_assign(&myserver.send_event,
                 pmix_globals.evbase,
                 myserver.sd,
                 EV_WRITE|EV_PERSIST,
                 pmix_usock_send_handler, &myserver);
    myserver.send_ev_active = false;
    return PMIX_SUCCESS;
}

int PMIx_Init(char namespace[], int *rank,
              struct event_base *evbase,
              char *mycredential)
{
    char **uri, *evar;
    int rc, debug_level;
    struct sockaddr_un address;
    
    ++init_cntr;
    if (1 < init_cntr) {
        return PMIX_SUCCESS;
    }

    /* setup the globals */
    setup_globals();

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }
    
    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        debug_level = strtol(evar, NULL, 10);
        pmix_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_globals.debug_output, debug_level);
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: init called");

    pmix_bfrop_open();
    pmix_client_hash_init();
    pmix_usock_init();

    /* we require the namespace */
    if (NULL == (evar = getenv("PMIX_NAMESPACE"))) {
        /* let the caller know that the server isn't available yet */
        pmix_output(0, "NO NAMESPACE");
        return PMIX_ERR_INVALID_NAMESPACE;
    }
    if (NULL != namespace) {
        (void)strncpy(namespace, evar, PMIX_MAX_NSLEN);
    }
    (void)strncpy(pmix_globals.namespace, evar, PMIX_MAX_NSLEN);
    (void)strncpy(myserver.namespace, evar, PMIX_MAX_NSLEN);

    /* if we don't have a path to the daemon rendezvous point,
     * then we need to return an error */
    if (NULL == (evar = getenv("PMIX_SERVER_URI"))) {
        /* let the caller know that the server isn't available */
        pmix_output(0, "NO SERVER URI");
        return PMIX_ERR_SERVER_NOT_AVAIL;
    }
    uri = pmix_argv_split(evar, ':');
    if (2 != pmix_argv_count(uri)) {
        pmix_argv_free(uri);
        pmix_output(0, "BAD URI");
        return PMIX_ERROR;
    }
    /* setup the path to the daemon rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    /* if the rendezvous file doesn't exist, that's an error */
    if (0 != access(uri[1], R_OK)) {
        pmix_argv_free(uri);
        pmix_output(0, "REND FILE NOT FOUND");
        return PMIX_ERR_NOT_FOUND;
    }
    server = strtoull(uri[0], NULL, 10);
    /* set the server rank */
    myserver.rank = server;
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "%s", uri[1]);
    pmix_argv_free(uri);

    /* we also require our rank */
    if (NULL == (evar = getenv("PMIX_RANK"))) {
        /* let the caller know that the server isn't available yet */
        pmix_output(0, "NO RANK");
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;
    }
    pmix_globals.rank = strtol(evar, NULL, 10);
    if (NULL != rank) {
        *rank = pmix_globals.rank;
    }
    
    /* if we were given a credential, save it */
    if (NULL != mycredential) {
        pmix_globals.credential = strdup(mycredential);
    }
    
    /* create an event base and progress thread for us */
    /* setup an event base */
    if (NULL != evbase) {
        /* use the one provided */
        pmix_globals.evbase = evbase;
        local_evbase = false;
    } else {
        /* create an event base and progress thread for us */
        if (NULL == (pmix_globals.evbase = pmix_start_progress_thread())) {
            return -1;
        }
        local_evbase = true;
    }

    /* connect to the server */
    if (PMIX_SUCCESS != (rc = connect_to_server(&address))){
        pmix_output(0, "NO CONNECT");
        return rc;
    }

    /* get all the job-related info to avoid multiple
     * calls to the server for that info */
    if (PMIX_SUCCESS != (rc = get_jobinfo())) {
        pmix_output(0, "FAILED TO RETRIEVE JOB INFO");
        return rc;
    }
    
    return PMIX_SUCCESS;
}

int PMIx_Initialized(void)
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

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client finalize called");

    if ( 0 <= myserver.sd ) {
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

        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:client sending finalize sync to server");

        /* push the message into our event base to send to the server */
        PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

        /* wait for the ack to return */
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        OBJ_RELEASE(cb);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:client finalize sync received");
    }

   if (0 <= myserver.sd) {
        CLOSE_THE_SOCKET(myserver.sd);
    }

    pmix_bfrop_close();
    pmix_usock_finalize();

    if (local_evbase) {
        pmix_stop_progress_thread(pmix_globals.evbase);
        pmix_globals.evbase = NULL;
    }

     return PMIX_SUCCESS;
}

int PMIx_Abort(int flag, const char msg[])
{
    pmix_buffer_t *bfr;
    pmix_cmd_t cmd = PMIX_ABORT_CMD;
    int rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client abort called");

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
    PMIX_ACTIVATE_SEND_RECV(&myserver, bfr, wait_cbfunc, cb);

    /* wait for the release */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);
    return PMIX_SUCCESS;
}

int PMIx_Put(pmix_scope_t scope, const char key[], pmix_value_t *val)
{
    int rc;
    pmix_kval_t kv, *kp;

    pmix_output_verbose(2, pmix_globals.debug_output,
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
        if (NULL == cache_local) {
            cache_local = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: put local data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cache_local, &kp, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    } else if (PMIX_REMOTE == scope) {
        if (NULL == cache_remote) {
            cache_remote = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: put remote data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cache_remote, &kp, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    } else {
        /* must be global */
        if (NULL == cache_global) {
            cache_global = OBJ_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: put global data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cache_global, &kp, 1, PMIX_KVAL))) {
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
    pmix_buffer_t buf;
    size_t np, i;
    pmix_kval_t *kp;
    pmix_modex_data_t *mdx;
    
    /* get the number of blobs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &np, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the modex data that contains the data for the next proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &mdx, &cnt, PMIX_MODEX))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* now unpack and store the values - everything goes into our internal store */
        OBJ_CONSTRUCT(&buf, pmix_buffer_t);
        buf.base_ptr = (char*)mdx->blob;
        buf.bytes_used = mdx->size;
        buf.bytes_allocated = mdx->size;
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&buf, &kp, &cnt, PMIX_KVAL))) {
            if (PMIX_SUCCESS != (rc = pmix_client_hash_store(mdx->namespace, mdx->rank, kp))) {
                PMIX_ERROR_LOG(rc);
            }
            OBJ_RELEASE(kp);
            cnt = 1;
        }
        OBJ_DESTRUCT(&buf);  // free's the data region
        free(mdx);
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
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
                      pmix_range_t *ranges,
                      size_t nranges,
                      int collect_data)
{
    int rc;
    pmix_scope_t scope;
    size_t i;
    pmix_range_t *r;
    
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
    /* pack any provided ranges */
    for (i=0; i < nranges; i++) {
        r = &ranges[i];
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &r, 1, PMIX_RANGE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    /* pack the collect_data flag */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &collect_data, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if we haven't already done it, ensure we have committed our values */
    if (NULL != cache_local) {
        scope = PMIX_LOCAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cache_local, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        OBJ_RELEASE(cache_local);
    }
    if (NULL != cache_remote) {
        scope = PMIX_REMOTE;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cache_remote, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        OBJ_RELEASE(cache_remote);
    }
    if (NULL != cache_global) {
        scope = PMIX_GLOBAL;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cache_global, 1, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        OBJ_RELEASE(cache_global);
    }
    return PMIX_SUCCESS;
}

int PMIx_Fence(const pmix_range_t ranges[],
               size_t nranges, int collect_data)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCE_CMD;
    pmix_cb_t *cb;
    int rc;
    pmix_range_t rg, *rgs = NULL;
    size_t nrg;

    pmix_output_verbose(2, pmix_globals.debug_output,
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
        (void)strncpy(rg.namespace, pmix_globals.namespace, PMIX_MAX_NSLEN);
        rg.ranks = NULL;
        rg.nranks = 0;
        rgs = &rg;
        nrg = 1;
    } else {
        rgs = (pmix_range_t*)ranges;
        nrg = nranges;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pack_fence(msg, cmd, rgs, nrg, collect_data))) {
        OBJ_RELEASE(msg);
        return rc;
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

    /* wait for the fence to complete */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* unpack and store any returned data */
    rc = unpack_return(&cb->data);
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence released");

    return rc;
}

static void fencenb_cbfunc(int sd, pmix_usock_hdr_t *hdr,
                           pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence_nb callback recvd");

    rc = unpack_return(buf);

    /* if a callback was provided, execute it */
    if (NULL != cb && NULL != cb->cbfunc) {
        cb->cbfunc(rc, NULL, cb->cbdata);
    }
    OBJ_RELEASE(cb);
}

int PMIx_Fence_nb(const pmix_range_t ranges[], size_t nranges,
                  int barrier, int collect_data,
                  pmix_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FENCENB_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_range_t rg, *rgs;
    size_t nrg;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: fence_nb called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == ranges && 0 != nranges) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 != collect_data && 0 == barrier) {
        /* we cannot return all the data if we don't
         * do a barrier */
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* if we are given a NULL range, then the caller is referencing
     * all procs within our own namespace */
    if (NULL == ranges) {
        (void)strncpy(rg.namespace, pmix_globals.namespace, PMIX_MAX_NSLEN);
        rg.ranks = NULL;
        rg.nranks = 0;
        rgs = &rg;
        nrg = 1;
    } else {
        rgs = (pmix_range_t*)ranges;
        nrg = nranges;
    }
    
    msg = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pack_fence(msg, cmd, rgs, nrg, collect_data))) {
        OBJ_RELEASE(msg);
        return rc;
    }
    /* add the barrier flag - if true, then the server must delay calling
     * us back until all participating procs have called fence_nb. If false,
     * then the server should call us back right away */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &barrier, 1, PMIX_INT))) {
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, fencenb_cbfunc, cb);

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
                             pmix_value_t **val)
{
    int rc;
    int32_t cnt;
    pmix_buffer_t buf;
    size_t np, i;
    pmix_kval_t *kp;
    pmix_modex_data_t *mdx;
    
    /* init the return */
    *val = NULL;
    
    /* we have received the entire data blob for this process - unpack
     * and cache all values, keeping the one we requested to return
     * to the caller */
    
    /* get the number of blobs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &np, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: unpacking %d blobs for key %s",
                        (int)np, (NULL == key) ? "NULL" : key);

    /* if data was returned, unpack and store it */
    for (i=0; i < np; i++) {
        /* get the next modex data */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(data, &mdx, &cnt, PMIX_MODEX))) {
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                break;
            }
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: unpacked blob %d of size %d",
                            (int)i, (int)mdx->size);
        if (NULL == mdx->blob) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            return PMIX_ERROR;
        }
        /* now unpack and store the values - everything goes into our internal store */
        OBJ_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_LOAD_BUFFER(&buf, mdx->blob, mdx->size);
        cnt = 1;
        while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&buf, &kp, &cnt, PMIX_KVAL))) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "pmix: unpacked key %s", kp->key);
            if (PMIX_SUCCESS != (rc = pmix_client_hash_store(mdx->namespace, mdx->rank, kp))) {
                PMIX_ERROR_LOG(rc);
            }
            if (NULL != key && 0 == strcmp(key, kp->key)) {
                    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: found requested value");
                if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)val, &kp->value, PMIX_VALUE))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(kp);
                    return rc;
                }
            } else {
                OBJ_RELEASE(kp);
            }
            cnt = 1;
        }
        OBJ_DESTRUCT(&buf);  // free's the data region
        free(mdx);
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    rc = PMIX_SUCCESS;

    return rc;
}

int PMIx_Get(const char namespace[], int rank,
             const char key[], pmix_value_t **val)
{
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    int rc;
    char *nm;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
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
        nm = pmix_globals.namespace;
    } else {
        nm = (char*)namespace;
    }
    
    /* first see if we already have the info in our dstore */
    if (PMIX_SUCCESS == pmix_client_hash_fetch(nm, rank, key, val)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

    /* wait for the data to return */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client get completed");

    if (PMIX_SUCCESS == (rc = unpack_get_return(&cb->data, key, val)) &&
        (NULL == key || NULL != *val)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:client get successfully unpacked");
        OBJ_RELEASE(cb);
        return PMIX_SUCCESS;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client get unsuccessfully unpacked");
    OBJ_RELEASE(cb);

    /* we didn't find the requested data - pass back a
     * status that indicates the source of the problem,
     * either during the data fetch, message unpacking,
     * or not found */
    *val = NULL;
    return PMIX_ERR_NOT_FOUND;
}

static void getnb_cbfunc(int sd, pmix_usock_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    int rc;
    pmix_value_t *val;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: get_nb callback recvd");

    rc = unpack_get_return(buf, cb->key, &val);

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
    
    pmix_output_verbose(2, pmix_globals.debug_output,
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
        nm = pmix_globals.namespace;
    } else {
        nm = (char*)namespace;
    }
    
    /* first see if we already have the info in our dstore */
    if (PMIX_SUCCESS == pmix_client_hash_fetch(nm, rank, key, &val)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
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
        event_assign(&(cb->ev), pmix_globals.evbase, -1,
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, getnb_cbfunc, cb);

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
    
    pmix_output_verbose(2, pmix_globals.debug_output,
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

    /* wait for the server to ack our request */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);
    
    return PMIX_SUCCESS;
}

int PMIx_Lookup(pmix_scope_t scope,
                pmix_info_t info[], size_t ninfo,
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
    pmix_value_t *value;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
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
    /* pack the scope */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

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
        strncpy(namespace, key, PMIX_MAX_NSLEN);
    }
    free(key);

    cnt = 1;
    /* unpack the returned values */
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&cb->data, &key, &cnt, PMIX_STRING))) {
        /* unpack the corresponding value */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&cb->data, &value, &cnt, PMIX_VALUE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            free(key);
            return rc;
        }
        /* find the matching key in the provided info array - error if not found */
        found = false;
        for (i=0; i < ninfo; i++) {
            if (0 == strcmp(info[i].key, key)) {
                /* transfer the value to the pmix_info_t */
                pmix_value_xfer(&info[i].value, value);
                found = true;
                break;
            }
        }
        free(key);
        PMIx_free_value_data(value);
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

int PMIx_Unpublish(pmix_scope_t scope,
                   const pmix_info_t info[],
                   size_t ninfo)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_UNPUBLISH_CMD;
    int rc;
    pmix_cb_t *cb;
    size_t i;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
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
    /* pack the scope */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &scope, 1, PMIX_SCOPE))) {
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

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
    pmix_app_t *aptr;
    size_t i;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
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
    for (i=0; i < napps; i++) {
        aptr = (pmix_app_t*)&apps[i];
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &aptr, 1, PMIX_APP))) {
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
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

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
    (void)strncpy(namespace, nspace, PMIX_MAX_NSLEN);
    
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
    pmix_globals.errhandler = err;
}

void PMIx_Deregister_errhandler(void)
{
   pmix_globals.errhandler = NULL;
}

int PMIx_Connect(const pmix_range_t ranges[], size_t nranges)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_CONNECT_CMD;
    int rc;
    pmix_cb_t *cb;
    size_t i;
    pmix_range_t *r, *rptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: connect called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == ranges || 0 >= nranges) {
        return PMIX_ERR_BAD_PARAM;
    }

    msg = OBJ_NEW(pmix_buffer_t);
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
    rptr = (pmix_range_t*)ranges;
    for (i=0; i < nranges; i++) {
        r = &rptr[i];
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &r, 1, PMIX_RANGE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

    /* wait for the connect to complete */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: connect completed");

    return PMIX_SUCCESS;
}

int PMIx_Disconnect(const pmix_range_t ranges[], size_t nranges)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_DISCONNECT_CMD;
    int rc;
    pmix_cb_t *cb;
    size_t i;
    pmix_range_t *r, *rptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: disconnect called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* check for bozo input */
    if (NULL == ranges || 0 >= nranges) {
        return PMIX_ERR_BAD_PARAM;
    }

    msg = OBJ_NEW(pmix_buffer_t);
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
    rptr = (pmix_range_t*)ranges;
    for (i=0; i < nranges; i++) {
        r = &rptr[i];
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &r, 1, PMIX_RANGE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

    /* wait for the connect to complete */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: disconnect completed");

    return PMIX_SUCCESS;
}

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

/* provide a function that retrieves the job-specific info
 * for this proc */
static int get_jobinfo(void)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_JOBINFO_CMD;
    int rc;
    pmix_cb_t *cb;
    pmix_kval_t *kv;
    int32_t cnt;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: get_jobinfo called");

    if (init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&myserver, msg, wait_cbfunc, cb);

    /* wait for the info to return */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* unpack and store the results */
    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(&cb->data, &kv, &cnt, PMIX_KVAL))) {
        if (PMIX_SUCCESS != (rc = pmix_client_hash_store(pmix_globals.namespace,
                                                         pmix_globals.rank, kv))) {
            PMIX_ERROR_LOG(rc);
        }
        OBJ_RELEASE(kv);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: get_jobinfo completed");

    return rc;
}


static int send_connect_ack(int sd)
{
    char *msg;
    pmix_usock_hdr_t hdr;
    size_t sdsize=0;
    size_t csize=0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: SEND CONNECT ACK");

    /* setup the header */
    (void)strncpy(hdr.namespace, pmix_globals.namespace, PMIX_MAX_NSLEN);
    hdr.rank = pmix_globals.rank;
    hdr.tag = UINT32_MAX;
    hdr.type = PMIX_USOCK_IDENT;

    /* if we were given a security credential, pass it along */
    if (NULL != pmix_globals.credential) {
        csize = strlen(pmix_globals.credential);
    }

    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = strlen(PMIX_VERSION) + 1 + csize;

    /* create a space for our message */
    sdsize = (sizeof(hdr) + hdr.nbytes);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg+sizeof(hdr), PMIX_VERSION, strlen(PMIX_VERSION));
    if (0 < csize) {
        memcpy(msg+sizeof(hdr)+strlen(PMIX_VERSION)+1, pmix_globals.credential, csize);
    }

    if (PMIX_SUCCESS != pmix_usock_send_blocking(sd, msg, sdsize)) {
        free(msg);
        return PMIX_ERR_UNREACH;
    }
    free(msg);
    return PMIX_SUCCESS;
}

/* we receive a connection acknowledgement from the server,
 * consisting of nothing more than a status report */
static int recv_connect_ack(int sd)
{
    int32_t reply;
    int rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT ACK FROM SERVER");
    rc = pmix_usock_recv_blocking(sd, (char*)&reply, sizeof(int32_t));
    if (PMIX_SUCCESS == rc) {
        rc = reply;
    }
    return rc;
}


static int usock_connect(struct sockaddr *addr)
{
    int rc, sd=-1;
    pmix_socklen_t addrlen = 0;
    int retries = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "usock_peer_try_connect: attempting to connect to server");
    
    addrlen = sizeof(struct sockaddr_un);
    while (retries < PMIX_MAX_RETRIES){
        retries++;
        /* Create the new socket */
        sd = socket(PF_UNIX, SOCK_STREAM, 0);
        if (sd < 0) {
            pmix_output(0, "pmix:create_socket: socket() failed: %s (%d)\n",
                        strerror(pmix_socket_errno),
                        pmix_socket_errno);
            continue;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "usock_peer_try_connect: attempting to connect to server on socket %d", sd);
        /* try to connect */
        if (connect(sd, addr, addrlen) < 0) {
            if (pmix_socket_errno == ETIMEDOUT) {
                /* The server may be too busy to accept new connections */
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "timeout connecting to server");
                CLOSE_THE_SOCKET(sd);
                continue;
            }

            /* Some kernels (Linux 2.6) will automatically software
               abort a connection that was ECONNREFUSED on the last
               attempt, without even trying to establish the
               connection.  Handle that case in a semi-rational
               way by trying twice before giving up */
            if (ECONNABORTED == pmix_socket_errno) {
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "connection to server aborted by OS - retrying");
                CLOSE_THE_SOCKET(sd);
                continue;
            }
        }
        /* otherwise, the connect succeeded - so break out of the loop */
        break;
    }

    if (retries == PMIX_MAX_RETRIES || sd < 0){
        /* We were unsuccessful in establishing this connection, and are
         * not likely to suddenly become successful */
        return PMIX_ERR_UNREACH;
    }

    /* send our identity and any authentication credentials to the server */
    if (PMIX_SUCCESS != (rc = send_connect_ack(sd))) {
        CLOSE_THE_SOCKET(sd);
        sd = -1;
        return sd;
    }

    /* receive ack from the server */
    if (PMIX_SUCCESS != (rc = recv_connect_ack(sd))) {
        CLOSE_THE_SOCKET(sd);
        sd = -1;
        return sd;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sock_peer_try_connect: Connection across to server succeeded");
    
    pmix_usock_set_nonblocking(sd);
    return sd;
}

