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
#include "src/client/usock.h"
#include "src/client/pmix_client.h"
#include "src/include/constants.h"
#include "src/buffer_ops/constants.h"
#include "src/util/error.h"


struct event_base *server_base = NULL;
struct event *listen_ev = NULL;
int listen_fd = -1;
static int start_listening(struct sockaddr_un *address);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);
pmix_usock_recv_t rcvd;
bool service = true;

int init_sendrcvd()
{
    rcvd.hdr_recvd = false;
    rcvd.data = NULL;
    rcvd.rdptr = NULL;
    rcvd.rdbytes = 0;
    rcvd.ev = NULL;
}

/*
 * Initialize global variables used w/in the server.
 */
int main(int argc, char **argv)
{
    struct sockaddr_un address;
    char **client_env=NULL;
    char **client_argv=NULL;

    /* initialize the event library */
    if (NULL == (server_base = event_base_new())) {
        printf("Cannot create libevent event\n");
        return 0;
    }
    
    /* setup the path to the rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "pmix");

    /* start listening */
    start_listening(&address);

    
    /* define the argv for the test client */
    //pmix_argv_append_nosize(&client_argv, "client");

    /* pass our URI */
    //asprintf(&tmp, "PMIX_SERVER_URI=0:%s", address.sun_path);
    //pmix_argv_append_nosize(&client_env, tmp);
    //free(tmp);
    /* pass an ID for the client */
    //pmix_argv_append_nosize(&client_env, "PMIX_ID=1");
    /* pass a security credential */
    //pmix_argv_append_nosize(&client_env, "PMIX_SEC_CRED=credential-string");
    
    /* fork/exec the test */

    /* cycle the event library until complete */
    while( service ){
         event_base_loop(server_base, EVLOOP_ONCE);
    }
    
    return 0;
}

/*
 * start listening on our rendezvous file
 */
static int start_listening(struct sockaddr_un *address)
{
    int flags;
    unsigned int addrlen;
    int sd = -1;

    /* create a listen socket for incoming connection attempts */
    sd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sd < 0) {
        printf("%s:%d socket() failed", __FILE__, __LINE__);
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(sd, (struct sockaddr*)address, addrlen) < 0) {
        printf("%s:%d bind() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* setup listen backlog to maximum allowed by kernel */
    if (listen(sd, SOMAXCONN) < 0) {
        printf("%s:%d listen() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        printf("%s:%d fcntl(F_GETFL) failed", __FILE__, __LINE__);
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sd, F_SETFL, flags) < 0) {
        printf("%s:%d fcntl(F_SETFL) failed", __FILE__, __LINE__);
        return -1;
    }

    /* record this socket */
    listen_fd = sd;

    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    printf("Server is listening for incoming connections\n");
    return 0;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incomind_sd, short flags, void* cbdata)
{
    int rc, sd;
    struct sockaddr_un sa;
    int sa_len = sizeof(struct sockaddr_un);
    struct event *ev = NULL;
    if( 0 > (sd = accept(incomind_sd,NULL,0)) ){
        printf("accept() failed");
        exit(0);
    }

    /* receive ack from the server */
    if (PMIX_SUCCESS != (rc = usock_recv_connect_ack(sd))) {
        printf("Bad acknoledgement!\n");
        exit(0);
    }

    /* send our globally unique process identifier to the server */
    if (PMIX_SUCCESS != (rc = usock_send_connect_ack(sd))) {
        printf("Cannot send my acknoledgement!\n");
        exit(0);
    }
    rcvd.ev = event_new(server_base, sd, EV_READ|EV_PERSIST, message_handler, &rcvd);
    event_add(rcvd.ev,0);
    printf("New client connection accepted\n");
}
