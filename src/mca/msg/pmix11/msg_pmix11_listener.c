/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2016 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <src/include/types.h>
#include <pmix/autogen/pmix_stdint.h>
#include <src/include/pmix_socket_errno.h>

#include <pmix_server.h>
#include "src/include/pmix_globals.h"

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
#include <ctype.h>
#include <sys/stat.h>
#include PMIX_EVENT_HEADER
#include <pthread.h>

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/fd.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/progress_threads.h"
#include "src/util/strnlen.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_server_ops.h"

// local functions for connection support
static void* listen_thread(void *obj);
static void listener_cb(int incoming_sd, void *cbdata);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static char *myversion = NULL;
static pthread_t engine;

/*
 * start listening on our rendezvous file
 */
pmix_status_t pmix_start_listening(pmix_listener_t *lt)
{
    int flags;
    pmix_status_t rc;
    socklen_t addrlen;
    char *ptr;
    struct sockaddr_un *address = &lt->address;

    /* create a listen socket for incoming connection attempts */
    lt->socket = socket(PF_UNIX, SOCK_STREAM, 0);
    if (lt->socket < 0) {
        printf("%s:%d socket() failed\n", __FILE__, __LINE__);
        return PMIX_ERROR;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(lt->socket, (struct sockaddr*)address, addrlen) < 0) {
        printf("%s:%d bind() failed\n", __FILE__, __LINE__);
        return PMIX_ERROR;
    }
    /* chown as required */
    if (lt->owner_given) {
        if (0 != chown(address->sun_path, lt->owner, -1)) {
            pmix_output(0, "CANNOT CHOWN socket %s: %s", address->sun_path, strerror (errno));
            goto sockerror;
        }
    }
    if (lt->group_given) {
        if (0 != chown(address->sun_path, -1, lt->group)) {
            pmix_output(0, "CANNOT CHOWN socket %s: %s", address->sun_path, strerror (errno));
            goto sockerror;
        }
    }
    /* set the mode as required */
    if (0 != chmod(address->sun_path, lt->mode)) {
        pmix_output(0, "CANNOT CHMOD socket %s: %s", address->sun_path, strerror (errno));
        goto sockerror;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(lt->socket, SOMAXCONN) < 0) {
        printf("%s:%d listen() failed\n", __FILE__, __LINE__);
        goto sockerror;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(lt->socket, F_GETFL, 0)) < 0) {
        printf("%s:%d fcntl(F_GETFL) failed\n", __FILE__, __LINE__);
        goto sockerror;
    }
    flags |= O_NONBLOCK;
    if (fcntl(lt->socket, F_SETFL, flags) < 0) {
        printf("%s:%d fcntl(F_SETFL) failed\n", __FILE__, __LINE__);
        goto sockerror;
    }

    /* setup my version for validating connections - we
     * only check the major version numbers */
    myversion = strdup(PMIX_VERSION);
    /* find the first '.' */
    ptr = strchr(myversion, '.');
    if (NULL != ptr) {
        ++ptr;
        /* stop it at the second '.', if present */
        if (NULL != (ptr = strchr(ptr, '.'))) {
            *ptr = '\0';
        }
    }

    /* if the server will listen for us, then ask it to do so now */
    rc = PMIX_ERR_NOT_SUPPORTED;
    if (NULL != pmix_host_server.listener) {
        rc = pmix_host_server.listener(lt->socket, listener_cb, (void*)lt);
    }

    if (PMIX_SUCCESS != rc && !pmix_server_globals.listen_thread_active) {
        /*** spawn internal listener thread */
        if (0 > pipe(pmix_server_globals.stop_thread)) {
            PMIX_ERROR_LOG(PMIX_ERR_IN_ERRNO);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        /* Make sure the pipe FDs are set to close-on-exec so that
           they don't leak into children */
        if (pmix_fd_set_cloexec(pmix_server_globals.stop_thread[0]) != PMIX_SUCCESS ||
            pmix_fd_set_cloexec(pmix_server_globals.stop_thread[1]) != PMIX_SUCCESS) {
            PMIX_ERROR_LOG(PMIX_ERR_IN_ERRNO);
            close(pmix_server_globals.stop_thread[0]);
            close(pmix_server_globals.stop_thread[1]);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        /* fork off the listener thread */
        pmix_server_globals.listen_thread_active = true;
        if (0 > pthread_create(&engine, NULL, listen_thread, NULL)) {
            pmix_server_globals.listen_thread_active = false;
            return PMIX_ERROR;
        }
    }

    return PMIX_SUCCESS;

sockerror:
    (void)close(lt->socket);
    lt->socket = -1;
    return PMIX_ERROR;
}

void pmix_stop_listening(void)
{
    int i;
    pmix_listener_t *lt;

    pmix_output_verbose(8, pmix_globals.debug_output,
                        "listen_thread: shutdown");

    if (!pmix_server_globals.listen_thread_active) {
        /* nothing we can do */
        return;
    }

    /* mark it as inactive */
    pmix_server_globals.listen_thread_active = false;
    /* use the block to break it loose just in
     * case the thread is blocked in a call to select for
     * a long time */
    i=1;
    if (0 > write(pmix_server_globals.stop_thread[1], &i, sizeof(int))) {
        return;
    }
    /* wait for thread to exit */
    pthread_join(engine, NULL);
    /* close the sockets to remove the connection points */
    PMIX_LIST_FOREACH(lt, &pmix_server_globals.listeners, pmix_listener_t) {
        CLOSE_THE_SOCKET(lt->socket);
        lt->socket = -1;
    }
    return;
}

static void* listen_thread(void *obj)
{
    int rc, max, accepted_connections;
    socklen_t addrlen = sizeof(struct sockaddr_storage);
    pmix_pending_connection_t *pending_connection;
    struct timeval timeout;
    fd_set readfds;
    pmix_listener_t *lt;

    pmix_output_verbose(8, pmix_globals.debug_output,
                        "listen_thread: active");

    while (pmix_server_globals.listen_thread_active) {
        FD_ZERO(&readfds);
        max = -1;
        PMIX_LIST_FOREACH(lt, &pmix_server_globals.listeners, pmix_listener_t) {
            FD_SET(lt->socket, &readfds);
            max = (lt->socket > max) ? lt->socket : max;
        }
        /* add the stop_thread fd */
        FD_SET(pmix_server_globals.stop_thread[0], &readfds);
        max = (pmix_server_globals.stop_thread[0] > max) ? pmix_server_globals.stop_thread[0] : max;

        /* set timeout interval */
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        /* Block in a select to avoid hammering the cpu.  If a connection
         * comes in, we'll get woken up right away.
         */
        rc = select(max + 1, &readfds, NULL, NULL, &timeout);
        if (!pmix_server_globals.listen_thread_active) {
            /* we've been asked to terminate */
            close(pmix_server_globals.stop_thread[0]);
            close(pmix_server_globals.stop_thread[1]);
            return NULL;
        }
        if (rc < 0) {
            continue;
        }

        /* Spin accepting connections until all active listen sockets
         * do not have any incoming connections, pushing each connection
         * onto the event queue for processing
         */
        do {
            accepted_connections = 0;
            PMIX_LIST_FOREACH(lt, &pmix_server_globals.listeners, pmix_listener_t) {

                /* according to the man pages, select replaces the given descriptor
                 * set with a subset consisting of those descriptors that are ready
                 * for the specified operation - in this case, a read. So we need to
                 * first check to see if this file descriptor is included in the
                 * returned subset
                 */
                if (0 == FD_ISSET(lt->socket, &readfds)) {
                    /* this descriptor is not included */
                    continue;
                }

                /* this descriptor is ready to be read, which means a connection
                 * request has been received - so harvest it. All we want to do
                 * here is accept the connection and push the info onto the event
                 * library for subsequent processing - we don't want to actually
                 * process the connection here as it takes too long, and so the
                 * OS might start rejecting connections due to timeout.
                 */
                pending_connection = PMIX_NEW(pmix_pending_connection_t);
                pending_connection->protocol = lt->protocol_type;
                event_assign(&pending_connection->ev, pmix_globals.evbase, -1,
                             EV_WRITE, connection_handler, pending_connection);
                pending_connection->sd = accept(lt->socket,
                                                (struct sockaddr*)&(pending_connection->addr),
                                                &addrlen);
                if (pending_connection->sd < 0) {
                    PMIX_RELEASE(pending_connection);
                    if (pmix_socket_errno != EAGAIN ||
                        pmix_socket_errno != EWOULDBLOCK) {
                        if (EMFILE == pmix_socket_errno) {
                            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
                        } else {
                            pmix_output(0, "listen_thread: accept() failed: %s (%d).",
                                        strerror(pmix_socket_errno), pmix_socket_errno);
                        }
                        goto done;
                    }
                    continue;
                }

                pmix_output_verbose(8, pmix_globals.debug_output,
                                    "listen_thread: new connection: (%d, %d)",
                                    pending_connection->sd, pmix_socket_errno);
                /* activate the event */
                event_active(&pending_connection->ev, EV_WRITE, 1);
                accepted_connections++;
            }
        } while (accepted_connections > 0);
    }

 done:
    pmix_server_globals.listen_thread_active = false;
    return NULL;
}

static void listener_cb(int incoming_sd, void *cbdata)
{
    pmix_pending_connection_t *pending_connection;
    pmix_listener_t *lt = (pmix_listener_t*)cbdata;

    /* throw it into our event library for processing */
    pmix_output_verbose(8, pmix_globals.debug_output,
                        "listen_cb: pushing new connection %d into evbase",
                        incoming_sd);
    pending_connection = PMIX_NEW(pmix_pending_connection_t);
    pending_connection->sd = incoming_sd;
    pending_connection->protocol = lt->protocol_type;
    event_assign(&pending_connection->ev, pmix_globals.evbase, -1,
                 EV_WRITE, connection_handler, pending_connection);
    event_active(&pending_connection->ev, EV_WRITE, 1);
}

/* Parse init-ack message:
 *    NSPACE<0><rank>VERSION<0>[CRED<0>]
 */
static pmix_status_t parse_connect_ack (char *msg, int len,
                                        char **nspace, int *rank,
                                        char **version, char **cred)
{
    int msglen;

    PMIX_STRNLEN(msglen, msg, len);
    if (msglen < len) {
        *nspace = msg;
        msg += strlen(*nspace) + 1;
        len -= strlen(*nspace) + 1;
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    PMIX_STRNLEN(msglen, msg, len);
    if (msglen <= len) {
        memcpy(rank, msg, sizeof(int));
        msg += sizeof(int);
        len -= sizeof(int);
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    PMIX_STRNLEN(msglen, msg, len);
    if (msglen < len) {
        *version = msg;
        msg += strlen(*version) + 1;
        len -= strlen(*version) + 1;
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    PMIX_STRNLEN(msglen, msg, len);
    if (msglen < len)
        *cred = msg;
    else {
        *cred = NULL;
    }

    return PMIX_SUCCESS;
}

/*  Receive the peer's identification info from a newly
 *  connected socket and verify the expected response.
 */
static pmix_status_t pmix_server_authenticate(int sd, uint16_t protocol,
                                              int *out_rank,
                                              pmix_peer_t **peer)
{
    char *msg, *nspace, *version, *cred;
    pmix_status_t rc;
    int rank;
    pmix_usock_hdr_t hdr;
    pmix_nspace_t *nptr, *tmp;
    pmix_rank_info_t *info;
    pmix_peer_t *psave = NULL;
    bool found;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "RECV CONNECT ACK FROM PEER ON SOCKET %d", sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));
    *peer = NULL;

    /* get the header */
    if (PMIX_SUCCESS != (rc = pmix_usock_recv_blocking(sd, (char*)&hdr, sizeof(pmix_usock_hdr_t)))) {
        return rc;
    }

    /* get the id, authentication and version payload (and possibly
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

    if (PMIX_SUCCESS != (rc = parse_connect_ack (msg, hdr.nbytes, &nspace,
                                                 &rank, &version, &cred))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "error parsing connect-ack from client ON SOCKET %d", sd);
        free(msg);
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack recvd from peer %s:%d:%s",
                        nspace, rank, version);

    /* do not check the version - we only retain it at this
     * time in case we need to check it at some future date.
     * For now, our intent is to retain backward compatibility
     * and so we will assume that all versions are compatible. */

    /* see if we know this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH(tmp, &pmix_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(tmp->nspace, nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        /* we don't know this namespace, reject it */
        free(msg);
        /* send an error reply to the client */
        rc = PMIX_ERR_NOT_FOUND;
        goto error;
    }

    /* see if we have this peer in our list */
    info = NULL;
    found = false;
    PMIX_LIST_FOREACH(info, &nptr->server->ranks, pmix_rank_info_t) {
        if (info->rank == rank) {
            found = true;
            break;
        }
    }
    if (!found) {
        /* rank unknown, reject it */
        free(msg);
        /* send an error reply to the client */
        rc = PMIX_ERR_NOT_FOUND;
        goto error;
    }
    *out_rank = rank;
    /* a peer can connect on multiple sockets since it can fork/exec
     * a child that also calls PMIx_Init, so add it here if necessary.
     * Create the tracker for this peer */
    psave = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(info);
    psave->info = info;
    info->proc_cnt++; /* increase number of processes on this rank */
    psave->sd = sd;
    if (0 > (psave->index = pmix_pointer_array_add(&pmix_server_globals.clients, psave))) {
        free(msg);
        PMIX_RELEASE(psave);
        /* probably cannot send an error reply if we are out of memory */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* see if there is a credential */
    if (NULL != pmix_sec.validate_cred) {
        if (PMIX_SUCCESS != (rc = pmix_sec.validate_cred(psave, cred))) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "validation of client credential failed");
            free(msg);
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            /* send an error reply to the client */
            goto error;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "client credential validated");
    }
    free(msg);

    /* execute the handshake if the security mode calls for it */
    if (NULL != pmix_sec.server_handshake) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "connect-ack executing handshake");
        rc = PMIX_ERR_READY_FOR_HANDSHAKE;
        if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&rc, sizeof(int)))) {
            PMIX_ERROR_LOG(rc);
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_sec.server_handshake(psave))) {
            PMIX_ERROR_LOG(rc);
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            return rc;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "connect-ack handshake complete");
    } else {
        /* send them success */
        rc = PMIX_SUCCESS;
        if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&rc, sizeof(int)))) {
            PMIX_ERROR_LOG(rc);
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            return rc;
        }
    }

    /* let the host server know that this client has connected */
    if (NULL != pmix_host_server.client_connected) {
        (void)strncpy(proc.nspace, psave->info->nptr->nspace, PMIX_MAX_NSLEN);
        proc.rank = psave->info->rank;
        rc = pmix_host_server.client_connected(&proc, psave->info->server_object,
                                               NULL, NULL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    /* send the client's array index */
    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&psave->index, sizeof(int)))) {
        PMIX_ERROR_LOG(rc);
        pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
        PMIX_RELEASE(psave);
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack from client completed");

    *peer = psave;
    return rc;

  error:
    /* send an error reply to the client */
    if (PMIX_SUCCESS != pmix_usock_send_blocking(sd, (char*)&rc, sizeof(int))) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int sd, short flags, void* cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t*)cbdata;
    pmix_peer_t *peer;
    int rank;

    pmix_output_verbose(8, pmix_globals.debug_output,
                        "connection_handler: new connection: %d",
                        pnd->sd);

    /* ensure the socket is in blocking mode */
    pmix_usock_set_blocking(pnd->sd);

    /* receive identifier info from the client and authenticate it - the
     * function will lookup and return the peer object if the connection
     * is successfully authenticated */
    if (PMIX_SUCCESS != pmix_server_authenticate(pnd->sd, pnd->protocol,
                                                 &rank, &peer)) {
        CLOSE_THE_SOCKET(pnd->sd);
        return;
    }
    pmix_usock_set_nonblocking(pnd->sd);

    /* start the events for this client */
    event_assign(&peer->recv_event, pmix_globals.evbase, pnd->sd,
                 EV_READ|EV_PERSIST, pmix_usock_recv_handler, peer);
    event_add(&peer->recv_event, NULL);
    peer->recv_ev_active = true;
    event_assign(&peer->send_event, pmix_globals.evbase, pnd->sd,
                 EV_WRITE|EV_PERSIST, pmix_usock_send_handler, peer);
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server client %s:%d has connected on socket %d",
                        peer->info->nptr->nspace, peer->info->rank, peer->sd);
    PMIX_RELEASE(pnd);
}

