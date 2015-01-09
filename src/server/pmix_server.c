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
#include "pmix_message.h"

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

// local classes
typedef struct {
    pmix_list_item_t super;
    pmix_range_t *ranges;
    size_t nranges;
    pmix_list_t locals;
    pmix_list_t *trkr;
} pmix_server_trkr_t;
static void tcon(pmix_server_trkr_t *t)
{
    t->ranges = NULL;
    t->nranges = 0;
    OBJ_CONSTRUCT(&t->locals, pmix_list_t);
    t->trkr = NULL;
}
static void tdes(pmix_server_trkr_t *t)
{
    size_t i;
    
    if (NULL != t->ranges) {
        for (i=0; i < t->nranges; i++) {
            if (NULL != t->ranges[i].ranks) {
                free(t->ranges[i].ranks);
            }
        }
        free(t->ranges);
    }
    PMIX_LIST_DESTRUCT(&t->locals);
}
static OBJ_CLASS_INSTANCE(pmix_server_trkr_t,
                          pmix_list_item_t,
                          tcon, tdes);

typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *peer;
    uint32_t tag;
} pmix_server_caddy_t;
static void cdcon(pmix_server_caddy_t *cd)
{
    cd->peer = NULL;
}
static void cddes(pmix_server_caddy_t *cd)
{
    if (NULL != cd->peer) {
        OBJ_RELEASE(cd->peer);
    }
}
static OBJ_CLASS_INSTANCE(pmix_server_caddy_t,
                          pmix_list_item_t,
                          cdcon, cddes);

// local variables
static int init_cntr = 0;
static pmix_server_module_t server;
static char *myuri = NULL;
static pmix_event_t listen_ev;
static bool listening = false;
static bool local_evbase = false;
static int mysocket = -1;
static pmix_list_t peers;
static struct sockaddr_un myaddress;
static pmix_list_t fences, gets;
static pmix_list_t connects, disconnects;
static pmix_list_t spawns;

// local functions
static int start_listening(struct sockaddr_un *address);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static int authenticate_client(int sd, pmix_peer_t **peer);
static int send_client_response(int sd, int status);
static void server_switchyard(int sd, pmix_usock_hdr_t *hdr,
                              pmix_buffer_t *buf, void *cbdata);
// global variables
pmix_globals_t pmix_globals;

/* queue a message to be sent to one of our procs - must
 * provide the following params:
 *
 * p - the peer object of the process
 * t - tag to be sent to
 * b - buffer to be sent
 */
#define PMIX_SERVER_QUEUE_REPLY(p, t, b)                                \
    do {                                                                \
    pmix_usock_send_t *snd;                                             \
    pmix_output_verbose(2, pmix_globals.debug_output,                   \
                        "[%s:%d] queue reply to %s:%d on tag %d",       \
                        __FILE__, __LINE__,                             \
                        (p)->namespace, (p)->rank, (t));                \
    snd = OBJ_NEW(pmix_usock_send_t);                                   \
    (void)strncpy(snd->hdr.namespace, pmix_globals.namespace, PMIX_MAX_NSLEN); \
    snd->hdr.rank = pmix_globals.rank;                                  \
    snd->hdr.type = PMIX_USOCK_USER;                                    \
    snd->hdr.tag = (t);                                                 \
    snd->hdr.nbytes = (b)->bytes_used;                                  \
    snd->data = (b);                                                    \
    /* always start with the header */                                  \
    snd->sdptr = (char*)&snd->hdr;                                      \
    snd->sdbytes = sizeof(pmix_usock_hdr_t);                            \
                                                                        \
    /* if there is no message on-deck, put this one there */            \
    if (NULL == (p)->send_msg) {                                        \
        (p)->send_msg = snd;                                            \
    } else {                                                            \
        /* add it to the queue */                                       \
        pmix_list_append(&(p)->send_queue, &snd->super);                \
    }                                                                   \
    /* ensure the send event is active */                               \
    if (!(p)->send_ev_active) {                                         \
        event_add(&(p)->send_event, 0);                                 \
        (p)->send_ev_active = true;                                     \
    }                                                                   \
    }while(0);

static int initialize_server_base(pmix_server_module_t *module, char *tmpdir,
                                   char *credential)
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

    (void)strncpy(pmix_globals.namespace, "pmix-server", PMIX_MAX_NSLEN);
    pmix_globals.debug_output = -1;
    OBJ_CONSTRUCT(&peers, pmix_list_t);
    OBJ_CONSTRUCT(&fences, pmix_list_t);
    OBJ_CONSTRUCT(&gets, pmix_list_t);
    OBJ_CONSTRUCT(&connects, pmix_list_t);
    OBJ_CONSTRUCT(&disconnects, pmix_list_t);
    OBJ_CONSTRUCT(&spawns, pmix_list_t);

    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        debug_level = strtol(evar, NULL, 10);
        pmix_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_globals.debug_output, debug_level);
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    /* setup the function pointers */
    memset(&server, 0, sizeof(pmix_server_module_t));
    server = *module;
    /* save the credential, if provided */
    if (NULL != credential) {
        pmix_globals.credential = strdup(credential);
    }

    /* initialize the datatype support */
    pmix_bfrop_open();

    /* setup the path to the daemon rendezvous point, using our
     * pid as the "rank" */
    pid = getpid();

    /* find the temp dir, if not given */
    if (NULL != tmpdir) {
        tdir = tmpdir;
    } else if (NULL == (tdir = getenv("TMPDIR"))) {
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

int PMIx_server_init_light(pmix_server_module_t *module, char *tmpdir,
                           char *credential)
{
    ++init_cntr;
    if (1 < init_cntr) {
        return 0;
    }
    return initialize_server_base(module, tmpdir, credential);
}

int PMIx_server_init(pmix_server_module_t *module,
                     struct event_base *evbase,
                     char *tmpdir, char *credential)
{
    pmix_usock_posted_recv_t *req;
    int rc;
    
    ++init_cntr;
    if (1 < init_cntr) {
        return 0;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    rc = initialize_server_base(module, tmpdir, credential);
    if( rc ){
        return rc;
    }

    /* and the usock system */
    pmix_usock_init();
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server constructed uri %s", myuri);

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

    /* setup the wildcard recv for inbound messages from clients */
    req = OBJ_NEW(pmix_usock_posted_recv_t);
    req->tag = UINT32_MAX;
    req->cbfunc = server_switchyard;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_usock_globals.posted_recvs, &req->super);

    /* start listening */
    if (0 != start_listening(&myaddress)) {
        PMIx_server_finalize();
        return -1;
    }

    return 0;
}

static void cleanup_server_state(void)
{
    PMIX_LIST_DESTRUCT(&peers);
    PMIX_LIST_DESTRUCT(&fences);
    PMIX_LIST_DESTRUCT(&gets);
    PMIX_LIST_DESTRUCT(&connects);
    PMIX_LIST_DESTRUCT(&disconnects);
    PMIX_LIST_DESTRUCT(&spawns);

    if (NULL != pmix_globals.credential) {
        free(pmix_globals.credential);
    }
    if (NULL != myuri) {
        free(myuri);
    }

    pmix_bfrop_close();

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");

    pmix_output_close(pmix_globals.debug_output);
    pmix_output_finalize();
    pmix_class_finalize();
}

int PMIx_server_finalize_light(void)
{
    if (1 != init_cntr) {
        --init_cntr;
       return 0;
    }
    init_cntr = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize called");
    cleanup_server_state();
    return 0;
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
    if (listening) {
        event_del(&listen_ev);
    }
    
    if (local_evbase) {
        pmix_stop_progress_thread(pmix_globals.evbase);
        event_base_free(pmix_globals.evbase);
#ifdef HAVE_LIBEVENT_SHUTDOWN
        libevent_global_shutdown();
#endif
    }

    if (0 <= mysocket) {
        CLOSE_THE_SOCKET(mysocket);
    }

    pmix_usock_finalize();

    /* cleanup the rendezvous file */
    unlink(myaddress.sun_path);
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");

    cleanup_server_state();
    return 0;
}

/* setup the envars for a child process */
int PMIx_server_setup_fork(const char namespace[], int rank, char ***env)
{
    char rankstr[PMIX_MAX_VALLEN];
    pmix_peer_t *peer;
    
    /* pass the namespace */
    pmix_setenv("PMIX_NAMESPACE", namespace, true, env);
    /* pass the rank */
    (void)snprintf(rankstr, PMIX_MAX_VALLEN, "%d", rank);
    pmix_setenv("PMIX_RANK", rankstr, true, env);
    /* pass our rendezvous info */
    pmix_setenv("PMIX_SERVER_URI", myuri, true, env);
    /* pass our security credential, if one was given */
    if (NULL != pmix_globals.credential) {
        pmix_setenv("PMIX_SERVER_CREDENTIAL", pmix_globals.credential, true, env);
    }
    /* setup a peer object for this client */
    peer = OBJ_NEW(pmix_peer_t);
    (void)strncpy(peer->namespace, namespace, PMIX_MAX_NSLEN);
    peer->rank = rank;
    pmix_list_append(&peers, &peer->super);
    
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

void PMIx_Register_errhandler(pmix_errhandler_fn_t err)
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

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incoming_sd, short flags, void* cbdata)
{
    int rc, sd;
    pmix_peer_t *peer;
    
    if( 0 > (sd = accept(incoming_sd,NULL,0)) ){
        printf("accept() failed");
        exit(0);
    }

    /* receive identifier info from the client and authenticate it - the
     * function will lookup and return the peer object */
    if (PMIX_SUCCESS != (rc = authenticate_client(sd, &peer))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:server client connection failed to authenticate");
        /* let the client know */
        send_client_response(sd, rc);
        return;
    }

    /* let the client know the connection was accepted */
    if (PMIX_SUCCESS != (rc = send_client_response(sd, PMIX_SUCCESS))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:server cannot confirm connection");
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
                        peer->namespace, peer->rank, peer->sd);
}

/* initialize pmix_peer_cred_t structure with pointers to
 * the message and header. Note: you don't need to free anything in
 * pmix_peer_cred_t. */
static int load_peer_cred(pmix_peer_cred_t *cred, pmix_usock_hdr_t hdr, char *msg)
{
    char *version;
    cred->namespace = hdr.namespace;
    cred->rank = hdr.rank;

    /* check that this is from a matching version */
    version = (char*)(msg);
    if (0 != strcmp(version, PMIX_VERSION)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:server client/server PMIx versions mismatch");
        return PMIX_ERR_NOT_SUPPORTED;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack version from client matches ours");

    /* check security token */
    if (NULL != server.authenticate) {
        /* server desires authentication */
        if( hdr.nbytes <= strlen(version) + 1 ){
            /* client did not provide authentication */
            pmix_output(0, "usock_peer_recv_connect_ack: "
                        "client failed to provide required authentication token");
            return PMIX_ERR_INVALID_ARG;
        }
        cred->auth_token = (char*)(msg + strlen(version) + 1);
    } else {
        cred->auth_token = NULL;
    }
    return PMIX_SUCCESS;
}

/*  Receive the peer's identification info from a newly
 *  connected socket and verify the expected response.
 */
static int authenticate_client(int sd, pmix_peer_t **peer)
{
    char *msg;
    int rc;
    pmix_usock_hdr_t hdr;
    pmix_peer_t *pr;
    pmix_peer_cred_t cred;
    bool found;
    
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
                        hdr.namespace, hdr.rank);

    /* get the authentication and version payload
     * artpol: put some sane limits here to prevent
     * attack, 1MB? */
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

    if( PMIX_SUCCESS != (rc = load_peer_cred(&cred, hdr, msg) ) ){
        free(msg);
        return rc;
    }

    /* see if we have this peer in our list */
    found = false;
    PMIX_LIST_FOREACH(pr, &peers, pmix_peer_t) {
        if (0 == strcmp(pr->namespace, cred.namespace) &&
            pr->rank == cred.rank) {
            found = true;
            if (pr->sd < 0) {
                *peer = pr;
                break;
            }
        }
    }
    if (!found) {
        /* we don't know this peer, reject it */
        return PMIX_ERR_NOT_FOUND;
    }
    /* a peer can connect on multiple sockets since it can
     * fork/exec a child that also calls PMIx_Init. */
    if (NULL == *peer) {
        /* need to add another tracker for this peer */
        *peer = OBJ_NEW(pmix_peer_t);
        (void)strncpy((*peer)->namespace, cred.namespace, PMIX_MAX_NSLEN);
        (*peer)->rank = cred.rank;
        pmix_list_append(&peers, &(*peer)->super);
    }
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack version from client matches ours");

    /* check security token */
    if (NULL != server.authenticate) {
        if (0 != server.authenticate(cred.auth_token)) {
            /* reject the connection */
            free(msg);
            return PMIX_ERR_UNREACH;
        }
    }
    free(msg);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack from client authenticated");

    return PMIX_SUCCESS;
}

static int send_client_response(int sd, int status)
{
    int rc;

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&status, sizeof(int)))) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_server_trkr_t* get_tracker(pmix_list_t *trks,
                                       pmix_range_t *ranges,
                                       size_t nranges)
{
    pmix_server_trkr_t *trk;
    size_t i, j;
    bool match;

    PMIX_LIST_FOREACH(trk, trks, pmix_server_trkr_t) {
        if (trk->nranges != nranges) {
            continue;
        }
        match = true;
        for (i=0; match && i < nranges; i++) {
            if (0 != strcmp(ranges[i].namespace, trk->ranges[i].namespace)) {
                match = false;
                break;
            }
            if (ranges[i].nranks != trk->ranges[i].nranks) {
                match = false;
                break;
            }
            if (NULL == ranges[i].ranks && NULL == trk->ranges[i].ranks) {
                /* this range matches */
                break;
            }
            for (j=0; j < ranges[i].nranks; j++) {
                if (ranges[i].ranks[j] != trk->ranges[i].ranks[j]) {
                    match = false;
                    break;
                }
            }
        }
        if (match) {
            return trk;
        }
    }
    /* get here if this tracker is new - create it */
    trk = OBJ_NEW(pmix_server_trkr_t);
    /* copy the ranges */
    trk->nranges = nranges;
    trk->ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
    for (i=0; i < nranges; i++) {
        memset(&trk->ranges[i], 0, sizeof(pmix_range_t));
        (void)strncpy(trk->ranges[i].namespace, ranges[i].namespace, PMIX_MAX_NSLEN);
        trk->ranges[i].nranks = ranges[i].nranks;
        trk->ranges[i].ranks = NULL;
        if (NULL != ranges[i].ranks) {
            trk->ranges[i].ranks = (int*)malloc(ranges[i].nranks * sizeof(int));
            for (j=0; j < ranges[i].nranks; j++) {
                trk->ranges[i].ranks[j] = ranges[i].ranks[j];
            }
        }
    }
    trk->trkr = trks;
    pmix_list_append(trks, &trk->super);
    return trk;
}

static void server_release(int status, pmix_modex_data_t data[],
                          size_t ndata, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    pmix_server_caddy_t *cd;
    size_t i;

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
        for (i=0; i < ndata; i++) {
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &data[i], 1, PMIX_MODEX))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                return;
            }
        }
    }
    /* send a copy to every member of the tracker */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        OBJ_RETAIN(reply);
        PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->tag, reply);
    }
    /* maintain reference count */
    OBJ_RELEASE(reply);
    /* cleanup the tracker */
    pmix_list_remove_item(tracker->trkr, &tracker->super);
    OBJ_RELEASE(tracker);
}

static void connect_release(int status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    pmix_server_caddy_t *cd;

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* send a copy to every member of the tracker */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        OBJ_RETAIN(reply);
        PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->tag, reply);
    }
    /* maintain reference count */
    OBJ_RELEASE(reply);
    /* cleanup the tracker */
    pmix_list_remove_item(tracker->trkr, &tracker->super);
    OBJ_RELEASE(tracker);
}

static void spawn_release(int status, char *namespace, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    pmix_server_caddy_t *cd;

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* add the namespace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &namespace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* send a copy to every member of the tracker */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        OBJ_RETAIN(reply);
        PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->tag, reply);
    }
    /* maintain reference count */
    OBJ_RELEASE(reply);
    /* cleanup the tracker */
    pmix_list_remove_item(&spawns, &tracker->super);
    OBJ_RELEASE(tracker);
}

static void server_switchyard(int sd, pmix_usock_hdr_t *hdr,
                              pmix_buffer_t *buf, void *cbdata)
{
    int rc, status, ret, barrier, collect_data;
    int32_t cnt;
    pmix_cmd_t cmd;
    char *msg;
    pmix_peer_t *peer, *pr;
    pmix_buffer_t *reply, *bptr;
    pmix_range_t *ranges, *rngptr, range;
    size_t i, nranges, ninfo;
    pmix_scope_t scope;
    char *nspace;
    int rnk;
    pmix_modex_data_t mdx;
    pmix_server_trkr_t *trk;
    pmix_server_caddy_t *cd;
    pmix_info_t *info, *iptr;
    char **keys;
    pmix_value_t *vptr;
    pmix_app_t *apps, *aptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "SWITCHYARD for %s:%d:%d", hdr->namespace, hdr->rank, sd);
    
    /* find the peer object */
    peer = NULL;
    PMIX_LIST_FOREACH(pr, &peers, pmix_peer_t) {
        if (0 == strcmp(hdr->namespace, pr->namespace) &&
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
    
    /* retrieve the cmd */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd pmix cmd %d from %s:%d",
                        cmd, hdr->namespace, hdr->rank);
    
    switch(cmd) {
    case PMIX_ABORT_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd ABORT");
        /* unpack the status */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &status, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the message */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &msg, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* let the local host's server execute it */
        if (NULL != server.abort) {
            ret = server.abort(status, msg);
        } else {
            ret = PMIX_ERR_NOT_SUPPORTED;
        }
        if (NULL != msg) {
            free(msg);
        }
        /* send the reply */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_FENCE_CMD:
    case PMIX_FENCENB_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd %s", (PMIX_FENCE_CMD == cmd) ? "FENCE" : "FENCENB");
        /* unpack the number of ranges */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd %s with %d ranges",
                            (PMIX_FENCE_CMD == cmd) ? "FENCE" : "FENCENB", (int)nranges);
        ranges = NULL;
        /* unpack the ranges, if provided */
        if (0 < nranges) {
            /* allocate reqd space */
            ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
            /* unpack the ranges */
            cnt = nranges;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, ranges, &cnt, PMIX_RANGE))) {
                PMIX_ERROR_LOG(rc);
                return;
            }
        }
        /* unpack the data flag - indicates if the caller wants
         * all modex data returned at end of procedure */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &collect_data, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack an additional flag indicating if we are to callback
         * once all procs have executed the fence_nb call, or
         * callback immediately */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &barrier, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack any provided data blobs */
        cnt = 1;
        (void)strncpy(mdx.namespace, peer->namespace, PMIX_MAX_NSLEN);
        mdx.rank = peer->rank;
        while (PMIX_SUCCESS == pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE)) {
            cnt = 1;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &bptr, &cnt, PMIX_BUFFER))) {
                PMIX_ERROR_LOG(rc);
                continue;
            }
            /* let the local host's server store it */
            mdx.blob = (uint8_t*)bptr->base_ptr;
            mdx.size = bptr->bytes_used;
            if (NULL != server.store_modex) {
                server.store_modex(scope, &mdx);
            }
            OBJ_RELEASE(bptr);
            cnt = 1;
        }
        if (PMIX_FENCE_CMD == cmd || 0 != barrier) {
            /* find/create the local tracker for this operation */
            trk = get_tracker(&fences, ranges, nranges);
            /* add this contributor to the tracker so they get
             * notified when we are done */
            cd = OBJ_NEW(pmix_server_caddy_t);
            OBJ_RETAIN(peer);
            cd->peer = peer;
            cd->tag = hdr->tag;
            pmix_list_append(&trk->locals, &cd->super);
            /* let the local host's server know that we are at the
             * "fence" point - they will callback once the barrier
             * across all participants has been completed */
            if (NULL != server.fence_nb) {
                ret = server.fence_nb(ranges, nranges, barrier,
                                      collect_data, server_release, trk);
            } else {
                /* need to send an "err_not_supported" status back
                 * to the client so they don't hang */
                reply = OBJ_NEW(pmix_buffer_t);
                ret = PMIX_ERR_NOT_SUPPORTED;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    return;
                }
                PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
            }
        } else {
            /* send an immediate release */
            reply = OBJ_NEW(pmix_buffer_t);
            ret = PMIX_SUCCESS;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                return;
            }
            PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        }
        if (NULL != ranges) {
            free(ranges);
        }
        break;

        
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FENCENB");
        break;

        
    case PMIX_GET_CMD:
    case PMIX_GETNB_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd %s", (PMIX_GET_CMD == cmd) ? "GET" : "GETNB");
        /* retrieve the namespace and rank of the requested proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nspace, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &rnk, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            free(nspace);
            return;
        }
        /* put it into range format */
        (void)strncpy(range.namespace, nspace, PMIX_MAX_NSLEN);
        range.ranks = (int*)malloc(sizeof(int));
        range.ranks[0] = rnk;
        range.nranks = 1;
        /* find/create the local tracker for this operation */
        trk = get_tracker(&gets, &range, 1);
        free(range.ranks);
        /* add this contributor to the tracker so they get
         * notified when we are done */
        cd = OBJ_NEW(pmix_server_caddy_t);
        OBJ_RETAIN(peer);
        cd->peer = peer;
        cd->tag = hdr->tag;
        pmix_list_append(&trk->locals, &cd->super);
        /* request the data, if supported */
        if (NULL != server.get_modex_nb) {
            ret = server.get_modex_nb(nspace, rnk, server_release, trk);
        } else {
            /* need to send an "err_not_supported" status back
             * to the client so they don't hang */
            reply = OBJ_NEW(pmix_buffer_t);
            ret = PMIX_ERR_NOT_SUPPORTED;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                free(nspace);
                return;
            }
            PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        }
        free(nspace);
        break;

        
    case PMIX_JOBINFO_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd JOBINFO");
        /* no further params are passed - just get the info
         * if available */
        ret = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != server.get_job_info) {
            ret = server.get_job_info(peer->namespace, peer->rank,
                                      &info, &ninfo);
        }
        /* send a release */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        /* add any returned info */
        if (NULL != info && 0 < ninfo) {
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ninfo, 1, PMIX_SIZE))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                return;
            }
            for (i=0; i < ninfo; i++) {
                iptr = &info[i];
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &iptr, 1, PMIX_INFO))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    return;
                }
            }
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;


    case PMIX_FINALIZE_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FINALIZE");
        /* call the local server, if supported */
        ret = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != server.terminated) {
            ret = server.terminated(peer->namespace, peer->rank);
        }
        /* turn off the recv event */
        if (peer->recv_ev_active) {
            event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        /* send a release */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_PUBLISH_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd PUBLISH");
        /* unpack the scope */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the number of info objects */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the array of info objects */
        if (0 < ninfo) {
            info = (pmix_info_t*)malloc(ninfo * sizeof(pmix_info_t));
            for (i=0; i < ninfo; i++) {
                cnt=1;
                iptr = &info[i];
                if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &iptr, &cnt, PMIX_INFO))) {
                    PMIX_ERROR_LOG(rc);
                    return;
                }
            }
        } else {
            info = NULL;
        }
        /* call the local server, if supported */
        ret = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != server.publish) {
            ret = server.publish(scope, info, ninfo);
        }
        /* send a release */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        if (NULL != info) {
            free(info);
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_LOOKUP_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd LOOKUP");
        /* unpack the scope */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the number of keys objects */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* setup the array of info objects */
        info = (pmix_info_t*)malloc(ninfo * sizeof(pmix_info_t));
        /* unpack the array of keys */
        for (i=0; i < ninfo; i++) {
            cnt=1;
            if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &msg, &cnt, PMIX_STRING))) {
                PMIX_ERROR_LOG(rc);
                return;
            }
            (void)strncpy(info[i].key, msg, PMIX_MAX_KEYLEN);
            free(msg);
        }
        /* call the local server, if supported */
        ret = PMIX_ERR_NOT_SUPPORTED;
        nspace = NULL;
        if (NULL != server.lookup) {
            ret = server.lookup(scope, info, ninfo, &nspace);
        }
        /* send a release */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        /* send the namespace */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nspace, 1, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        /* pack the results as key-values */
        for (i=0; i < ninfo; i++) {
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(buf, &info[i].key, 1, PMIX_STRING))) {
                PMIX_ERROR_LOG(rc);
                return;
            }
            vptr = &info[i].value;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(buf, &vptr, 1, PMIX_VALUE))) {
                PMIX_ERROR_LOG(rc);
                return;
            }
        }
        if (NULL != info) {
            free(info);
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_UNPUBLISH_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd UNPUBLISH");
        /* unpack the scope */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the number of keys */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the array of keys */
        keys = NULL;
        if (0 < ninfo) {
            for (i=0; i < ninfo; i++) {
                cnt=1;
                if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &msg, &cnt, PMIX_STRING))) {
                    PMIX_ERROR_LOG(rc);
                    return;
                }
                pmix_argv_append_nosize(&keys, msg);
                free(msg);
            }
        }
        /* call the local server, if supported */
        ret = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != server.unpublish) {
            ret = server.unpublish(scope, keys);
        }
        pmix_argv_free(keys);
        /* send a release */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_SPAWN_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd SPAWN");
        /* unpack the number of apps */
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* unpack the array of apps */
        apps = (pmix_app_t*)malloc(ninfo * sizeof(pmix_app_t));
        if (0 < ninfo) {
            for (i=0; i < ninfo; i++) {
                cnt=1;
                aptr = &apps[i];
                if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &aptr, &cnt, PMIX_APP))) {
                    PMIX_ERROR_LOG(rc);
                    return;
                }
            }
        }
        /* get/create a tracker for this operation so we don't block */
        (void)strncpy(range.namespace, peer->namespace, PMIX_MAX_NSLEN);
        range.ranks = (int*)malloc(sizeof(int));
        range.ranks[0] = peer->rank;
        range.nranks = 1;
        trk = get_tracker(&spawns, &range, 1);
        free(range.ranks);
        /* add this contributor to the tracker so they get
         * notified when we are done */
        cd = OBJ_NEW(pmix_server_caddy_t);
        OBJ_RETAIN(peer);
        cd->peer = peer;
        cd->tag = hdr->tag;
        pmix_list_append(&trk->locals, &cd->super);
        /* call the local server, if supported */
        ret = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != server.spawn) {
            ret = server.spawn(apps, ninfo, spawn_release, trk);
        }
        if (PMIX_SUCCESS != ret) {
            /* send a release so the caller doesn't hang */
            reply = OBJ_NEW(pmix_buffer_t);
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
                PMIX_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                return;
            }
            PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        }
        /* free the apps array */
        break;

        
    case PMIX_CONNECT_CMD:
    case PMIX_DISCONNECT_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd %s", (PMIX_CONNECT_CMD == cmd) ? "CONNECT" : "DISCONNECT");
        /* unpack the number of ranges */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        ranges = NULL;
        /* unpack the ranges, if provided */
        if (0 < nranges) {
            /* allocate reqd space */
            ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
            /* unpack the ranges */
            for (i=0; i < nranges; i++) {
                rngptr = &ranges[i];
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &rngptr, &cnt, PMIX_RANGE))) {
                    PMIX_ERROR_LOG(rc);
                    return;
                }
            }
        }
        /* find/create the local tracker for this operation */
        if (PMIX_CONNECT_CMD == cmd) {
            trk = get_tracker(&connects, ranges, nranges);
        } else {
            trk = get_tracker(&disconnects, ranges, nranges);
        }
        /* add this contributor to the tracker so they get
         * notified when we are done */
        cd = OBJ_NEW(pmix_server_caddy_t);
        OBJ_RETAIN(peer);
        cd->peer = peer;
        cd->tag = hdr->tag;
        pmix_list_append(&trk->locals, &cd->super);
        if (PMIX_CONNECT_CMD == cmd) {
            /* request the connect */
            if (NULL != server.connect) {
                ret = server.connect(ranges, nranges, connect_release, trk);
            } else {
                /* need to send an "err_not_supported" status back
                 * to the client so they don't hang */
                reply = OBJ_NEW(pmix_buffer_t);
                ret = PMIX_ERR_NOT_SUPPORTED;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    return;
                }
                PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
            }
        } else {
            /* request the connect */
            if (NULL != server.disconnect) {
                ret = server.disconnect(ranges, nranges, connect_release, trk);
            } else {
                /* need to send an "err_not_supported" status back
                 * to the client so they don't hang */
                reply = OBJ_NEW(pmix_buffer_t);
                ret = PMIX_ERR_NOT_SUPPORTED;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    return;
                }
                PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
            }
        }
        free(ranges);
        break;
    }
}

int PMIx_server_cred_extract(pmix_message_t *msg_opaq, pmix_peer_cred_t *cred)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    int rc;
    rc = load_peer_cred(cred,msg->hdr, msg->payload);
    return rc;
}

pmix_message_t *PMIx_server_cred_reply(int rc)
{
    pmix_message_inst_t *msg = (void*)PMIx_message_new();
    if( NULL == msg ){
        return NULL;
    }
    msg->hdr.nbytes = sizeof(int);
    msg->payload = calloc(1, sizeof(int));
    if( NULL == msg->payload ){
        PMIx_message_free((void*)msg);
        return NULL;
    }
    *((int*)msg->payload) = rc;
    msg->hdr_rcvd = 1;
    msg->hdr.rank = pmix_globals.rank;
    (void)strncpy(msg->hdr.namespace, pmix_globals.namespace, PMIX_MAX_NSLEN);
    msg->hdr.type = PMIX_USOCK_IDENT;
    msg->hdr.tag = 0;
    return (pmix_message_t *)msg;
}

int PMIx_server_set_handlers(pmix_server_module_t *module);
int PMIx_server_process_msg(pmix_message_t *msg);
