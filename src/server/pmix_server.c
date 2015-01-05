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

// local variables
static int init_cntr = 0;
static pmix_server_module_t server;
static char *myuri = NULL;
static pmix_event_t listen_ev;
static bool local_evbase = false;
static int mysocket = -1;
static pmix_list_t peers;
static struct sockaddr_un myaddress;

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
    snd->data = (b)->base_ptr;                                          \
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


int PMIx_server_init(pmix_server_module_t *module,
                     struct event_base *evbase,
                     char *tmpdir, char *credential)
{
    int debug_level;
    pid_t pid;
    char *tdir, *evar;
    pmix_usock_posted_recv_t *req;
    
    ++init_cntr;
    if (1 < init_cntr) {
        return 0;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    /* setup the globals */
    memset(&pmix_globals, 0, sizeof(pmix_globals));
    memset(&server, 0, sizeof(pmix_server_module_t));
    OBJ_CONSTRUCT(&peers, pmix_list_t);
    
    /* initialize the output system */
    if (!pmix_output_init()) {
        return -1;
    }
    
    /* setup the function pointers */
    server = *module;
    /* save the credential, if provided */
    if (NULL != credential) {
        pmix_globals.credential = strdup(credential);
    }
    
    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        debug_level = strtol(evar, NULL, 10);
        pmix_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_globals.debug_output, debug_level);
    }

    /* initialize the datatype support */
    pmix_bfrop_open();
    /* and the usock system */
    pmix_usock_init();
    
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
    snprintf(myaddress.sun_path, sizeof(myaddress.sun_path)-1, "%s/pmix", tdir);
    asprintf(&myuri, "%lu:%s", (unsigned long)pid, myaddress.sun_path);
    
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
    return start_listening(&myaddress);
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

    if (0 <= mysocket) {
        CLOSE_THE_SOCKET(mysocket);
    }

    if (NULL != pmix_globals.credential) {
        free(pmix_globals.credential);
    }
    if (NULL != myuri) {
        free(myuri);
    }
    PMIX_LIST_DESTRUCT(&peers);
    
    pmix_bfrop_close();
    pmix_usock_finalize();

    if (local_evbase) {
        pmix_stop_progress_thread(pmix_globals.evbase);
    }

    /* cleanup the rendezvous file */
    unlink(myaddress.sun_path);
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");
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

/*  Receive the peer's identification info from a newly
 *  connected socket and verify the expected response.
 */
static int authenticate_client(int sd, pmix_peer_t **peer)
{
    char *msg;
    char *version;
    int rc;
    pmix_usock_hdr_t hdr;
    pmix_peer_t *pr;
    char *credential;
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

    /* see if we have this peer in our list */
    found = false;
    PMIX_LIST_FOREACH(pr, &peers, pmix_peer_t) {
        if (0 == strcmp(pr->namespace, hdr.namespace) &&
            pr->rank == hdr.rank) {
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
        (void)strncpy((*peer)->namespace, hdr.namespace, PMIX_MAX_NSLEN);
        (*peer)->rank = hdr.rank;
        pmix_list_append(&peers, &(*peer)->super);
    }
    
    /* get the authentication and version payload */
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
        pmix_output(0, "usock_peer_recv_connect_ack: "
                    "received different version from client: %s instead of %s",
                    version, PMIX_VERSION);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack version from client matches ours");

    /* check security token */
    if (NULL != server.authenticate) {
        /* server desires authentication */
        credential = (char*)(msg + strlen(version) + 1);
        if (0 != server.authenticate(credential)) {
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

            static void server_switchyard(int sd, pmix_usock_hdr_t *hdr,
                                          pmix_buffer_t *buf, void *cbdata)
{
    int rc, status, ret;
    int32_t cnt;
    pmix_cmd_t cmd;
    char *msg;
    pmix_peer_t *peer, *pr;
    pmix_buffer_t *reply, *bptr;
    pmix_range_t *ranges, *rngptr;
    size_t i, nranges, nblobs;
    pmix_scope_t scope;
    char *nspace;
    int rnk;
    pmix_modex_data_t mdx, *mdxarray, *mptr;
    
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
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FENCE");
        /* unpack the number of ranges */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        ranges = NULL;
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
        /* let the local host's server execute the fence - this
         * should block until the fence is complete */
        if (NULL != server.fence) {
            ret = server.fence(ranges, nranges, &mdxarray, &nblobs);
        }
        if (NULL != ranges) {
            free(ranges);
        }
        /* send the reply */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        /* pack the nblobs being returned */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nblobs, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        if (0 < nblobs) {
            for (i=0; i < nblobs; i++) {
                mptr = &mdxarray[i];
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &mptr, 1, PMIX_MODEX))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    free(mdxarray);
                    return;
                }
                if (NULL != mdxarray[i].blob) {
                    free(mdxarray[i].blob);
                }
            }
            free(mdxarray);
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_FENCENB_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FENCENB");
        break;

        
    case PMIX_GET_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd GET");
        /* retrieve the namespace and rank of the requested proc */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nspace, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &rnk, &cnt, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        /* request the data, if supported */
        if (NULL != server.get_modex) {
            ret = server.get_modex(nspace, rnk, &mdxarray, &nblobs);
        }
        /* send the reply */
        reply = OBJ_NEW(pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ret, 1, PMIX_INT))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        /* pack the nblobs being returned */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nblobs, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        /* pack any blobs */
        if (0 < nblobs) {
            for (i=0; i < nblobs; i++) {
                mptr = &mdxarray[i];
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &mptr, 1, PMIX_MODEX))) {
                    PMIX_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    free(mdxarray);
                    return;
                }
                if (NULL != mdxarray[i].blob) {
                    free(mdxarray[i].blob);
                }
            }
            free(mdxarray);
        }
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        break;

        
    case PMIX_GETNB_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd GETNB");
        break;

        
    case PMIX_FINALIZE_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FINALIZE");
        /* call the local server, if supported */
        ret = PMIX_SUCCESS;
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
        break;
    case PMIX_LOOKUP_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd LOOKUP");
        break;
    case PMIX_UNPUBLISH_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd UNPUBLISH");
        break;
    case PMIX_SPAWN_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd SPAWN");
        break;
    case PMIX_CONNECT_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd CONNECT");
        break;
    case PMIX_DISCONNECT_CMD:
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd DISCONNECT");
        break;
    }
}

