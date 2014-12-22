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

int send_bytes(int sd, char **buf, int *remain)
{
    int ret = PMIX_SUCCESS, rc;
    char *ptr = *buf;
    while (0 < *remain) {
        rc = write(sd, ptr, *remain);
        if (rc < 0) {
            if (pmix_socket_errno == EINTR) {
                continue;
            } else if (pmix_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                ret = PMIX_ERR_RESOURCE_BUSY;
                goto exit;
            } else if (pmix_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                ret = PMIX_ERR_WOULD_BLOCK;
                goto exit;
            }
            /* we hit an error and cannot progress this message */
            pmix_output(0, "%s pmix_usock_msg_send_bytes: write failed: %s (%d) [sd = %d]",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        strerror(pmix_socket_errno),
                        pmix_socket_errno, sd);
            ret = PMIX_ERR_COMM_FAILURE;
            goto exit;
        }
        /* update location */
        (*remain) -= rc;
        ptr += rc;
    }
    /* we sent the full data block */
exit:
    *buf = ptr;
    return ret;
}

int read_bytes(int sd, char **buf, int *remain)
{
    int ret = PMIX_SUCCESS, rc;
    char *ptr = *buf;

    /* read until all bytes recvd or error */
    while (0 < *remain) {
        rc = read(sd, ptr, *remain);
        if (rc < 0) {
            if(pmix_socket_errno == EINTR) {
                continue;
            } else if (pmix_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                ret = PMIX_ERR_RESOURCE_BUSY;
                goto exit;
            } else if (pmix_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                ret = PMIX_ERR_WOULD_BLOCK;
                goto exit;
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
            ret = PMIX_ERR_UNREACH;
            goto exit;
        } else if (rc == 0)  {
            ret = PMIX_ERR_UNREACH;
            goto exit;
        }
        /* we were able to read something, so adjust counters and location */
        *remain -= rc;
        ptr += rc;
    }
    /* we read the full data block */
exit:
    *buf = ptr;
    return ret;
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

    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock:send_handler SENDING TO SERVER with %s msg",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        (NULL == msg) ? "NULL" : "NON-NULL");
    if (NULL != msg) {
        if (!msg->hdr_sent) {
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s usock:send_handler SENDING HEADER",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            if (PMIX_SUCCESS == (rc = send_bytes(sd,&msg->sdptr, &msg->sdbytes))) {
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
            if (PMIX_SUCCESS == (rc = send_bytes(sd,&msg->sdptr, &msg->sdbytes))) {
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
}

/*
 * Dispatch to the appropriate action routine based on the state
 * of the connection with the peer.
 */

void pmix_usock_recv_handler(int sd, short flags, void *cbdata)
{
    int rc;

    pmix_usock_recv_t *msg = NULL;
    pmix_output_verbose(2, pmix_client_globals.debug_level,
                        "%s usock:recv:handler called",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
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
    msg = pmix_client_globals.recv_msg;
    /* if the header hasn't been completely read, read it */
    if (!pmix_client_globals.recv_msg->hdr_recvd) {

        pmix_output_verbose(2, pmix_client_globals.debug_level,
                            "usock:recv:handler read hdr");
        if (PMIX_SUCCESS == (rc = read_bytes(sd, &msg->rdptr, &msg->rdbytes))) {
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
            /* the remote peer closed the connection - report that condition
             * and let the caller know
             */
            pmix_output_verbose(2, pmix_client_globals.debug_level,
                                "%s pmix_usock_msg_recv: peer closed connection",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
            goto err_closed;
        }
    }

    if (pmix_client_globals.recv_msg->hdr_recvd) {
        /* continue to read the data block - we start from
             * wherever we left off, which could be at the
             * beginning or somewhere in the message
             */
        if (PMIX_SUCCESS == (rc = read_bytes(sd, &msg->rdptr, &msg->rdbytes))) {
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
err_closed:
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
    pmix_client_globals.sd = -1;
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
