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
	if(!pmix_server_globals.listen_thread_active) {
            if (0 > pthread_create(&engine, NULL, listen_thread, NULL)) {
                pmix_server_globals.listen_thread_active = false;
                return PMIX_ERROR;
            }
	    else {
                pmix_server_globals.listen_thread_active = true;
	    }
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

/* process the callback with tool connection info */
static void process_cbfunc(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t*)cbdata;
    pmix_nspace_t *nptr;
    pmix_rank_info_t *info;
    int rc;

    /* if the request failed, then reject the request - that
     * will be the requestor's signal we aren't accepting it */
    if (PMIX_SUCCESS != pnd->status) {
        /* send an error reply to the client */
        if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(pnd->sd, (char*)&pnd->status, sizeof(int)))) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_RELEASE(pnd);
        return;
    }

    pmix_usock_set_nonblocking(pnd->sd);

    /* add this nspace to our pool */
    nptr = PMIX_NEW(pmix_nspace_t);
    (void)strncpy(nptr->nspace, pnd->nspace, PMIX_MAX_NSLEN);
    nptr->server = PMIX_NEW(pmix_server_nspace_t);
    pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    /* add this tool rank to the nspace */
    info = PMIX_NEW(pmix_rank_info_t);
    PMIX_RETAIN(nptr);
    info->nptr = nptr;
    info->rank = 0;
    pmix_list_append(&nptr->server->ranks, &info->super);

    /* setup a peer object for this tool */
    pmix_peer_t *peer = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(info);
    peer->info = info;
    peer->proc_cnt = 1;
    peer->sd = pnd -> sd;
    if (0 > (peer->index = pmix_pointer_array_add(&pmix_server_globals.clients, peer))) {
        PMIX_RELEASE(pnd);
        PMIX_RELEASE(peer);
        pmix_list_remove_item(&pmix_globals.nspaces, &nptr->super);
        PMIX_RELEASE(nptr);  // will release the info object
        /* probably cannot send an error reply if we are out of memory */
        return;
    }

    /* start the events for this client */
    event_assign(&peer->recv_event, pmix_globals.evbase, pnd->sd,
                 EV_READ|EV_PERSIST, pmix_usock_recv_handler, peer);
    event_add(&peer->recv_event, NULL);
    peer->recv_ev_active = true;
    event_assign(&peer->send_event, pmix_globals.evbase, pnd->sd,
                 EV_WRITE|EV_PERSIST, pmix_usock_send_handler, peer);
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server tool %s:%d has connected on socket %d",
                        peer->info->nptr->nspace, peer->info->rank, peer->sd);
}

/* receive a callback from the host RM with an nspace
 * for a connecting tool */
static void cnct_cbfunc(pmix_status_t status,
                        char *nspace, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t*)cbdata;
    pmix_info_t info[14];
    int i,ninfo = 0;
    char hostname[PMIX_MAX_NSLEN+1];
    char * nodemap_regex, *procmap_regex;
    pmix_info_array_t info_array;

    gethostname(hostname, PMIX_MAX_NSLEN);

    /* save the nspace and status */
    pnd->status = status;
    (void)strncpy(pnd->nspace, nspace, PMIX_MAX_NSLEN);
    
    /* Send nspace, rank and status back to the tool. */
    pmix_usock_send_blocking(pnd->sd, (char *)&pnd->status, sizeof(pmix_status_t));
    if(pnd -> status != PMIX_SUCCESS)
      return;

    pmix_usock_send_blocking(pnd->sd, (char*)nspace, PMIX_MAX_NSLEN+1);
    pmix_usock_send_blocking(pnd->sd, (char*)&pmix_globals.myid.rank, sizeof(pmix_globals.myid.rank));

    /* need to thread-shift this into our context */
    event_assign(&pnd->ev, pmix_globals.evbase, -1,
                 EV_WRITE, process_cbfunc, pnd);
    event_active(&pnd->ev, EV_WRITE, 1);

    /*
     * Setup all the info that would normally be set by the RM
     */

#if 0
/*
 * JOBID doesn't make much sense unless RM can tell us what job we are part of, but RM 
 * maybe didn't create us
 */
    memcpy(info[ninfo].key, PMIX_JOBID, sizeof(info[ninfo].key));
    info[ninfo].value.data.string = strdup(nspace->name);
    info[ninfo].value.type = PMIX_STRING;
    ninfo++;
#endif

    memcpy(info[ninfo].key, PMIX_NPROC_OFFSET, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 0;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;

    PMIx_generate_regex(hostname, &nodemap_regex);

    memcpy(info[ninfo].key, PMIX_NODE_MAP, sizeof(info[ninfo].key));
    info[ninfo].value.data.string = nodemap_regex;
    info[ninfo].value.type = PMIX_STRING;
    ninfo++;

    PMIx_generate_ppn("0", &procmap_regex);

    memcpy(info[ninfo].key, PMIX_PROC_MAP, sizeof(info[ninfo].key));
    info[ninfo].value.data.string = procmap_regex;
    info[ninfo].value.type = PMIX_STRING;
    ninfo++;

#if 0
/*
 * RM would have to provide this
 */
    memcpy(info[ninfo].key, PMIX_NODEID, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32= 0;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;
#endif

    memcpy(info[ninfo].key, PMIX_NODE_SIZE, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 1;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;

    memcpy(info[ninfo].key, PMIX_LOCAL_PEERS, sizeof(info[ninfo].key));
    info[ninfo].value.data.string = strdup("0");
    info[ninfo].value.type = PMIX_STRING;
    ninfo++;
#if 0
/*
 * No way to know (even RM would not know this)
 */
    memcpy(info[ninfo].key, PMIX_LOCAL_CPUSETS, sizeof(info[ninfo].key));
    info[ninfo].value.data.string = strdup("0");
    info[ninfo].value.type = PMIX_STRING;
    ninfo++;
#endif
    memcpy(info[ninfo].key, PMIX_LOCALLDR, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 0;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;

    memcpy(info[ninfo].key, PMIX_UNIV_SIZE, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 1;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;
/*
 * The settings of JOB_SIZE and LOCAL_SIZE are questionable.  There could be lots of 
 * other processes in this "job", but we don't know what "job" this process is part of
 * Maybe it is better to not set anything for these rather than using "1"?
 */
    memcpy(info[ninfo].key, PMIX_JOB_SIZE, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 1;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;

    memcpy(info[ninfo].key, PMIX_LOCAL_SIZE, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 1;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;

/*
 * Don't know how this is used.  If it is used to decide how many processes
 * you may spawn, then the RM will have to provide this
 */
#if 0
    memcpy(info[ninfo].key, PMIX_MAX_PROCS, sizeof(info[ninfo].key));
    info[ninfo].value.data.uint32 = 1;
    info[ninfo].value.type = PMIX_UINT32;
    ninfo++;
#endif

    info_array.array= (pmix_info_t*) malloc(sizeof(info[0]) * 8);

    memcpy(info[ninfo].key, PMIX_PROC_DATA, PMIX_MAX_KEYLEN);
    info[ninfo].value.data.array = info_array;
    info[ninfo].value.type = PMIX_INFO_ARRAY;
    
    i = 0;

    memcpy(info_array.array[i].key, PMIX_RANK, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.integer = 0;
    info_array.array[i].value.type = PMIX_INT;
    i++;

    memcpy(info_array.array[i].key, PMIX_APPNUM, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.uint32 = 0;
    info_array.array[i].value.type = PMIX_UINT32;
    i++;

    memcpy(info_array.array[i].key, PMIX_APPLDR, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.uint32 = 0;
    info_array.array[i].value.type = PMIX_UINT32;
    i++;

    memcpy(info_array.array[i].key, PMIX_GLOBAL_RANK, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.uint32 = 0;
    info_array.array[i].value.type = PMIX_UINT32;
    i++;

    memcpy(info_array.array[i].key ,PMIX_APP_RANK, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.uint32 = 0;
    info_array.array[i].value.type = PMIX_UINT32;
    i++;
/*
 * More really fuzzy settings that are probably incorrect
 */
    memcpy(info_array.array[i].key, PMIX_LOCAL_RANK, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.uint16 = 0;
    info_array.array[i].value.type = PMIX_UINT16;
    i++;

    memcpy(info_array.array[i].key, PMIX_NODE_RANK, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.uint16 = 0;
    info_array.array[i].value.type = PMIX_UINT16;
    i++;

    memcpy(info_array.array[i].key, PMIX_HOSTNAME, PMIX_MAX_KEYLEN);
    info_array.array[i].value.data.string = strdup(hostname);
    info_array.array[i].value.type = PMIX_STRING;
    i++;

    info_array.size = i;
    info[ninfo].value.data.size = i;
    ninfo++;

    PMIx_server_register_nspace(nspace, 1, info, ninfo, NULL, NULL);
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
    pmix_pending_connection_t *pnd = PMIX_NEW(pmix_pending_connection_t);

    memset(pnd, 0, sizeof(pmix_pending_connection_t));
    
    pnd -> sd = sd;


    pmix_output_verbose(2, pmix_globals.debug_output,
                        "RECV CONNECT ACK FROM PEER ON SOCKET %d",
                        sd);

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
                            "unable to complete recv of connect-ack with client ON SOCKET %d",
                            sd);
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
    
    pmix_globals.myid.rank = rank;
    /* for connections, the tag in the header tells us if the
     * caller is a client process or a tool seeking to connect.
     * If the latter, then the tool will not know its nspace/rank
     * and we will need to provide it */
    if (0 != hdr.tag) {
        /* this is a tool requesting connection - does this
         * server support that operation? */
        if (NULL == pmix_host_server.tool_connected) {
            /* reject the request */
            free(msg);
            /* send an error reply to the requestor */
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto error;
        }
        /* request an nspace for this requestor - it will
         * automatically be assigned rank=0 */
          pnd->msg = msg;
        pmix_host_server.tool_connected(NULL, 0, (pmix_tool_connection_cbfunc_t) cnct_cbfunc, pnd);
        return PMIX_ERR_OPERATION_IN_PROGRESS;
    }

    /* get the nspace */
    nspace = msg;  // a NULL terminator is in the data

    /* get the rank */
    memcpy(&rank, msg+strlen(nspace)+1, sizeof(int));


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
    psave->sd = pnd -> sd;
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
    /* let the host server know that this client has connected */
    if (NULL != pmix_host_server.client_connected) {
        (void)strncpy(proc.nspace, psave->info->nptr->nspace, PMIX_MAX_NSLEN);
        proc.rank = psave->info->rank;
        rc = pmix_host_server.client_connected(&proc, psave->info->server_object);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
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
    pmix_status_t status;
    pmix_output_verbose(8, pmix_globals.debug_output,
                        "connection_handler: new connection: %d",
                        pnd->sd);

    /* ensure the socket is in blocking mode */
    pmix_usock_set_blocking(pnd->sd);

    /* 
     * Receive identifier info from the client and authenticate it - the
     * function will lookup and return the peer object if the connection
     * is successfully authenticated */
    if (PMIX_SUCCESS != (status = pmix_server_authenticate(pnd->sd, pnd->protocol,
                                                 &rank, &peer))) {
        if(status != PMIX_ERR_OPERATION_IN_PROGRESS)
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

