/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
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

#include "src/buffer_ops/buffer_ops.h"

#include "usock.h"

/* instance usock globals */
pmix_usock_globals_t pmix_usock_globals;

void pmix_usock_init(void)
{
    /* setup the usock globals */
    OBJ_CONSTRUCT(&pmix_usock_globals.posted_recvs, pmix_list_t);
}

void pmix_usock_finalize(void)
{
    PMIX_LIST_DESTRUCT(&pmix_usock_globals.posted_recvs);
}

int pmix_usock_set_nonblocking(int sd)
{
    int flags;
     /* setup the socket as non-blocking */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "usock_peer_connect: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
    } else {
        flags |= O_NONBLOCK;
        if(fcntl(sd, F_SETFL, flags) < 0)
            pmix_output(0, "usock_peer_connect: fcntl(F_SETFL) failed: %s (%d)\n",
                        strerror(pmix_socket_errno),
                        pmix_socket_errno);
    }
    return PMIX_SUCCESS;
}

/*
 * A blocking send on a non-blocking socket. Used to send the small amount of connection
 * information that identifies the peers endpoint.
 */
int pmix_usock_send_blocking(int sd, char *ptr, size_t size)
{
    size_t cnt = 0;
    int retval;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "send blocking of %"PRIsize_t" bytes to socket %d",
                        size, sd );
    while (cnt < size) {
        retval = send(sd, (char*)ptr+cnt, size-cnt, 0);
        if (retval < 0) {
            if ( pmix_socket_errno != EINTR ) {
                pmix_output(0, "usock_peer_send_blocking: send() to socket %d failed: %s (%d)\n",
                            sd, strerror(pmix_socket_errno),
                            pmix_socket_errno);
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "blocking send complete to socket %d", sd);
    return PMIX_SUCCESS;
}

/*
 * A blocking recv on a non-blocking socket. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 */
int pmix_usock_recv_blocking(int sd, char *data, size_t size)
{
    size_t cnt = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "waiting for connect ack");

    while (cnt < size) {
        int retval = recv(sd, (char *)data+cnt, size-cnt, 0);

        /* remote closed connection */
        if (retval == 0) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "usock_recv_blocking: remote closed connection");
            return PMIX_ERR_UNREACH;
        }

        /* handle errors */
        if (retval < 0) {
            if (pmix_socket_errno != EINTR ) {
                /* If we overflow the listen backlog, it's
                       possible that even though we finished the three
                       way handshake, the remote host was unable to
                       transition the connection from half connected
                       (received the initial SYN) to fully connected
                       (in the listen backlog).  We likely won't see
                       the failure until we try to receive, due to
                       timing and the like.  The first thing we'll get
                       in that case is a RST packet, which receive
                       will turn into a connection reset by peer
                       errno.  In that case, leave the socket in
                       CONNECT_ACK and propogate the error up to
                       recv_connect_ack, who will try to establish the
                       connection again */
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "connect ack received error %s from remote",
                                    strerror(pmix_socket_errno));
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect ack received from remote");
    return PMIX_SUCCESS;
}

/***   INSTANTIATE INTERNAL CLASSES   ***/
static void scon(pmix_usock_send_t *p)
{
    memset(&p->hdr, 0, sizeof(pmix_usock_hdr_t));
    p->hdr.type = 0;
    p->hdr.tag = UINT32_MAX;
    p->hdr.nbytes = 0;
    p->data = NULL;
    p->hdr_sent = false;
    p->sdptr = NULL;
    p->sdbytes = 0;
}
static void sdes(pmix_usock_send_t *p)
{
    if (NULL != p->data) {
        OBJ_RELEASE(p->data);
    }
}
OBJ_CLASS_INSTANCE(pmix_usock_send_t,
                   pmix_list_item_t,
                   scon, sdes);

static void rcon(pmix_usock_recv_t *p)
{
    memset(&p->hdr, 0, sizeof(pmix_usock_hdr_t));
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
    p->peer = NULL;
    p->bfr = NULL;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
OBJ_CLASS_INSTANCE(pmix_usock_sr_t,
                   pmix_object_t,
                   srcon, NULL);

static void pcon(pmix_peer_t *p)
{
    memset(p->namespace, 0, PMIX_MAX_NSLEN);
    p->rank = -1;
    p->sd = -1;
    p->send_ev_active = false;
    p->recv_ev_active = false;
    OBJ_CONSTRUCT(&p->send_queue, pmix_list_t);
    p->send_msg = NULL;
    p->recv_msg = NULL;
}
static void pdes(pmix_peer_t *p)
{
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
    if (p->send_ev_active) {
        event_del(&p->send_event);
    }
    if (p->recv_ev_active) {
        event_del(&p->recv_event);
    }
    PMIX_LIST_DESTRUCT(&p->send_queue);
    if (NULL != p->send_msg) {
        OBJ_RELEASE(p->send_msg);
    }
    if (NULL != p->recv_msg) {
        OBJ_RELEASE(p->recv_msg);
    }
}
OBJ_CLASS_INSTANCE(pmix_peer_t,
                   pmix_list_item_t,
                   pcon, pdes);
