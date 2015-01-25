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
 * Copyright (c) 2013-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <event.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "src/include/types.h"
#include "src/usock/usock.h"
#include "src/api/pmix_common.h"
#include "src/buffer_ops/types.h"
#include "src/util/error.h"
#include <errno.h>
#include "src/util/argv.h"

#include "test_common.h"
#include "pmix_server.h"

pmix_modex_data_t **pmix_db = NULL;
struct event_base *server_base = NULL;
pmix_event_t *listen_ev = NULL;
int listen_fd = -1;
int client_fd = -1;
pid_t client_pid = -1;

static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);

pmix_event_t *cli_ev, *exit_ev;
pmix_usock_hdr_t hdr;
char *data = NULL;
bool hdr_rcvd = false;
char *rptr;
size_t rbytes = 0;

int service = true;

int main(int argc, char **argv, char **env)
{
    struct sockaddr_un address;

    fprintf(stderr,"Start PMIx_lite smoke test\n");

    if (0 != (rc = PMIx_server_init(&mymodule, false))) {
        fprintf(stderr, "PMIx_init failed with error %d\n", rc);
        exit(rc);
    }
    
    /* initialize the event library - we will be providing messaging support
     * for the server */
    if (NULL == (server_base = event_base_new())) {
        printf("Cannot create libevent event\n");
        return 1;
    }
    
    /* setup the path to the rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "pmix");

    fprintf(stderr,"PMIx srv: Initialization finished\n");

    /* start listening */
    listen_fd = start_listening(&address);

    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    fprintf(stderr, "PMIx srv: Server is listening for incoming connections\n");


    /* fork/exec the test */
    if( 0 > (client_pid = run_client(address, server_base, env, &service) ) ){
        fprintf(stderr,"PMIx srv: Cannot spawn the client\n");
        exit(0);
    }

    /* cycle the event library until complete */
    while( service ){
         event_base_loop(server_base, EVLOOP_ONCE);
    }
    fprintf(stderr,"PMIx srv: exit service loop. wait() for the client termination\n");
    wait(NULL);
    return 0;
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
static void connection_handler(int incomind_sd, short flags, void* cbdata)
{
    int rc, sd;

    fprintf(stderr, "PMIx srv: Incoming connection from the client\n");

    if( 0 > (sd = accept(incomind_sd,NULL,0)) ){
        fprintf(stderr, "PMIx srv: %s:%d accept() failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        exit(0);
    }

    /* authenticate the connection */
    if (PMIX_SUCCESS != (rc = PMIx_server_authenticate_client(sd, snd_message))) {
        printf("PMIx srv: Bad authentification!\n");
        exit(0);
    }

    cli_ev = event_new(server_base, sd,
                      EV_READ|EV_PERSIST, message_handler, NULL);
    event_add(cli_ev,NULL);
    pmix_usock_set_nonblocking(sd);
    client_fd = sd;
    printf("PMIx srv: New client connection accepted\n");
}


static void message_handler(int sd, short flags, void* cbdata)
{
    int rc;
    if( !hdr_rcvd && rptr == NULL ){
        rptr = (char*)&hdr;
        rbytes = sizeof(hdr);
    }

    if( !hdr_rcvd ){
        /* if the header hasn't been completely read, read it */
        rc = blocking_recv(sd, rptr, rbytes);
        /* Process errors first (if any) */
        if ( PMIX_ERR_UNREACH == rc ) {
            /* Communication error */
            fprintf(stderr,"PMIx srv: Client closed connection\n");
            event_del(cli_ev);
            event_free(cli_ev);
            close(client_fd);
            client_fd = -1;
            return;
        } else if( PMIX_ERR_RESOURCE_BUSY == rc ||
                   PMIX_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
        }
        /* completed reading the header */
        hdr_rcvd = true;

        /* if this is a zero-byte message, then we are done */
        if (0 == hdr.nbytes) {
            data = NULL;  /* Protect the data */
            rptr = NULL;
            rbytes = 0;
        } else {
            data = (char*)malloc(hdr.nbytes);
            if (NULL == data ) {
                fprintf(stderr, "PMIx srv: %s:%d Cannot allocate memory! %d",
                        __FILE__, __LINE__, errno);
                return;
            }
            /* point to it */
            rptr = data;
            rbytes = hdr.nbytes;
        }
    }

    if (hdr_rcvd) {
        /* continue to read the data block - we start from
             * wherever we left off, which could be at the
             * beginning or somewhere in the message
             */
        if (PMIX_SUCCESS == (rc = blocking_recv(sd, rptr, rbytes))) {
            /* we recvd all of the message */
            /* process the message */
            process_message();
        } else if( PMIX_ERR_RESOURCE_BUSY == rc ||
                   PMIX_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
        } else {
            /* Communication error */
            fprintf(stderr, "PMIx srv: %s:%d Error communicating with the client",
                    __FILE__, __LINE__);
            kill(client_pid, SIGKILL);
            // TODO: make sure to kill the client
            exit(0);
        }
    }
}
