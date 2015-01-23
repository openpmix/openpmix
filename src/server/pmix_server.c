/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix_server.h"
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
#include "src/util/pmix_environ.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_server_ops.h"

// local variables
static int init_cntr = 0;
static char *myuri = NULL;
static pmix_event_t listen_ev;
static bool listening = false;
static int mysocket = -1;
static struct sockaddr_un myaddress;
static bool using_internal_comm = false;
static char *security_mode = NULL;

// local functions
static int start_listening(struct sockaddr_un *address);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static int authenticate_client(int sd, pmix_peer_t **peer);
static void server_message_handler(int sd, pmix_usock_hdr_t *hdr,
                              pmix_buffer_t *buf, void *cbdata);
// global variables
pmix_globals_t pmix_globals = {
    .evbase = NULL,
    .debug_output = -1,
    .errhandler = NULL,
    .server = true,
};
pmix_server_globals_t pmix_server_globals;

/* queue a message to be sent to one of our procs - must
 * provide the following params:
 *
 * p - the peer object of the process
 * t - tag to be sent to
 * b - buffer to be sent
 */
#define PMIX_SERVER_QUEUE_REPLY(p, t, b)                                \
    do {                                                                \
        pmix_usock_send_t *snd;                                         \
        pmix_output_verbose(2, pmix_globals.debug_output,               \
                            "[%s:%d] queue reply to %s:%d on tag %d",   \
                            __FILE__, __LINE__,                         \
                            (p)->nspace, (p)->rank, (t));               \
        snd = OBJ_NEW(pmix_usock_send_t);                               \
        (void)strncpy(snd->hdr.nspace, pmix_globals.nspace, PMIX_MAX_NSLEN); \
        snd->hdr.rank = pmix_globals.rank;                              \
        snd->hdr.type = PMIX_USOCK_USER;                                \
        snd->hdr.tag = (t);                                             \
        snd->hdr.nbytes = (b)->bytes_used;                              \
        snd->data = (b);                                                \
        /* always start with the header */                              \
        snd->sdptr = (char*)&snd->hdr;                                  \
        snd->sdbytes = sizeof(pmix_usock_hdr_t);                        \
                                                                        \
        /* if there is no message on-deck, put this one there */        \
        if (NULL == (p)->send_msg) {                                    \
            (p)->send_msg = snd;                                        \
        } else {                                                        \
            /* add it to the queue */                                   \
            pmix_list_append(&(p)->send_queue, &snd->super);            \
        }                                                               \
        /* ensure the send event is active */                           \
        if (!(p)->send_ev_active) {                                     \
            event_add(&(p)->send_event, 0);                             \
            (p)->send_ev_active = true;                                 \
        }                                                               \
    } while(0);

static int initialize_server_base(pmix_server_module_t *module)
{
    int debug_level;
    pid_t pid;
    char *tdir, *evar;

    /* initialize the output system */
    if (!pmix_output_init()) {
        return -1;
    }
    /* Zero globals */
    memset(&pmix_globals, 0, sizeof(pmix_globals));

    /* setup the globals */

    (void)strncpy(pmix_globals.nspace, "pmix-server", PMIX_MAX_NSLEN);
    pmix_globals.debug_output = -1;
    OBJ_CONSTRUCT(&pmix_server_globals.peers, pmix_list_t);
    OBJ_CONSTRUCT(&pmix_server_globals.fence_ops, pmix_list_t);
    OBJ_CONSTRUCT(&pmix_server_globals.connect_ops, pmix_list_t);
    OBJ_CONSTRUCT(&pmix_server_globals.disconnect_ops, pmix_list_t);

    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        debug_level = strtol(evar, NULL, 10);
        pmix_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_globals.debug_output, debug_level);
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    /* setup the function pointers */
    memset(&pmix_host_server, 0, sizeof(pmix_server_module_t));
    pmix_host_server = *module;

    /* initialize the datatype support */
    pmix_bfrop_open();

    /* init security */
    pmix_sec_init();
    security_mode = strdup(pmix_sec.name);
    
    /* setup the path to the daemon rendezvous point, using our
     * pid as the "rank" */
    pid = getpid();

    /* find the temp dir */
    if (NULL == (tdir = getenv("TMPDIR"))) {
        if (NULL == (tdir = getenv("TEMP"))) {
            if (NULL == (tdir = getenv("TMP"))) {
                tdir = "/tmp";
            }
        }
    }
    /* now set the address */
    memset(&myaddress, 0, sizeof(struct sockaddr_un));
    myaddress.sun_family = AF_UNIX;
    snprintf(myaddress.sun_path, sizeof(myaddress.sun_path)-1, "%s/pmix-%d", tdir, pid);
    asprintf(&myuri, "%lu:%s", (unsigned long)pid, myaddress.sun_path);


    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server constructed uri %s", myuri);
    return 0;
}

int PMIx_server_init(pmix_server_module_t *module,
                     int use_internal_comm)
{
    pmix_usock_posted_recv_t *req;
    int rc;
    
    ++init_cntr;
    if (1 < init_cntr) {
        return 0;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    /* save the comm flag for when we finalize */
    using_internal_comm = use_internal_comm;
    
    if (0 != (rc = initialize_server_base(module))) {
        return rc;
    }

    /* if the caller doesn't want us to use our own internal
     * messaging system, then we are done */
    if (!use_internal_comm) {
        return 0;
    }
    
    /* and the usock system */
    pmix_usock_init();
    
    /* create an event base and progress thread for us */
    if (NULL == (pmix_globals.evbase = pmix_start_progress_thread())) {
        return -1;
    }

    /* setup the wildcard recv for inbound messages from clients */
    req = OBJ_NEW(pmix_usock_posted_recv_t);
    req->tag = UINT32_MAX;
    req->cbfunc = server_message_handler;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_usock_globals.posted_recvs, &req->super);

    /* start listening */
    if (0 != start_listening(&myaddress)) {
        PMIx_server_finalize();
        return -1;
    }

    return 0;
}

struct sockaddr_un PMIx_get_addr(void)
{
    return myaddress;
}

static void cleanup_server_state(void)
{
    PMIX_LIST_DESTRUCT(&pmix_server_globals.peers);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.fence_ops);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.connect_ops);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.disconnect_ops);

    if (NULL != myuri) {
        free(myuri);
    }

    pmix_bfrop_close();
    pmix_sec_finalize();
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");

    pmix_output_close(pmix_globals.debug_output);
    pmix_output_finalize();
    pmix_class_finalize();
}

int PMIx_server_finalize(void)
{
    if (1 != init_cntr) {
        --init_cntr;
        return 0;
    }
    init_cntr = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize called");
    
    if (using_internal_comm) {
        if (listening) {
            event_del(&listen_ev);
        }
    
        pmix_stop_progress_thread(pmix_globals.evbase);
        event_base_free(pmix_globals.evbase);
#ifdef HAVE_LIBEVENT_SHUTDOWN
        libevent_global_shutdown();
#endif
        
        if (0 <= mysocket) {
            CLOSE_THE_SOCKET(mysocket);
        }

        pmix_usock_finalize();

        /* cleanup the rendezvous file */
        unlink(myaddress.sun_path);
    }
    
    cleanup_server_state();
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");
    return 0;
}

/* setup the envars for a child process */
int PMIx_server_setup_fork(const char nspace[], int rank,
                           uid_t uid, gid_t gid, char ***env)
{
    char rankstr[PMIX_MAX_VALLEN];
    pmix_peer_t *peer;
    
    /* pass the nspace */
    pmix_setenv("PMIX_NAMESPACE", nspace, true, env);
    /* pass the rank */
    (void)snprintf(rankstr, PMIX_MAX_VALLEN, "%d", rank);
    pmix_setenv("PMIX_RANK", rankstr, true, env);
    /* pass our rendezvous info */
    pmix_setenv("PMIX_SERVER_URI", myuri, true, env);
    /* pass our active security mode */
    pmix_setenv("PMIX_SECURITY_MODE", security_mode, true, env);
    
    /* setup a peer object for this client */
    peer = OBJ_NEW(pmix_peer_t);
    (void)strncpy(peer->nspace, nspace, PMIX_MAX_NSLEN);
    peer->rank = rank;
    peer->uid = uid;
    peer->gid = gid;
    pmix_list_append(&pmix_server_globals.peers, &peer->super);

    return 0;
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

void PMIx_free_value(pmix_value_t **val)
{
    if (NULL == val || NULL == *val) {
        return;
    }
    PMIx_free_value_data(*val);
    free(*val);
    *val = NULL;
}

void PMIx_Register_errhandler(pmix_notification_fn_t err)
{
    pmix_globals.errhandler = err;
}

void PMIx_Deregister_errhandler(void)
{
   pmix_globals.errhandler = NULL;
}

/*
 * start listening on our rendezvous file
 */
static int start_listening(struct sockaddr_un *address)
{
    int flags;
    unsigned int addrlen;

    /* create a listen socket for incoming connection attempts */
    mysocket = socket(PF_UNIX, SOCK_STREAM, 0);
    if (mysocket < 0) {
        printf("%s:%d socket() failed", __FILE__, __LINE__);
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(mysocket, (struct sockaddr*)address, addrlen) < 0) {
        printf("%s:%d bind() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* setup listen backlog to maximum allowed by kernel */
    if (listen(mysocket, SOMAXCONN) < 0) {
        printf("%s:%d listen() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(mysocket, F_GETFL, 0)) < 0) {
        printf("%s:%d fcntl(F_GETFL) failed", __FILE__, __LINE__);
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(mysocket, F_SETFL, flags) < 0) {
        printf("%s:%d fcntl(F_SETFL) failed", __FILE__, __LINE__);
        return -1;
    }

    /* setup to listen via the event lib */
    event_assign(&listen_ev, pmix_globals.evbase, mysocket,
                 EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(&listen_ev, 0);
    return 0;
}

static int send_client_response(int sd, int status, pmix_buffer_t *payload)
{
    int rc;
    pmix_usock_hdr_t hdr;
    
    hdr.nbytes = sizeof(int);
    if (NULL != payload) {
        hdr.nbytes += payload->bytes_used;
    }
    hdr.rank = pmix_globals.rank;
    hdr.type = PMIX_USOCK_IDENT;
    hdr.tag = 0; // tag doesn't matter as we aren't matching to a recv

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&hdr, sizeof(hdr)))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&status, sizeof(int)))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (NULL != payload) {
        if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)payload->base_ptr, payload->bytes_used))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    
    return PMIX_SUCCESS;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incoming_sd, short flags, void* cbdata)
{
    int sd;
    pmix_peer_t *peer;
    
    if( 0 > (sd = accept(incoming_sd,NULL,0)) ){
        printf("accept() failed");
        exit(0);
    }

    /* receive identifier info from the client and authenticate it - the
     * function will lookup and return the peer object if the connection
     * is successfully authenticated */
    if (PMIX_SUCCESS != authenticate_client(sd, &peer)) {
        CLOSE_THE_SOCKET(sd);
        return;
    }
    pmix_usock_set_nonblocking(sd);
    peer->sd = sd;

    /* start the events for this client */
    event_assign(&peer->recv_event, pmix_globals.evbase, sd,
                 EV_READ|EV_PERSIST, pmix_usock_recv_handler, peer);
    event_add(&peer->recv_event, NULL);
    peer->recv_ev_active = true;
    event_assign(&peer->send_event, pmix_globals.evbase, sd,
                 EV_WRITE|EV_PERSIST, pmix_usock_send_handler, peer);
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server client %s:%d has connected on socket %d",
                        peer->nspace, peer->rank, peer->sd);
}

/*  Receive the peer's identification info from a newly
 *  connected socket and verify the expected response.
 */
static int authenticate_client(int sd, pmix_peer_t **peer)
{
    char *msg, *version, *cred;
    int rc;
    pmix_usock_hdr_t hdr;
    pmix_peer_t *pr, *psave = NULL;
    size_t csize;
    pmix_buffer_t buf;
    pmix_info_t *info;
    size_t i, ninfo;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "RECV CONNECT ACK FROM PEER ON SOCKET %d", sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));
    *peer = NULL;
    
    if (PMIX_SUCCESS != (rc = pmix_usock_recv_blocking(sd, (char*)&hdr, sizeof(pmix_usock_hdr_t)))) {
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack recvd from peer %s:%d",
                        hdr.nspace, hdr.rank);

    /* get the authentication and version payload (and possibly
     * security credential) - to guard against potential attacks,
     * we'll set an arbitrary limit per a define */
    if (PMIX_MAX_CRED_SIZE < hdr.nbytes) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    if (PMIX_SUCCESS != pmix_usock_recv_blocking(sd, msg, hdr.nbytes)) {
        /* unable to complete the recv */
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "unable to complete recv of connect-ack with client ON SOCKET %d", sd);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    /* check that this is from a matching version */
    version = (char*)(msg);
    if (0 != strcmp(version, PMIX_VERSION)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:server client/server PMIx versions mismatch");
        free(msg);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack version from client matches ours");

    /* see if we have this peer in our list */
    PMIX_LIST_FOREACH(pr, &pmix_server_globals.peers, pmix_peer_t) {
        if (0 == strcmp(pr->nspace, hdr.nspace) &&
            pr->rank == hdr.rank) {
            psave = pr;
            if (pr->sd < 0) {
                *peer = pr;
                pr->sd = sd;
                break;
            }
        }
    }
    if (NULL == psave) {
        /* we don't know this peer, reject it */
        free(msg);
        return PMIX_ERR_NOT_FOUND;
    }
    /* a peer can connect on multiple sockets
     * since it can fork/exec a child that also calls PMIx_Init, so
     * add it here if necessary */
    if (NULL == *peer) {
        /* need to create another tracker for this peer */
        *peer = OBJ_NEW(pmix_peer_t);
        (void)strncpy((*peer)->nspace, psave->nspace, PMIX_MAX_NSLEN);
        (*peer)->rank = psave->rank;
        (*peer)->sd = sd;
        pmix_list_append(&pmix_server_globals.peers, &(*peer)->super);
    }
    psave = *peer;
    
    /* see if there is a credential */
    csize = strlen(version) + 1;
    if (csize < hdr.nbytes) {
        cred = (char*)(msg + csize);
        if (NULL != cred && NULL != pmix_sec.validate_cred) {
            if (PMIX_SUCCESS != (rc = pmix_sec.validate_cred(psave, cred))) {
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "validation of client credential failed");
                free(msg);
                pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
                OBJ_RELEASE(*peer);
                return rc;
            }
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "client credential validated");
        }
    }
    free(msg);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack version from client matches ours");

    /* execute the handshake if the security mode calls for it */
    if (NULL != pmix_sec.server_handshake) {
        if (PMIX_SUCCESS != send_client_response(sd, PMIX_ERR_READY_FOR_HANDSHAKE, NULL)) {
            pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
            OBJ_RELEASE(psave);
            return PMIX_ERR_UNREACH;
        }
        if (PMIX_SUCCESS != pmix_sec.server_handshake(psave)) {
            pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
            OBJ_RELEASE(psave);
            return PMIX_ERR_UNREACH;
        }
    }
    
    /* get any job info the host server might have for this client */
    if (NULL != pmix_host_server.get_job_info) {
        rc = pmix_host_server.get_job_info(psave->nspace, psave->rank,
                                           &info, &ninfo);
        /* pack any returned info */
        if (0 == rc && NULL != info && 0 < ninfo) {
            OBJ_CONSTRUCT(&buf, pmix_buffer_t);
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, &ninfo, 1, PMIX_SIZE))) {
                PMIX_ERROR_LOG(rc);
                OBJ_DESTRUCT(&buf);
                pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
                OBJ_RELEASE(psave);
                return rc;
            }
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, info, ninfo, PMIX_INFO))) {
                PMIX_ERROR_LOG(rc);
                OBJ_DESTRUCT(&buf);
                pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
                OBJ_RELEASE(psave);
                return rc;
            }
            for (i=0; i < ninfo; i++) {
                PMIx_free_value_data(&info[i].value);
            }
            free(info);
            /* let the client know we are ready to go */
            if (PMIX_SUCCESS != (rc = send_client_response(sd, PMIX_SUCCESS, &buf))) {
                pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
                OBJ_RELEASE(psave);
            }
            return rc;
        }
    }
    
    /* let the client know we are ready to go */
    if (PMIX_SUCCESS != (rc = send_client_response(sd, PMIX_SUCCESS, NULL))) {
        pmix_list_remove_item(&pmix_server_globals.peers, &psave->super);
        OBJ_RELEASE(psave);
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack from client %s",
                        (PMIX_SUCCESS == rc) ? "completed" : "failed");

    return rc;
}

static void op_cbfunc(int status, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* send a copy to the originator */
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    /* cleanup */
    OBJ_RELEASE(cd);
}

static void spawn_cbfunc(int status, char *nspace, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* add the nspace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    /* cleanup */
    OBJ_RELEASE(cd);
}

static void lookup_cbfunc(int status, pmix_info_t info[], size_t ninfo,
                          char nspace[], void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* pack the returned nspace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }

    /* pack the returned info objects */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, info, ninfo, PMIX_INFO))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }

    /* send to the originator */
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    /* cleanup */
    OBJ_RELEASE(cd);
}

static void modex_cbfunc(int status, pmix_modex_data_t data[],
                         size_t ndata, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    pmix_server_caddy_t *cd;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:modex_cbfunc called with %d elements", (int)ndata);

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* pack the nblobs being returned */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ndata, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    if (0 < ndata) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, data, ndata, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
    }
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        OBJ_RETAIN(reply);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "server:modex_cbfunc reply being sent to %s:%d",
                            cd->peer->nspace, cd->peer->rank);
       PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    }
    OBJ_RELEASE(reply);  // maintain accounting
    pmix_list_remove_item(tracker->trklist, &tracker->super);
    OBJ_RELEASE(tracker);
}

static void get_cbfunc(int status, pmix_modex_data_t data[],
                       size_t ndata, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:modex_cbfunc called with %d elements", (int)ndata);

    if (NULL == cd) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* pack the nblobs being returned */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ndata, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < ndata) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, data, ndata, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
        goto cleanup;
        }
    }
    /* send the data to the requestor */
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:get_cbfunc reply being sent to %s:%d",
                        cd->peer->nspace, cd->peer->rank);
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);

 cleanup:
    OBJ_RELEASE(cd);
}

static void cnct_cbfunc(int status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    pmix_server_caddy_t *cd;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:cnct_cbfunc called");

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        OBJ_RETAIN(reply);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "server:cnct_cbfunc reply being sent to %s:%d",
                            cd->peer->nspace, cd->peer->rank);
       PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    }
    OBJ_RELEASE(reply);  // maintain accounting
    pmix_list_remove_item(tracker->trklist, &tracker->super);
    OBJ_RELEASE(tracker);
}


/* the switchyard is the primary message handling function. It's purpose
 * is to take incoming commands (packed into a buffer), unpack them,
 * and then call the corresponding host server's function to execute
 * them. Some commands involve only a single proc (i.e., the one
 * sending the command) and can be executed while we wait. In these cases,
 * the switchyard will construct and pack a reply buffer to be returned
 * to the sender.
 *
 * Other cases (either multi-process collective or cmds that require
 * an async reply) cannot generate an immediate reply. In these cases,
 * the reply buffer will be NULL. An appropriate callback function will
 * be called that will be responsible for eventually replying to the
 * calling processes.
 *
 * Should an error be encountered at any time within the switchyard, an
 * error reply buffer will be returned so that the caller can be notified,
 * thereby preventing the process from hanging. */ 
static int server_switchyard(pmix_peer_t *peer, uint32_t tag,
                             pmix_buffer_t *buf)
{
    int rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_server_caddy_t *cd;
    
    /* retrieve the cmd */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd pmix cmd %d from %s:%d",
                        cmd, peer->nspace, peer->rank);

    if (PMIX_ABORT_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag); 
        if (PMIX_SUCCESS != (rc = pmix_server_abort(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }
        
    if (PMIX_FENCENB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_fence(cd, buf, modex_cbfunc, op_cbfunc))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PMIX_GETNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_get(buf, get_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }
        
    if (PMIX_FINALIZE_CMD == cmd) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FINALIZE");
        /* call the local server, if supported */
        rc = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != pmix_host_server.terminated) {
            rc = pmix_host_server.terminated(peer->nspace, peer->rank);
        }
        PMIX_PEER_CADDY(cd, peer, tag);
        op_cbfunc(rc, cd);
        /* turn off the recv event */
        if (peer->recv_ev_active) {
            event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        return rc;
    }

        
    if (PMIX_PUBLISHNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_publish(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

    
    if (PMIX_LOOKUPNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_lookup(buf, lookup_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

        
    if (PMIX_UNPUBLISHNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_unpublish(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

        
    if (PMIX_SPAWNNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_spawn(buf, spawn_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

        
    if (PMIX_CONNECTNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, false, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

    if (PMIX_DISCONNECTNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, true, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

static void server_message_handler(int sd, pmix_usock_hdr_t *hdr,
                                   pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer, *pr;
    pmix_buffer_t *reply;
    int rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "SWITCHYARD for %s:%d:%d", hdr->nspace, hdr->rank, sd);

    /* find the peer object */
    peer = NULL;
    PMIX_LIST_FOREACH(pr, &pmix_server_globals.peers, pmix_peer_t) {
        if (0 == strcmp(hdr->nspace, pr->nspace) &&
            hdr->rank == pr->rank &&
            sd == pr->sd) {
            peer = pr;
            break;
        }
    }
    if (NULL == peer) {
        /* should be impossible as the connection
         * was validated */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        return;
    }

    rc = server_switchyard(peer, hdr->tag, buf);
    /* send the return, if there was an error returned */
    if (PMIX_SUCCESS != rc) {
        reply = OBJ_NEW(pmix_buffer_t);
        pmix_bfrop.pack(reply, &rc, 1, PMIX_INT);
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        OBJ_RELEASE(reply);
    }
}
