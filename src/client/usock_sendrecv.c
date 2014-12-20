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
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2014 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_event.h"
#include "pmix_client.h"
#include "constants.h"
#include "types.h"
#include "pmix_stdint.h"
#include "pmix_socket_errno.h"
#include "pmix_stdint.h"
#include "src/util/error.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
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
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

static void usock_complete_connect(void);
static int usock_recv_connect_ack(void);

static int send_bytes(pmix_usock_send_t *msg)
{
    int rc;

    while (0 < msg->sdbytes) {
        rc = write(pmix_client_globals.sd, msg->sdptr, msg->sdbytes);
        if (rc < 0) {
            if (pmix_socket_errno == EINTR) {
                continue;
            } else if (pmix_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PMIX_ERR_RESOURCE_BUSY;
            } else if (pmix_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PMIX_ERR_WOULD_BLOCK;
            }
            /* we hit an error and cannot progress this message */
            pmix_output(0, "%s pmix_usock_msg_send_bytes: write failed: %s (%d) [sd = %d]",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        strerror(pmix_socket_errno),
                        pmix_socket_errno,
                        pmix_client_globals.sd);
            return PMIX_ERR_COMM_FAILURE;
        }
        /* update location */
        msg->sdbytes -= rc;
        msg->sdptr += rc;
    }
    /* we sent the full data block */
    return PMIX_SUCCESS;
}

/*
 * A file descriptor is available/ready for send. Check the state
 * of the socket and take the appropriate action.
 */
void pmix_usock_send_handler(int sd, short flags, void *cbdata)
{
    pmix_usock_send_t *msg = pmix_client_globals.send_msg;
    int rc;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock:send_handler called to send to server",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    switch (pmix_client_globals.state) {
    case PMIX_USOCK_CONNECTING:
    case PMIX_USOCK_CLOSED:
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "usock:send_handler %s",
                            pmix_usock_state_print(pmix_client_globals.state));
        usock_complete_connect();
        /* de-activate the send event until the connection
         * handshake completes
         */
        if (pmix_client_globals.send_ev_active) {
            pmix_event_del(&pmix_client_globals.send_event);
            pmix_client_globals.send_ev_active = false;
        }
        break;
    case PMIX_USOCK_CONNECTED:
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock:send_handler SENDING TO SERVER with %s msg",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            (NULL == msg) ? "NULL" : "NON-NULL");
        if (NULL != msg) {
            if (!msg->hdr_sent) {
                pmix_output_verbose(2, pmix_client_globals.debug_level,
                                    "%s usock:send_handler SENDING HEADER",
                                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                if (PMIX_SUCCESS == (rc = send_bytes(msg))) {
                    /* header is completely sent */
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s usock:send_handler HEADER SENT",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                    msg->hdr_sent = true;
                    /* setup to send the data */
                    if (NULL == msg->data) {
                        /* this was a zero-byte msg - nothing more to do */
                        OBJ_RELEASE(msg);
                        pmix_client_globals.send_msg = NULL;
                        goto next;
                    } else {
                        /* send the data as a single block */
                        msg->sdptr = msg->data;
                        msg->sdbytes = msg->hdr.nbytes;
                    }
                    /* fall thru and let the send progress */
                } else if (PMIX_ERR_RESOURCE_BUSY == rc ||
                           PMIX_ERR_WOULD_BLOCK == rc) {
                    /* exit this event and let the event lib progress */
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s usock:send_handler RES BUSY OR WOULD BLOCK",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                    return;
                } else {
                    // report the error
                    pmix_output(0, "%s pmix_usock_peer_send_handler: unable to send message ON SOCKET %d",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                pmix_client_globals.sd);
                    pmix_event_del(&pmix_client_globals.send_event);
                    pmix_client_globals.send_ev_active = false;
                    OBJ_RELEASE(msg);
                    pmix_client_globals.send_msg = NULL;
                    return;
                }
            }

            if (msg->hdr_sent) {
                pmix_output_verbose(2, pmix_client_globals.debug_level,
                                    "%s usock:send_handler SENDING BODY OF MSG",
                                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                if (PMIX_SUCCESS == (rc = send_bytes(msg))) {
                    // message is complete
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s usock:send_handler BODY SENT",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                    OBJ_RELEASE(msg);
                    pmix_client_globals.send_msg = NULL;
                    goto next;
                } else if (PMIX_ERR_RESOURCE_BUSY == rc ||
                           PMIX_ERR_WOULD_BLOCK == rc) {
                    /* exit this event and let the event lib progress */
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s usock:send_handler RES BUSY OR WOULD BLOCK",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                    return;
                } else {
                    // report the error
                    pmix_output(0, "%s pmix_usock_peer_send_handler: unable to send message ON SOCKET %d",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                pmix_client_globals.sd);
                    pmix_event_del(&pmix_client_globals.send_event);
                    pmix_client_globals.send_ev_active = false;
                    OBJ_RELEASE(msg);
                    pmix_client_globals.send_msg = NULL;
                    return;
                }
            }

        next:
            /* if current message completed - progress any pending sends by
             * moving the next in the queue into the "on-deck" position. Note
             * that this doesn't mean we send the message right now - we will
             * wait for another send_event to fire before doing so. This gives
             * us a chance to service any pending recvs.
             */
            pmix_client_globals.send_msg = (pmix_usock_send_t*)
                pmix_list_remove_first(&pmix_client_globals.send_queue);
        }

        /* if nothing else to do unregister for send event notifications */
        if (NULL == pmix_client_globals.send_msg &&
            pmix_client_globals.send_ev_active) {
            pmix_event_del(&pmix_client_globals.send_event);
            pmix_client_globals.send_ev_active = false;
        }
        break;

    default:
        pmix_output(0, "%s pmix_usock_peer_send_handler: invalid connection state (%d) on socket %d",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    pmix_client_globals.state, pmix_client_globals.sd);
        if (pmix_client_globals.send_ev_active) {
            pmix_event_del(&pmix_client_globals.send_event);
            pmix_client_globals.send_ev_active = false;
        }
        break;
    }
}

static int read_bytes(pmix_usock_recv_t* recv)
{
    int rc;

    /* read until all bytes recvd or error */
    while (0 < recv->rdbytes) {
        rc = read(pmix_client_globals.sd, recv->rdptr, recv->rdbytes);
        if (rc < 0) {
            if(pmix_socket_errno == EINTR) {
                continue;
            } else if (pmix_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PMIX_ERR_RESOURCE_BUSY;
            } else if (pmix_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PMIX_ERR_WOULD_BLOCK;
            }
            /* we hit an error and cannot progress this message - report
             * the error back to the RML and let the caller know
             * to abort this message
             */
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s pmix_usock_msg_recv: readv failed: %s (%d)",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                strerror(pmix_socket_errno),
                                pmix_socket_errno);
            return PMIX_ERR_COMM_FAILURE;
        } else if (rc == 0)  {
            /* the remote peer closed the connection - report that condition
             * and let the caller know
             */
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s pmix_usock_msg_recv: peer closed connection",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            /* stop all events */
            if (pmix_client_globals.recv_ev_active) {
                pmix_event_del(&pmix_client_globals.recv_event);
                pmix_client_globals.recv_ev_active = false;
            }
            if (pmix_client_globals.timer_ev_active) {
                pmix_event_del(&pmix_client_globals.timer_event);
                pmix_client_globals.timer_ev_active = false;
            }
            if (pmix_client_globals.send_ev_active) {
                pmix_event_del(&pmix_client_globals.send_event);
                pmix_client_globals.send_ev_active = false;
            }
            if (NULL != pmix_client_globals.recv_msg) {
                OBJ_RELEASE(pmix_client_globals.recv_msg);
                pmix_client_globals.recv_msg = NULL;
            }
            CLOSE_THE_SOCKET(pmix_client_globals.sd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        /* we were able to read something, so adjust counters and location */
        recv->rdbytes -= rc;
        recv->rdptr += rc;
    }

    /* we read the full data block */
    return PMIX_SUCCESS;
}

/*
 * Dispatch to the appropriate action routine based on the state
 * of the connection with the peer.
 */

void pmix_usock_recv_handler(int sd, short flags, void *cbdata)
{
    int rc;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock:recv:handler called",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    switch (pmix_client_globals.state) {
    case PMIX_USOCK_CONNECT_ACK:
        if (PMIX_SUCCESS == (rc = usock_recv_connect_ack())) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s usock:recv:handler starting send/recv events",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            /* we connected! Start the send/recv events */
            if (!pmix_client_globals.recv_ev_active) {
                pmix_event_add(&pmix_client_globals.recv_event, 0);
                pmix_client_globals.recv_ev_active = true;
            }
            if (pmix_client_globals.timer_ev_active) {
                pmix_event_del(&pmix_client_globals.timer_event);
                pmix_client_globals.timer_ev_active = false;
            }
            /* if there is a message waiting to be sent, queue it */
            if (NULL == pmix_client_globals.send_msg) {
                pmix_client_globals.send_msg = (pmix_usock_send_t*)pmix_list_remove_first(&pmix_client_globals.send_queue);
            }
            if (NULL != pmix_client_globals.send_msg && !pmix_client_globals.send_ev_active) {
                pmix_event_add(&pmix_client_globals.send_event, 0);
                pmix_client_globals.send_ev_active = true;
            }
            /* update our state */
            pmix_client_globals.state = PMIX_USOCK_CONNECTED;
        } else {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s UNABLE TO COMPLETE CONNECT ACK WITH SERVER",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            pmix_event_del(&pmix_client_globals.recv_event);
            pmix_client_globals.recv_ev_active = false;
            return;
        }
        break;
    case PMIX_USOCK_CONNECTED:
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock:recv:handler CONNECTED",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        /* allocate a new message and setup for recv */
        if (NULL == pmix_client_globals.recv_msg) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s usock:recv:handler allocate new recv msg",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            pmix_client_globals.recv_msg = OBJ_NEW(pmix_usock_recv_t);
            if (NULL == pmix_client_globals.recv_msg) {
                pmix_output(0, "%s usock_recv_handler: unable to allocate recv message\n",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                return;
            }
            /* start by reading the header */
            pmix_client_globals.recv_msg->rdptr = (char*)&pmix_client_globals.recv_msg->hdr;
            pmix_client_globals.recv_msg->rdbytes = sizeof(pmix_usock_hdr_t);
        }
        /* if the header hasn't been completely read, read it */
        if (!pmix_client_globals.recv_msg->hdr_recvd) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "usock:recv:handler read hdr");
            if (PMIX_SUCCESS == (rc = read_bytes(pmix_client_globals.recv_msg))) {
                /* completed reading the header */
                pmix_client_globals.recv_msg->hdr_recvd = true;
                /* if this is a zero-byte message, then we are done */
                if (0 == pmix_client_globals.recv_msg->hdr.nbytes) {
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s RECVD ZERO-BYTE MESSAGE FROM SERVER for tag %d",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                        pmix_client_globals.recv_msg->hdr.tag);
                    pmix_client_globals.recv_msg->data = NULL;  // make sure
                    pmix_client_globals.recv_msg->rdptr = NULL;
                    pmix_client_globals.recv_msg->rdbytes = 0;
                } else {
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s usock:recv:handler allocate data region of size %lu",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                        (unsigned long)pmix_client_globals.recv_msg->hdr.nbytes);
                    /* allocate the data region */
                    pmix_client_globals.recv_msg->data = (char*)malloc(pmix_client_globals.recv_msg->hdr.nbytes);
                    /* point to it */
                    pmix_client_globals.recv_msg->rdptr = pmix_client_globals.recv_msg->data;
                    pmix_client_globals.recv_msg->rdbytes = pmix_client_globals.recv_msg->hdr.nbytes;
                }
                /* fall thru and attempt to read the data */
            } else if (PMIX_ERR_RESOURCE_BUSY == rc ||
                       PMIX_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                /* close the connection */
                pmix_output_verbose(2, pmix_client_globals.debug_level,
                                    "%s usock:recv:handler error reading bytes - closing connection",
                                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                CLOSE_THE_SOCKET(pmix_client_globals.sd);
                return;
            }
        }

        if (pmix_client_globals.recv_msg->hdr_recvd) {
            /* continue to read the data block - we start from
             * wherever we left off, which could be at the
             * beginning or somewhere in the message
             */
            if (PMIX_SUCCESS == (rc = read_bytes(pmix_client_globals.recv_msg))) {
                /* we recvd all of the message */
                pmix_output_verbose(2, pmix_client_globals.debug_level,
                                    "%s RECVD COMPLETE MESSAGE FROM SERVER OF %d BYTES FOR TAG %d",
                                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                    (int)pmix_client_globals.recv_msg->hdr.nbytes,
                                    pmix_client_globals.recv_msg->hdr.tag);
                /* post it for delivery */
                PMIX_ACTIVATE_POST_MSG(pmix_client_globals.recv_msg);
                pmix_client_globals.recv_msg = NULL;
            } else if (PMIX_ERR_RESOURCE_BUSY == rc ||
                       PMIX_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                // report the error
                pmix_output(0, "%s usock_peer_recv_handler: unable to recv message",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
                /* turn off the recv event */
                pmix_event_del(&pmix_client_globals.recv_event);
                pmix_client_globals.recv_ev_active = false;
                CLOSE_THE_SOCKET(pmix_client_globals.sd);
                return;
            }
        }
        break;
    default: 
        pmix_output(0, "%s usock_peer_recv_handler: invalid socket state(%d)",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    pmix_client_globals.state);
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        break;
    }
}

/*
 * A blocking recv on a non-blocking socket. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 */
static bool usock_recv_blocking(char *data, size_t size)
{
    size_t cnt = 0;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s waiting for connect ack from server",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    while (cnt < size) {
        int retval = recv(pmix_client_globals.sd, (char *)data+cnt, size-cnt, 0);

        /* remote closed connection */
        if (retval == 0) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s usock_recv_blocking: server closed connection: state %d",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                pmix_client_globals.state);
            pmix_client_globals.state = PMIX_USOCK_CLOSED;
            CLOSE_THE_SOCKET(pmix_client_globals.sd);
            return false;
        }

        /* socket is non-blocking so handle errors */
        if (retval < 0) {
            if (pmix_socket_errno != EINTR &&
                pmix_socket_errno != EAGAIN &&
                pmix_socket_errno != EWOULDBLOCK) {
                if (pmix_client_globals.state == PMIX_USOCK_CONNECT_ACK) {
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
                    pmix_output_verbose(2, pmix_client_globals.debug_level,
                                        "%s connect ack received error %s from server",
                                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                        strerror(pmix_socket_errno));
                    return false;
                } else {
                    pmix_output(0,
                                "%s usock_recv_blocking: "
                                "recv() failed for server: %s (%d)\n",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                                strerror(pmix_socket_errno),
                                pmix_socket_errno);
                    pmix_client_globals.state = PMIX_USOCK_FAILED;
                    CLOSE_THE_SOCKET(pmix_client_globals.sd);
                    return false;
                }
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s connect ack received from server",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
    return true;
}


/*
 *  Receive the peers globally unique process identification from a newly
 *  connected socket and verify the expected response. If so, move the
 *  socket to a connected state.
 */
static int usock_recv_connect_ack(void)
{
    char *msg;
    char *version;
    int rc;
//    opal_sec_cred_t creds;
    pmix_usock_hdr_t hdr;

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s RECV CONNECT ACK FROM SERVER ON SOCKET %d",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        pmix_client_globals.sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));

    if (usock_recv_blocking((char*)&hdr, sizeof(pmix_usock_hdr_t))) {
        /* If the state is CONNECT_ACK, then we were waiting for
         * the connection to be ack'd
         */
        if (pmix_client_globals.state != PMIX_USOCK_CONNECT_ACK) {
            /* handshake broke down - abort this connection */
            pmix_output(0, "%s RECV CONNECT BAD HANDSHAKE FROM SERVER ON SOCKET %d",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        pmix_client_globals.sd);
            pmix_client_globals.state = PMIX_USOCK_FAILED;
            CLOSE_THE_SOCKET(pmix_client_globals.sd);
            return PMIX_ERR_UNREACH;
        }
    } else {
        /* unable to complete the recv */
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s unable to complete recv of connect-ack from server ON SOCKET %d",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            pmix_client_globals.sd);
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s connect-ack recvd from server",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* compare the servers name to the expected value */
    if (hdr.id != pmix_client_globals.server) {
        pmix_output(0, "usock_peer_recv_connect_ack: "
                    "%s received unexpected process identifier %"PRIu64" from server: expected %"PRIu64"",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    hdr.id, pmix_client_globals.server);
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s connect-ack header from server is okay",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* get the authentication and version payload */
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    if (!usock_recv_blocking(msg, hdr.nbytes)) {
        /* unable to complete the recv */
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s unable to complete recv of connect-ack from server ON SOCKET %d",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            pmix_client_globals.sd);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    /* check that this is from a matching version */
    version = (char*)(msg);
    if (0 != strcmp(version, pmix_version_string)) {
        pmix_output(0, "usock_peer_recv_connect_ack: "
                    "%s received different version from server: %s instead of %s",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    version, pmix_version_string);
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s connect-ack version from server matches ours",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* check security token */
//    creds.credential = (char*)(msg + strlen(version) + 1);
//    creds.size = hdr.nbytes - strlen(version) - 1;
//    if (PMIX_SUCCESS != (rc = opal_sec.authenticate(&creds))) {
//        PMIX_ERROR_LOG(rc);
//    }
    free(msg);

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s connect-ack from server authenticated",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    /* connected */
    pmix_client_globals.state = PMIX_USOCK_CONNECTED;
    /* initiate send of first message on queue */
    if (NULL == pmix_client_globals.send_msg) {
        pmix_client_globals.send_msg = (pmix_usock_send_t*)
            pmix_list_remove_first(&pmix_client_globals.send_queue);
    }
    if (NULL != pmix_client_globals.send_msg && !pmix_client_globals.send_ev_active) {
        pmix_event_add(&pmix_client_globals.send_event, 0);
        pmix_client_globals.send_ev_active = true;
    }
    if (2 <= pmix_output_get_verbosity(pmix_client_globals.debug_level)) {
        pmix_usock_dump("connected");
    }
    return PMIX_SUCCESS;
}


/*
 * Check the status of the connection. If the connection failed, will retry
 * later. Otherwise, send this process' identifier to the server on the
 * newly connected socket.
 */
static void usock_complete_connect(void)
{
    int so_error = 0;
    pmix_socklen_t so_length = sizeof(so_error);

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock:complete_connect called for server on socket %d",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        pmix_client_globals.sd);

    /* check connect completion status */
    if (getsockopt(pmix_client_globals.sd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &so_length) < 0) {
        pmix_output(0, "%s usock_peer_complete_connect: getsockopt() to server failed: %s (%d)\n",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        return;
    }

    if (so_error == EINPROGRESS) {
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock:send:handler still in progress",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        return;
    } else if (so_error == ECONNREFUSED || so_error == ETIMEDOUT) {
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock_peer_complete_connect: connection to server failed: %s (%d)",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            strerror(so_error),
                            so_error);
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        return;
    } else if (so_error != 0) {
        /* No need to worry about the return code here - we return regardless
           at this point, and if an error did occur a message has already been
           printed for the user */
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock_peer_complete_connect: "
                            "connection to server failed with error %d",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                            so_error);
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
        return;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock_peer_complete_connect: sending ack to server",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));

    if (usock_send_connect_ack() == PMIX_SUCCESS) {
        pmix_client_globals.state = PMIX_USOCK_CONNECT_ACK;
        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "%s usock_peer_complete_connect: setting read event on connection to server",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        
        if (!pmix_client_globals.recv_ev_active) {
            pmix_event_add(&pmix_client_globals.recv_event, 0);
            pmix_client_globals.recv_ev_active = true;
        }
    } else {
        pmix_output(0, "%s usock_complete_connect: unable to send connect ack to server",
                    PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        pmix_client_globals.state = PMIX_USOCK_FAILED;
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
    }
}

