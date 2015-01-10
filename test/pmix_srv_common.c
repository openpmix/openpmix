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
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */


#define _GNU_SOURCE
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
extern int errno;
#include <errno.h>
#include "src/util/argv.h"

#include "test_common.h"
#include "pmix_srv_common.h"

int blocking_recv(int sd, void *buf, int size)
{
    int rc;
    int rcvd = 0;
    while( rcvd < size ){
        rc = read(sd, buf, size);
        if( rc < 0 ){
            if( errno == EAGAIN ){
                continue;
            } else {
                fprintf(stderr, "PMIx srv: %s:%d read() failed %d:%s",
                        __FILE__, __LINE__, errno, strerror(errno));
                return PMIX_ERR_UNREACH;
            }
        }else if( rc == 0){
            fprintf(stderr, "PMIx srv: %s:%d read() failed: client closed connection",
                    __FILE__, __LINE__);
            return PMIX_ERR_UNREACH;
        }
        rcvd += rc;
    }
    return PMIX_SUCCESS;
}

/* Copy from src/client/usock.c */
int blocking_send(int sd, void *ptr, size_t size)
{
    size_t cnt = 0;
    int retval;

    while (cnt < size) {
        retval = send(sd, (char*)ptr+cnt, size-cnt, 0);
        if (retval < 0) {
            if ( errno != EINTR ) {
                fprintf(stderr, "PMIx srv: %s:%d send() to socket %d failed: %d:%s",
                        __FILE__, __LINE__, sd, errno, strerror(errno));
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    return PMIX_SUCCESS;
}

/*
 * start listening on our rendezvous file
 */
int start_listening(struct sockaddr_un *address)
{
    int flags;
    unsigned int addrlen;
    int sd = -1;

    /* unlink old unix socket file if presents */
    if (0 == access(address->sun_path, R_OK)) {
        unlink(address->sun_path);
    }

    /* create a listen socket for incoming connection attempts */
    sd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sd < 0) {
        fprintf(stderr, "PMIx srv: %s:%d socket() failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(sd, (struct sockaddr*)address, addrlen) < 0) {
        fprintf(stderr, "PMIx srv: %s:%d bind() failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        return -1;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(sd, SOMAXCONN) < 0) {
        fprintf(stderr, "PMIx srv: %s:%d listen() failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        return -1;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        fprintf(stderr, "PMIx srv: %s:%d fcntl(F_GETFL) failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sd, F_SETFL, flags) < 0) {
        fprintf(stderr, "PMIx srv: %s:%d fcntl(F_SETFL) failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));

        return -1;
    }

    /* record this socket */
    return sd;
}

static void exit_handler(int incomind_sd, short flags, void* cbdata)
{
    int *service = (int*)cbdata;
    fprintf(stderr, "PMIx srv: Caught signal from the client. Exit\n");
    *service = false;
}

int run_client(struct sockaddr_un address, struct event_base *base,
               char **env, int *service)
{
    char **client_env = NULL;
    char **client_argv = NULL;
    char *tmp;
    int pid;
    int i;

    // setup signal handler to be notified on error
    // at client exec
    pmix_event_t *ev = evsignal_new(base, SIGUSR1,
                                    exit_handler, (void*)service);
    event_add(ev,NULL);
    fprintf(stderr, "PMIx srv: SIGUSR1 handler was intalled\n");

    /* define the argv for the test client */
    pmix_argv_append_nosize(&client_argv, "pmix_client");

    for(i=0; NULL != env[i]; i++){
        pmix_argv_append_nosize(&client_env, env[i]);
    }
    /* pass our URI */
    asprintf(&tmp, "PMIX_SERVER_URI=0:%s", address.sun_path);
    pmix_argv_append_nosize(&client_env, tmp);
    free(tmp);
    /* pass an ID for the client */
    pmix_argv_append_nosize(&client_env, "PMIX_RANK=1");
    /* pass a security credential */
    asprintf(&tmp, "PMIX_NAMESPACE=smoky_namespace");
    pmix_argv_append_nosize(&client_env, tmp);
    free(tmp);

    fprintf(stderr, "PMIx srv: Client environment was initialized\n");

    /* fork/exec the test */
    pid = fork();
    if( pid < 0 ){
        printf("Cannot fork()\n");
        return -1;
    }else if( pid == 0 ){
        /* setup environment */
        execve("./pmix_client", client_argv, client_env);
        // shouldn't get here
        printf("Cannot execle the client, rc = %d (%s)\n",
               errno, strerror(errno));
        fflush(stderr);
        // Let parent know
        pid_t ppid = getppid();
        kill(ppid,SIGUSR1);
        abort();
    }

    fprintf(stderr, "PMIx srv: Client was spawned\n");

    return pid;
}

int reply_cli_auth(int sd, int rc)
{
    pmix_usock_hdr_t hdr;

    hdr.nbytes = 4;
    hdr.rank = pmix_globals.rank;
    hdr.type = PMIX_USOCK_IDENT_PMIX;
    hdr.tag = 0; // TODO: do we need to put other value here?

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&hdr, sizeof(hdr)))) {
        PMIX_ERROR_LOG(rc);
    }

    return blocking_send(sd,&rc,sizeof(rc));
}

int verify_kvps(void *blob)
{
    pmix_buffer_t *xfer = (pmix_buffer_t *)blob;
    pmix_buffer_t *bptr;
    pmix_scope_t scope = 0;
    int rc, cnt = 1;
    pmix_kval_t *kvp = NULL;
    int collect_data, barrier;

    // We expect 3 keys here:
    // 1. for the local scope:  "local-key-2" = "12342" (INT)
    // 2. for the remote scope: "remote-key-2" = "Test string #2" (STRING)
    // 3. for the global scope: "global-key-2" = "12.15" (FLOAT)
    // Should test more cases in future

    cnt = 1;
    if( PMIX_SUCCESS != pmix_bfrop.unpack(xfer,&collect_data,&cnt, PMIX_INT) ){
        fprintf(stderr,"PMIx srv [%s:%d]: Cannot unpack\n", __FILE__,__LINE__);
        abort();
    }

    cnt = 1;
    if( PMIX_SUCCESS != pmix_bfrop.unpack(xfer,&barrier,&cnt, PMIX_INT) ){
        fprintf(stderr,"PMIx srv [%s:%d]: Cannot unpack\n", __FILE__,__LINE__);
        abort();
    }

    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(xfer, &scope, &cnt, PMIX_SCOPE))) {
        /* unpack the buffer */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(xfer, &bptr, &cnt, PMIX_BUFFER))) {
            fprintf(stderr,"PMIx srv [%s:%d]: Cannot unpack\n", __FILE__,__LINE__);
            abort();
        }

        /* process the input */
        if (PMIX_LOCAL == scope) {
            int keys = 0;
            while( 1 ){
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &kvp, &cnt, PMIX_KVAL))) {
                    break;
                }
                keys++;
                if( keys == 1){
                    if( strcmp(kvp->key, "local-key-2") || kvp->value->type != PMIX_INT
                            || kvp->value->data.integer != 12342 ){
                        fprintf(stderr,"PMIx srv [verify_kvps]: Error receiving LOCAL key\n");
                        abort();
                    }
                }
            }
        } else if (PMIX_REMOTE == scope) {
            int keys = 0;
            while( 1 ){
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &kvp, &cnt, PMIX_KVAL))) {
                    break;
                }
                keys++;
                if( keys == 1){
                    if( strcmp(kvp->key, "remote-key-2") || kvp->value->type != PMIX_STRING
                            || strcmp(kvp->value->data.string,"Test string #2") ){
                        fprintf(stderr,"Error receiving REMOTE key\n");
                        abort();
                    }
                }
            }
        } else {
            int keys = 0;
            while( 1 ){
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &kvp, &cnt, PMIX_KVAL))) {
                    break;
                }
                keys++;
                if( keys == 1){
                    if( strcmp(kvp->key, "global-key-2") || kvp->value->type != PMIX_FLOAT ||
                            kvp->value->data.fval != (float)12.15 ){
                        fprintf(stderr,"Error receiving GLOBAL key\n");
                        abort();
                    }
                }
            }
        }
        cnt = 1;
    }
    // TODO: Return an error at error :)
    return PMIX_SUCCESS;
}
