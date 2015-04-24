/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
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
#include "src/include/pmix_stdint.h"
#include "src/include/pmix_socket_errno.h"

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
#include "src/sec/pmix_sec.h"

#include "pmix_client_hash.h"
#include "pmix_client_ops.h"

#define PMIX_MAX_RETRIES 10

static int usock_connect(struct sockaddr *address);
static void myerrhandler(pmix_status_t status,
                         pmix_range_t ranges[], size_t nranges,
                         pmix_info_t info[], size_t ninfo)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client default errhandler activated");
}

static void pmix_client_notify_recv(struct pmix_peer_t *peer, pmix_usock_hdr_t *hdr,
                                    pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t pstatus;
    int status, rc;
    int32_t cnt;
    pmix_range_t *ranges=NULL;
    size_t nranges, ninfo;
    pmix_info_t *info=NULL;
    
    if (NULL == pmix_globals.errhandler) {
        return;
    }

    /* unpack the status */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &status, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    pstatus = status;
    
    /* unpack the ranges that are impacted */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < nranges) {
        ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
        cnt = nranges;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ranges, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    
    /* unpack the info that might have been provided */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < ninfo) {
        info = (pmix_info_t*)malloc(ninfo * sizeof(pmix_info_t));
        cnt = ninfo;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &info, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    
    pmix_globals.errhandler(pstatus, ranges, nranges, info, ninfo);

    /* cleanup */
 cleanup:
    PMIX_RANGE_FREE(ranges, nranges);
    PMIX_INFO_FREE(info, ninfo);
}


// global variables
pmix_globals_t pmix_globals = {
    .evbase = NULL,
    .debug_output = -1,
    .errhandler = myerrhandler,
    .server = false,
};
pmix_client_globals_t pmix_client_globals = {
    .init_cntr = 0,
    .cache_local = NULL,
    .cache_remote = NULL,
    .cache_global = NULL
};
    
/* callback for wait completion */
static void wait_cbfunc(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int)buf->bytes_used);

    cb->active = false;
}
 
static void setup_globals(void)
{
    pmix_rank_info_t *info;
    
    PMIX_CONSTRUCT(&pmix_client_globals.myserver_nspace, pmix_nspace_t);
    (void)strncpy(pmix_client_globals.myserver_nspace.nspace, "pmix-server", PMIX_MAX_NSLEN);
    info = PMIX_NEW(pmix_rank_info_t);
    info->nptr = &pmix_client_globals.myserver_nspace;
    pmix_list_append(&pmix_client_globals.myserver_nspace.ranks, &info->super);
    
    PMIX_CONSTRUCT(&pmix_client_globals.myserver, pmix_peer_t);
    PMIX_RETAIN(info);
    pmix_client_globals.myserver.info = info;
    
    /* setup our copy of the pmix globals object */
    memset(&pmix_globals.nspace, 0, sizeof(pmix_globals.nspace));
}

static int connect_to_server(struct sockaddr_un *address)
{
    int rc;

    rc = usock_connect((struct sockaddr *)address);
    if( rc < 0 ){
        return rc;
    }
    pmix_client_globals.myserver.sd = rc;
    /* setup recv event */
    event_assign(&pmix_client_globals.myserver.recv_event,
                 pmix_globals.evbase,
                 pmix_client_globals.myserver.sd,
                 EV_READ | EV_PERSIST,
                 pmix_usock_recv_handler, &pmix_client_globals.myserver);
    event_add(&pmix_client_globals.myserver.recv_event, 0);
    pmix_client_globals.myserver.recv_ev_active = true;

    /* setup send event */
    event_assign(&pmix_client_globals.myserver.send_event,
                 pmix_globals.evbase,
                 pmix_client_globals.myserver.sd,
                 EV_WRITE|EV_PERSIST,
                 pmix_usock_send_handler, &pmix_client_globals.myserver);
    pmix_client_globals.myserver.send_ev_active = false;
    return PMIX_SUCCESS;
}

const char* PMIx_Get_version(void)
{
    return pmix_version_string;
}

int PMIx_Init(char nspace[], int *rank)
{
    char **uri, *evar;
    int rc, debug_level;
    struct sockaddr_un address;
    
    ++pmix_client_globals.init_cntr;
    if (1 < pmix_client_globals.init_cntr) {
        /* since we have been called before, the nspace and
         * rank should be known. So return them here if
         * requested */
        if (NULL != nspace) {
            (void)strncpy(nspace, pmix_globals.nspace, PMIX_MAX_NSLEN);
        }
        if (NULL != rank) {
            *rank = pmix_globals.rank;
        }
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
    pmix_usock_init(pmix_client_notify_recv);
    pmix_sec_init();
    
    /* we require the nspace */
    if (NULL == (evar = getenv("PMIX_NAMESPACE"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_INVALID_NAMESPACE;
    }
    if (NULL != nspace) {
        (void)strncpy(nspace, evar, PMIX_MAX_NSLEN);
    }
    (void)strncpy(pmix_globals.nspace, evar, PMIX_MAX_NSLEN);

    /* if we don't have a path to the daemon rendezvous point,
     * then we need to return an error */
    if (NULL == (evar = getenv("PMIX_SERVER_URI"))) {
        /* let the caller know that the server isn't available */
        return PMIX_ERR_SERVER_NOT_AVAIL;
    }
    uri = pmix_argv_split(evar, ':');
    if (2 != pmix_argv_count(uri)) {
        pmix_argv_free(uri);
        return PMIX_ERROR;
    }
    /* setup the path to the daemon rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    /* if the rendezvous file doesn't exist, that's an error */
    if (0 != access(uri[1], R_OK)) {
        pmix_argv_free(uri);
        return PMIX_ERR_NOT_FOUND;
    }
    /* set the server rank */
    pmix_client_globals.myserver.info->rank = strtoull(uri[0], NULL, 10);
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "%s", uri[1]);
    pmix_argv_free(uri);

    /* we also require our rank */
    if (NULL == (evar = getenv("PMIX_RANK"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;
    }
    pmix_globals.rank = strtol(evar, NULL, 10);
    if (NULL != rank) {
        *rank = pmix_globals.rank;
    }
    pmix_globals.pindex = -1;
    
    /* create an event base and progress thread for us */
    if (NULL == (pmix_globals.evbase = pmix_start_progress_thread())) {
        return -1;
    }

    /* connect to the server - returns job info if successful */
    if (PMIX_SUCCESS != (rc = connect_to_server(&address))){
        pmix_output(0, "NO CONNECT");
        return rc;
    }

    return PMIX_SUCCESS;
}

int PMIx_Initialized(void)
{
    if (0 < pmix_client_globals.init_cntr) {
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

    if (1 != pmix_client_globals.init_cntr) {
        --pmix_client_globals.init_cntr;
        return PMIX_SUCCESS;
    }
    pmix_client_globals.init_cntr = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client finalize called");

    if ( 0 <= pmix_client_globals.myserver.sd ) {
        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        msg = PMIX_NEW(pmix_buffer_t);
        /* pack the cmd */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }

        /* create a callback object as we need to pass it to the
         * recv routine so we know which callback to use when
         * the return message is recvd */
        cb = PMIX_NEW(pmix_cb_t);
        cb->active = true;

        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:client sending finalize sync to server");

        /* push the message into our event base to send to the server */
        PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, msg, wait_cbfunc, cb);

        /* wait for the ack to return */
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        PMIX_RELEASE(cb);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:client finalize sync received");
    }

    pmix_usock_finalize();
    PMIX_DESTRUCT(&pmix_client_globals.myserver);

    pmix_stop_progress_thread(pmix_globals.evbase);
    event_base_free(pmix_globals.evbase);
#ifdef HAVE_LIBEVENT_SHUTDOWN
    libevent_global_shutdown();
#endif

    if (0 <= pmix_client_globals.myserver.sd) {
        CLOSE_THE_SOCKET(pmix_client_globals.myserver.sd);
    }
    pmix_client_hash_finalize();
    pmix_bfrop_close();
    pmix_sec_finalize();
    
    pmix_output_close(pmix_globals.debug_output);
    pmix_output_finalize();
    pmix_class_finalize();

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

    if (pmix_client_globals.init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* create a buffer to hold the message */
    bfr = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(bfr, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }
    /* pack the status flag */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(bfr, &flag, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }
    /* pack the string message - a NULL is okay */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(bfr, &msg, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, bfr, wait_cbfunc, cb);

    /* wait for the release */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    PMIX_RELEASE(cb);
    return PMIX_SUCCESS;
}

int PMIx_Put(pmix_scope_t scope, const char key[], pmix_value_t *val)
{
    int rc;
    pmix_kval_t *kv;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: executing put");

    if (pmix_client_globals.init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    /* setup to xfer the data */
    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup((char*)key);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    pmix_value_xfer(kv->value, val);
    /* put it in the hash table */
    if (PMIX_SUCCESS != (rc = pmix_client_hash_store(pmix_globals.nspace, pmix_globals.rank, kv))) {
        PMIX_ERROR_LOG(rc);
    }

    /* pack the cache that matches the scope */
    if (PMIX_LOCAL == scope) {
        if (NULL == pmix_client_globals.cache_local) {
            pmix_client_globals.cache_local = PMIX_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: put local data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_local, kv, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    } else if (PMIX_REMOTE == scope) {
        if (NULL == pmix_client_globals.cache_remote) {
            pmix_client_globals.cache_remote = PMIX_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: put remote data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_remote, kv, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    } else {
        /* must be global */
        if (NULL == pmix_client_globals.cache_global) {
            pmix_client_globals.cache_global = PMIX_NEW(pmix_buffer_t);
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix: put global data for key %s", key);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(pmix_client_globals.cache_global, kv, 1, PMIX_KVAL))) {
            PMIX_ERROR_LOG(rc);
        }
    }
    PMIX_RELEASE(kv);

    return rc;
}

pmix_status_t PMIx_Commit(void)
{
    int rc;
    pmix_scope_t scope;
    pmix_buffer_t *msg;
    pmix_cmd_t cmd=PMIX_COMMIT_CMD;

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
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
        PMIX_RELEASE(pmix_client_globals.cache_local);
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
        PMIX_RELEASE(pmix_client_globals.cache_remote);
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
        PMIX_RELEASE(pmix_client_globals.cache_global);
    }
    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, msg, NULL, NULL);
    return PMIX_SUCCESS;
}

void PMIx_Register_errhandler(pmix_notification_fn_t err)
{
    pmix_globals.errhandler = err;
}

void PMIx_Deregister_errhandler(void)
{
   pmix_globals.errhandler = NULL;
}

static int send_connect_ack(int sd)
{
    char *msg;
    pmix_usock_hdr_t hdr;
    size_t sdsize=0, csize=0;
    char *cred = NULL;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: SEND CONNECT ACK");

    /* setup the header */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;

    /* reserve space for the nspace and rank info */
    sdsize = strlen(pmix_globals.nspace) + 1 + sizeof(int);
    
    /* get a credential, if the security system provides one. Not
     * every SPC will do so, thus we must first check */
    if (NULL != pmix_sec.create_cred) {
        if (NULL == (cred = pmix_sec.create_cred())) {
            /* an error occurred - we cannot continue */
            return PMIX_ERR_INVALID_CRED;
        }
        csize = strlen(cred) + 1;  // must NULL terminate the string!
    }
    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = sdsize + strlen(PMIX_VERSION) + 1 + csize;  // must NULL terminate the VERSION string!

    /* create a space for our message */
    sdsize = (sizeof(hdr) + hdr.nbytes);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        if (NULL != cred) {
            free(cred);
        }
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    csize=0;
    memcpy(msg, &hdr, sizeof(pmix_usock_hdr_t));
    csize += sizeof(pmix_usock_hdr_t);
    memcpy(msg+csize, pmix_globals.nspace, strlen(pmix_globals.nspace));
    csize += strlen(pmix_globals.nspace)+1;
    memcpy(msg+csize, &pmix_globals.rank, sizeof(int));
    csize += sizeof(int);
    memcpy(msg+csize, PMIX_VERSION, strlen(PMIX_VERSION));
    csize += strlen(PMIX_VERSION)+1;
    if (NULL != cred) {
        memcpy(msg+csize, cred, strlen(cred));  // leaves last position in msg set to NULL
    }
    
    if (PMIX_SUCCESS != pmix_usock_send_blocking(sd, msg, sdsize)) {
        free(msg);
        if (NULL != cred) {
            free(cred);
        }
        return PMIX_ERR_UNREACH;
    }
    free(msg);
    if (NULL != cred) {
        free(cred);
    }
    return PMIX_SUCCESS;
}

/* we receive a connection acknowledgement from the server,
 * consisting of nothing more than a status report. If success,
 * then we initiate authentication method */
static int recv_connect_ack(int sd)
{
    pmix_usock_hdr_t hdr;
    int32_t reply;
    int rc;
    int32_t cnt;
    char *msg = NULL;
    size_t i, ninfo;
    pmix_info_t *info;
    pmix_buffer_t buf;
    pmix_kval_t *kv;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT ACK FROM SERVER");
    /* receive the header */
    rc = pmix_usock_recv_blocking(sd, (char*)&hdr, sizeof(pmix_usock_hdr_t));
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* get whatever else was sent */
    msg = (char*)malloc(hdr.nbytes);
    if (PMIX_SUCCESS != (rc = pmix_usock_recv_blocking(sd, msg, hdr.nbytes))) {
        free(msg);
        return rc;
    }
    /* load the buffer for unpacking */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER(&buf, msg, hdr.nbytes);

    /* unpack the status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&buf, &reply, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* see if they want us to do the handshake */
    if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
        free(msg);
        msg = NULL;
        if (NULL == pmix_sec.client_handshake) {
            rc = PMIX_ERR_HANDSHAKE_FAILED;
            goto cleanup;
        }
        if (PMIX_SUCCESS != pmix_sec.client_handshake(sd)) {
            goto cleanup;
        }
        /* if we successfully did the handshake, there will be a follow-on
         * message that contains any job info */
        rc = pmix_usock_recv_blocking(sd, (char*)&hdr, sizeof(pmix_usock_hdr_t));
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* get whatever else was sent */
        msg = (char*)malloc(hdr.nbytes);
        if (PMIX_SUCCESS != (rc = pmix_usock_recv_blocking(sd, msg, hdr.nbytes))) {
            goto cleanup;
        }
        PMIX_DESTRUCT(&buf);
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_LOAD_BUFFER(&buf, msg, hdr.nbytes);
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&buf, &reply, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* see if we succeeded */
    if (PMIX_SUCCESS != reply) {
        rc = reply;
        goto cleanup;
    }
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT CONFIRMATION AND INITIAL DATA FROM SERVER OF %d BYTES",
                        (int)hdr.nbytes);
    
    /* unpack our index into the server's client array */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&buf, &pmix_globals.pindex, &cnt, PMIX_INT))) {
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            /* this isn't an error - the host must provide us
             * the localid */
            rc = PMIX_SUCCESS;
            goto cleanup;
        }
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&buf, &ninfo, &cnt, PMIX_SIZE))) {
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            /* this isn't an error - the host server may not
             * have provided any job-level info */
            rc = PMIX_SUCCESS;
            goto cleanup;
        }
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < ninfo) {
        info = (pmix_info_t*)malloc(ninfo * sizeof(pmix_info_t));
        cnt = ninfo;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&buf, info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            free(info);
            goto cleanup;
        }
        for (i=0; i < ninfo; i++) {
            kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(info[i].key);
            kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
            pmix_value_xfer(kv->value, &info[i].value);
            if (PMIX_SUCCESS != (rc = pmix_client_hash_store(pmix_globals.nspace,
                                                             pmix_globals.rank, kv))) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_RELEASE(kv); // maintain accounting
        }
        //free(info);
    }

 cleanup:
    buf.base_ptr = NULL;  // protect data region from double-free
    PMIX_DESTRUCT(&buf);
    free(msg);
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
        if (0 <= sd) {
            CLOSE_THE_SOCKET(sd);
        }
        return PMIX_ERR_UNREACH;
    }

    /* send our identity and any authentication credentials to the server */
    if (PMIX_SUCCESS != (rc = send_connect_ack(sd))) {
        CLOSE_THE_SOCKET(sd);
        return sd;
    }

    /* do whatever handshake is required */
    if (PMIX_SUCCESS != (rc = recv_connect_ack(sd))) {
        CLOSE_THE_SOCKET(sd);
        return sd;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sock_peer_try_connect: Connection across to server succeeded");

    pmix_usock_set_nonblocking(sd);
    return sd;
}

