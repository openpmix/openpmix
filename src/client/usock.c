/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC. 
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2014 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "pmix_config.h"
#include "pmix_client.h"
#include "constants.h"
#include "types.h"
#include "pmix_stdint.h"
#include "pmix_socket_errno.h"
#include "pmix_stdint.h"
#include "src/util/error.h"
#include "pmix_event.h"

#include <event.h>

#include <fcntl.h>
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif



static int usock_send_blocking(char *ptr, size_t size);
static void pmix_usock_try_connect(int fd, short args, void *cbdata);
static int usock_create_socket(void);



/* State machine for internal operations */
typedef struct {
    pmix_object_t super;
    struct event *ev;
} pmix_usock_op_t;

static void usock_con(pmix_usock_op_t *p)
{
    p->ev = NULL;
}
static void usock_des(pmix_usock_op_t *p)
{
    if( NULL != p->ev ){
        event_free(p->ev);
    }
}

static OBJ_CLASS_INSTANCE(pmix_usock_op_t,
                          pmix_object_t,
                          NULL, NULL);

#define PMIX_ACTIVATE_USOCK_STATE(cbfunc)            \
    do {                                             \
        pmix_usock_op_t *op;                         \
        op = OBJ_NEW(pmix_usock_op_t);               \
        op->ev = event_new(pmix_client_globals.evbase, -1,   \
                       PMIX_EV_WRITE, (cbfunc), op);      \
        event_add(op->ev, 0);                        \
    } while(0);

/*
 * A blocking send on a non-blocking socket. Used to send the small amount of connection
 * information that identifies the peers endpoint.
 */
static int usock_send_blocking(char *ptr, size_t size)
{
    size_t cnt = 0;
    int retval;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s send blocking of %"PRIsize_t" bytes to socket %d",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        size, pmix_client_globals.sd );
    while (cnt < size) {
        retval = send(pmix_client_globals.sd, (char*)ptr+cnt, size-cnt, 0);
        if (retval < 0) {
            if (pmix_socket_errno != EINTR && pmix_socket_errno != EAGAIN && pmix_socket_errno != EWOULDBLOCK) {
                pmix_output(0, "%s usock_peer_send_blocking: send() to socket %d failed: %s (%d)\n",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), pmix_client_globals.sd,
                            strerror(pmix_socket_errno),
                            pmix_socket_errno);
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s blocking send complete to socket %d",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        pmix_client_globals.sd);
    return PMIX_SUCCESS;
}

void pmix_usock_send_recv(int fd, short args, void *cbdata)
{
    pmix_usock_sr_t *ms = (pmix_usock_sr_t*)cbdata;
    pmix_usock_posted_recv_t *req;
    pmix_usock_send_t *snd;
    uint32_t tag = UINT32_MAX;

    if (NULL != ms->cbfunc) {
        /* if a callback msg is expected, setup a recv for it */
        req = OBJ_NEW(pmix_usock_posted_recv_t);
        /* take the next tag in the sequence */
        if (UINT32_MAX == pmix_client_globals.tag ) {
            pmix_client_globals.tag = 0;
        }
        req->tag = pmix_client_globals.tag++;
        tag = req->tag;
        req->cbfunc = ms->cbfunc;
        req->cbdata = ms->cbdata;
        pmix_output_verbose(5, pmix_client_globals.debug_level,
                            "%s posting recv on tag %d",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), req->tag);
        /* add it to the list of recvs - we cannot have unexpected messages
         * in this subsystem as the server never sends us something that
         * we didn't previously request */
        pmix_list_append(&pmix_client_globals.posted_recvs, &req->super);
    }

    snd = OBJ_NEW(pmix_usock_send_t);
    snd->hdr.id = pmix_client_globals.tag;
    snd->hdr.type = PMIX_USOCK_USER;
    snd->hdr.tag = tag;
    snd->hdr.nbytes = ms->bfr->bytes_used;
    snd->data = ms->bfr->base_ptr;
    /* always start with the header */
    snd->sdptr = (char*)&snd->hdr;
    snd->sdbytes = sizeof(pmix_usock_hdr_t);

    /* add the msg to the send queue if we are already connected*/
    if (PMIX_USOCK_CONNECTED == pmix_client_globals.state) {
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock:send_nb: already connected to server - queueing for send",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        /* if there is no message on-deck, put this one there */
        if (NULL == pmix_client_globals.send_msg) {
            pmix_client_globals.send_msg = snd;
        } else {
            /* add it to the queue */
            pmix_list_append(&pmix_client_globals.send_queue, &snd->super);
        }
        /* ensure the send event is active */
        if (!pmix_client_globals.send_ev_active) {
            event_add(&pmix_client_globals.send_event, 0);
            pmix_client_globals.send_ev_active = true;
        }
        return;
    }

    /* add the message to the queue for sending after the
     * connection is formed
     */
    pmix_list_append(&pmix_client_globals.send_queue, &snd->super);

    if (PMIX_USOCK_CONNECTING != pmix_client_globals.state &&
        PMIX_USOCK_CONNECT_ACK != pmix_client_globals.state) {
        /* we have to initiate the connection - again, we do not
         * want to block while the connection is created.
         * So throw us into an event that will create
         * the connection via a mini-state-machine :-)
         */
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock:send_nb: initiating connection to server",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        pmix_client_globals.state = PMIX_USOCK_CONNECTING;
        PMIX_ACTIVATE_USOCK_STATE(pmix_usock_try_connect);
    }
}

void pmix_usock_process_msg(int fd, short flags, void *cbdata)
{
    pmix_usock_recv_t *msg = (pmix_usock_recv_t*)cbdata;
    pmix_usock_posted_recv_t *rcv;
    pmix_buffer_t buf;

    pmix_output_verbose(5, pmix_client_globals.debug_level,
                         "%s message received %d bytes for tag %u",
                         PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                         (int)msg->hdr.nbytes, msg->hdr.tag);

    /* see if we have a waiting recv for this message */
    PMIX_LIST_FOREACH(rcv, &pmix_client_globals.posted_recvs, pmix_usock_posted_recv_t) {
        pmix_output_verbose(5, pmix_client_globals.debug_level,
                            "%s checking msg on tag %u for tag %u",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            msg->hdr.tag, rcv->tag);

        if (msg->hdr.tag == rcv->tag) {
            if (NULL != rcv->cbfunc) {
                /* construct and load the buffer */
                OBJ_CONSTRUCT(&buf, pmix_buffer_t);
                if (NULL != msg->data) {
                    pmix_bfrop.load(&buf, msg->data, msg->hdr.nbytes);
                }
                msg->data = NULL;  // protect the data region
                if (NULL != rcv->cbfunc) {
                    rcv->cbfunc(&buf, rcv->cbdata);
                }
                OBJ_DESTRUCT(&buf);  // free's the msg data
                /* also done with the recv */
                pmix_list_remove_item(&pmix_client_globals.posted_recvs, &rcv->super);
                OBJ_RELEASE(rcv);
                OBJ_RELEASE(msg);
                return;
            }
        }
    }

    /* we get here if no matching recv was found - this is an error */
    pmix_output(0, "%s UNEXPECTED MESSAGE",
                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
    OBJ_RELEASE(msg);
}

static int usock_create_socket(void)
{
   int flags;

   if (pmix_client_globals.sd > 0) {
        return PMIX_SUCCESS;
    }

    pmix_output_verbose(1, pmix_client_globals.debug_level,
                         "%s pmix:usock:peer creating socket to server",
                         PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
    
    pmix_client_globals.sd = socket(PF_UNIX, SOCK_STREAM, 0);

    if (pmix_client_globals.sd < 0) {
        pmix_output(0, "%s usock_peer_create_socket: socket() failed: %s (%d)\n",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
        return PMIX_ERR_UNREACH;
    }

     /* setup the socket as non-blocking */
    if ((flags = fcntl(pmix_client_globals.sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "%s usock_peer_connect: fcntl(F_GETFL) failed: %s (%d)\n",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
    } else {
        flags |= O_NONBLOCK;
        if(fcntl(pmix_client_globals.sd, F_SETFL, flags) < 0)
            pmix_output(0, "%s usock_peer_connect: fcntl(F_SETFL) failed: %s (%d)\n",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        strerror(pmix_socket_errno),
                        pmix_socket_errno);
    }

    /* setup event callbacks */
    pmix_event_set(pmix_client_globals.evbase,
                   &pmix_client_globals.recv_event,
                   pmix_client_globals.sd,
                   PMIX_EV_READ | PMIX_EV_PERSIST,
                   pmix_usock_recv_handler, NULL);
    pmix_event_set_priority(&pmix_client_globals.recv_event, PMIX_EV_MSG_LO_PRI);
    pmix_client_globals.recv_ev_active = false;

    pmix_event_set(pmix_client_globals.evbase,
                   &pmix_client_globals.send_event,
                   pmix_client_globals.sd,
                   PMIX_EV_WRITE|PMIX_EV_PERSIST,
                   pmix_usock_send_handler, NULL);
    pmix_event_set_priority(&pmix_client_globals.send_event, PMIX_EV_MSG_LO_PRI);
    pmix_client_globals.send_ev_active = false;

    return PMIX_SUCCESS;
}


/*
 * Try connecting to a peer
 */
static void pmix_usock_try_connect(int fd, short args, void *cbdata)
{
    int rc;
    pmix_socklen_t addrlen = 0;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock_peer_try_connect: attempting to connect to server",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    if (PMIX_SUCCESS != usock_create_socket()) {
        return;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock_peer_try_connect: attempting to connect to server on socket %d",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        pmix_client_globals.sd);

    addrlen = sizeof(struct sockaddr_un);
 retry_connect:
    pmix_client_globals.retries++;
    if (connect(pmix_client_globals.sd, (struct sockaddr *) &pmix_client_globals.address, addrlen) < 0) {
        /* non-blocking so wait for completion */
        if (pmix_socket_errno == EINPROGRESS || pmix_socket_errno == EWOULDBLOCK) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s waiting for connect completion to server - activating send event",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            /* just ensure the send_event is active */
            if (!pmix_client_globals.send_ev_active) {
                pmix_event_add(&pmix_client_globals.send_event, 0);
                pmix_client_globals.send_ev_active = true;
            }
            return;
        }

        /* Some kernels (Linux 2.6) will automatically software
           abort a connection that was ECONNREFUSED on the last
           attempt, without even trying to establish the
           connection.  Handle that case in a semi-rational
           way by trying twice before giving up */
        if (ECONNABORTED == pmix_socket_errno) {
            if (pmix_client_globals.retries < pmix_client_globals.max_retries) {
                pmix_output_verbose(2, pmix_client_globals.debug_level,
                                    "%s connection to server aborted by OS - retrying",
                                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                goto retry_connect;
            } else {
                /* We were unsuccessful in establishing this connection, and are
                 * not likely to suddenly become successful,
                 */
                pmix_client_globals.state = PMIX_USOCK_FAILED;
                CLOSE_THE_SOCKET(pmix_client_globals.sd);
                return;
            }
        }
    }

    /* connection succeeded */
    pmix_client_globals.retries = 0;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s sock_peer_try_connect: Connection across to server succeeded",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
 
   /* setup our recv to catch the return ack call */
    if (!pmix_client_globals.recv_ev_active) {
        pmix_event_add(&pmix_client_globals.recv_event, 0);
        pmix_client_globals.recv_ev_active = true;
    }

    /* send our globally unique process identifier to the server */
    if (PMIX_SUCCESS == (rc = usock_send_connect_ack())) {
        pmix_client_globals.state = PMIX_USOCK_CONNECT_ACK;
    } else {
        pmix_output(0,
                    "%s usock_peer_try_connect: "
                    "usock_send_connect_ack to server failed: %s (%d)",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    pmix_strerror(rc), rc);
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        return;
    }
}

int usock_send_connect_ack(void)
{
    char *msg;
    pmix_usock_hdr_t hdr;
    int rc;
    size_t sdsize;
    // TODO: Deal with security
    //opal_sec_cred_t *cred;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s SEND CONNECT ACK",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* setup the header */
    hdr.id = PMIX_PROC_MY_NAME;
    hdr.tag = UINT32_MAX;
    hdr.type = PMIX_USOCK_IDENT;

    /* get our security credential */
//    if (PMIX_SUCCESS != (rc = opal_sec.get_my_credential(opal_dstore_internal, &OPAL_PROC_MY_NAME, &cred))) {
//        return rc;
//    }

    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = strlen(pmix_version_string) + 1; // + cred->size;

    /* create a space for our message */
    sdsize = (sizeof(hdr) + strlen(pmix_version_string) + 1 /* + cred->size */);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg+sizeof(hdr), pmix_version_string, strlen(pmix_version_string));
    //memcpy(msg+sizeof(hdr)+strlen(opal_version_string)+1, cred->credential, cred->size);


    if (PMIX_SUCCESS != usock_send_blocking(msg, sdsize)) {
        // TODO: Process all peer state changes if need
        free(msg);
        return PMIX_ERR_UNREACH;
    }
    free(msg);
    return PMIX_SUCCESS;
}



/*
 * Routine for debugging to print the connection state and socket options
 */
void pmix_usock_dump(const char* msg)
{
    char buff[255];
    int nodelay,flags;

    if ((flags = fcntl(pmix_client_globals.sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "%s usock_peer_dump: fcntl(F_GETFL) failed: %s (%d)\n",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
    }
                                                                                                            
#if defined(USOCK_NODELAY)
    optlen = sizeof(nodelay);
    if (getsockopt(pmix_client_globals.sd, IPPROTO_USOCK, USOCK_NODELAY, (char *)&nodelay, &optlen) < 0) {
        pmix_output(0, "%s usock_peer_dump: USOCK_NODELAY option: %s (%d)\n",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    strerror(pmix_socket_errno), pmix_socket_errno);
    }
#else
    nodelay = 0;
#endif

    snprintf(buff, sizeof(buff), "%s %s: nodelay %d flags %08x\n",
             PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
             msg, nodelay, flags);
    pmix_output(0, "%s", buff);
}

char* pmix_usock_state_print(pmix_usock_state_t state)
{
    switch (state) {
    case PMIX_USOCK_UNCONNECTED:
        return "UNCONNECTED";
    case PMIX_USOCK_CLOSED:
        return "CLOSED";
    case PMIX_USOCK_RESOLVE:
        return "RESOLVE";
    case PMIX_USOCK_CONNECTING:
        return "CONNECTING";
    case PMIX_USOCK_CONNECT_ACK:
        return "ACK";
    case PMIX_USOCK_CONNECTED:
        return "CONNECTED";
    case PMIX_USOCK_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

