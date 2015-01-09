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

pmix_modex_data_t **pmix_db = NULL;
struct event_base *server_base = NULL;
pmix_event_t *listen_ev = NULL;
int listen_fd = -1;
int client_fd = -1;
pid_t client_pid = -1;
static int start_listening(struct sockaddr_un *address);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);
static int read_bytes(int sd, char **buf, size_t *remain);
int blocking_recv(int sd, void *buf, int size);

int reply_cli_auth(int sd, int rc);
int recv_cli_auth(int sd);

pmix_event_t *cli_ev, *exit_ev;
pmix_usock_hdr_t hdr;
char *data = NULL;
bool hdr_rcvd = false;
char *rptr;
size_t rbytes = 0;

bool service = true;

void process_message(void);
void prepare_db(void);
int run_client(struct sockaddr_un address, char **env);

/*
 * Initialize global variables used w/in the server.
 */
int main(int argc, char **argv, char **env)
{
    struct sockaddr_un address;

    fprintf(stderr,"Start PMIx smoke test\n");

    pmix_bfrop_open();
    /* initialize the event library */
    if (NULL == (server_base = event_base_new())) {
        printf("Cannot create libevent event\n");
        return 0;
    }
    /* prepare database */
    prepare_db();
    
    /* setup the path to the rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "pmix");

    fprintf(stderr,"PMIx srv: Initialization finished\n");

    /* start listening */
    start_listening(&address);

    /* fork/exec the test */
    if( run_client(address, env) ){
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
    listen_fd = sd;

    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    fprintf(stderr, "PMIx srv: Server is listening for incoming connections\n");
    return 0;
}

static void exit_handler(int incomind_sd, short flags, void* cbdata)
{
    fprintf(stderr, "PMIx srv: Caught signal from the client. Exit\n");
    service = false;
}

int run_client(struct sockaddr_un address, char **env)
{
    char **client_env = NULL;
    char **client_argv = NULL;
    char *tmp;
    int i;

    // setup signal handler to be notified on error
    // at client exec
    pmix_event_t *ev = evsignal_new(server_base, SIGUSR1, exit_handler, NULL);
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
    client_pid = fork();
    if( client_pid < 0 ){
        printf("Cannot fork()\n");
        return -1;
    }else if( client_pid == 0 ){
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

    return 0;
}

int blocking_recv(int sd, void *buf, int size)
{
    int rc;
    int rcvd = 0;
    while( rcvd < size ){
        rc = read(sd, buf, size);
        if( rc < 0 ){
            fprintf(stderr, "PMIx srv: %s:%d read() failed %d:%s",
                    __FILE__, __LINE__, errno, strerror(errno));
            return PMIX_ERR_UNREACH;
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
static int blocking_send(int sd, void *ptr, size_t size)
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

int reply_cli_auth(int sd, int rc)
{
    return blocking_send(sd,&rc,sizeof(rc));
}

int recv_cli_auth(int sd)
{
    pmix_usock_hdr_t hdr;
    char *data;
    char str[256];
    int rc, len;

    if( PMIX_SUCCESS != (rc = blocking_recv(sd,&hdr,sizeof(hdr))) ){
        return rc;
    }
    data = malloc(hdr.nbytes);
    if( PMIX_SUCCESS != (rc = blocking_recv(sd,data, hdr.nbytes)) ){
        return rc;
    }
    len = strlen(PMIX_VERSION);
    memcpy(str,data,len);
    str[len] = '\0';
    if( strcmp(PMIX_VERSION, str) ){
        fprintf(stderr, "PMIx srv: %s:%d pmix version mismatch: expect \"%s\", get \"%s\"",
                __FILE__, __LINE__, PMIX_VERSION, str);
        rc = PMIX_ERR_BAD_PARAM;
        reply_cli_auth(sd, rc);
        return rc;
    }
    memcpy(str,data + len + 1,hdr.nbytes - len - 1);
    len = hdr.nbytes - len - 1;
    str[len] = '\0';
    if( strcmp(TEST_CREDENTIAL, str) ){
        fprintf(stderr, "PMIx srv: %s:%d pmix credential mismatch: expect \"%s\", get \"%s\"",
                __FILE__, __LINE__, PMIX_VERSION, str);
        rc = PMIX_ERR_BAD_PARAM;
        reply_cli_auth(sd, rc);
        return rc;
    }
    rc = PMIX_SUCCESS;
    reply_cli_auth(sd, rc);
    return rc;
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

    /* receive ack from the server */
    if (PMIX_SUCCESS != (rc = recv_cli_auth(sd))) {
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
        rc = read_bytes(sd, &rptr, &rbytes);
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
        if (PMIX_SUCCESS == (rc = read_bytes(sd, &rptr, &rbytes))) {
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

static int read_bytes(int sd, char **buf, size_t *remain)
{
    int ret = PMIX_SUCCESS, rc;
    char *ptr = *buf;

    /* read until all bytes recvd or error */
    while (0 < *remain) {
        rc = read(sd, ptr, *remain);
        if (rc < 0) {
            if(errno == EINTR) {
                continue;
            } else if (errno == EAGAIN) {
                ret = PMIX_ERR_RESOURCE_BUSY;
                goto exit;
            } else if (errno == EWOULDBLOCK) {
                ret = PMIX_ERR_WOULD_BLOCK;
                goto exit;
            }
            ret = PMIX_ERR_UNREACH;
            goto exit;
        } else if (0 == rc) {
            /* the remote peer closed the connection */
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

void prepare_db(void)
{
    int i;

    pmix_db = malloc(sizeof(pmix_modex_data_t*) * 3);
    for (i=0;i<3;i++){
        pmix_buffer_t *buf;
        pmix_value_t val;
        pmix_kval_t kv, *kvp = &kv;
        char sval[256], key[256];

        pmix_db[i] = malloc(sizeof(pmix_modex_data_t));
        strcpy(pmix_db[i]->namespace, TEST_NAMESPACE);
        pmix_db[i]->rank = i;

        /* Construct the buffer */
        buf = OBJ_NEW(pmix_buffer_t);

        /* Create local data  */
        sprintf(key,"local-key-%d",i);
        PMIX_VAL_SET(&val, int, 12340+i);
        kv.key = key;
        kv.value = &val;
        pmix_bfrop.pack(buf,&kvp, 1, PMIX_KVAL);
        PMIX_VAL_FREE(&val);

        /* Create remote data  */
        sprintf(key,"remote-key-%d",i);
        sprintf(sval,"Test string #%d",i);
        PMIX_VAL_SET(&val,string, sval);
        kv.key = key;
        kv.value = &val;
        pmix_bfrop.pack(buf,&kvp, 1, PMIX_KVAL);
        PMIX_VAL_FREE(&val);

        /* Create global data  */
        sprintf(key,"global-key-%d",i);
        PMIX_VAL_SET(&val,float, 10.15 + i);
        kv.key = key;
        kv.value = &val;
        pmix_bfrop.pack(buf,&kvp, 1, PMIX_KVAL);
        PMIX_VAL_FREE(&val);

        pmix_db[i]->blob = (uint8_t*)buf->base_ptr;
        pmix_db[i]->size = buf->bytes_used;
        buf->base_ptr = NULL;
        OBJ_RELEASE(buf);
    }
}

static void reply_to_client(pmix_buffer_t *reply, uint8_t type, uint32_t tag)
{
    int rc;
    pmix_usock_hdr_t hdr;
    hdr.nbytes = reply->bytes_used;
    hdr.tag = tag;
    hdr.type = type;

    rc = blocking_send(client_fd, &hdr, sizeof(hdr));
    if( PMIX_SUCCESS != rc ){
        pmix_output(0,"PMIx srv: Error sending the header to the client");
        return;
    }

    rc = blocking_send(client_fd,reply->unpack_ptr,reply->bytes_used);
    if( PMIX_SUCCESS != rc ){
        pmix_output(0,"PMIx srv: Error sending the header to the client");
        return;
    }
}

inline static int verify_kvps(pmix_buffer_t *xfer)
{
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

void process_message(void)
{
    int rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_buffer_t *reply = NULL;
    pmix_buffer_t xfer;
    uint32_t tag;
    uint8_t type;

    /* xfer the message to a buffer for unpacking */
    OBJ_CONSTRUCT(&xfer, pmix_buffer_t);
    xfer.base_ptr = (char*)data;
    xfer.bytes_allocated = xfer.bytes_used = hdr.nbytes;
    xfer.unpack_ptr = xfer.base_ptr;
    xfer.pack_ptr = ((char*)xfer.base_ptr) + xfer.bytes_used;

    type = hdr.type;
    tag = hdr.tag;
    /* protect the transferred data */
    data = NULL;
    hdr_rcvd = false;
    rptr = NULL;
    rbytes = 0;

    /* retrieve the cmd */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&xfer, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        OBJ_DESTRUCT(&xfer);
        return;
    }
//    opal_output_verbose(2, pmix_server_output,
//                        "%s recvd pmix cmd %d from %s",
//                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd, ORTE_NAME_PRINT(&name));

    /* get the job and proc objects for the sender */
//    memcpy((char*)&name, (char*)&id, sizeof(orte_process_name_t));

    switch(cmd) {
    case PMIX_FINALIZE_CMD:
        fprintf(stderr,"PMIx srv: PMIx_Finalize() was called by the client\n");
        service = false;
        reply = OBJ_NEW(pmix_buffer_t);
        break;
    case PMIX_JOBINFO_CMD:{
        reply = OBJ_NEW(pmix_buffer_t);
        pmix_kval_t *kp = OBJ_NEW(pmix_kval_t);
        kp->value = malloc(sizeof(pmix_value_t));
        int rc = PMIX_SUCCESS;

        pmix_bfrop.pack(reply,&rc,1,PMIX_INT);

        /* Put some jobinfo */
        kp->key = PMIX_UNIV_SIZE;
        PMIX_VAL_SET(kp->value,uint32_t,3);
        pmix_bfrop.pack(reply,&kp,1,PMIX_KVAL);
        PMIX_VAL_FREE(kp->value);

        kp->key = PMIX_JOBID;
        PMIX_VAL_SET(kp->value,string,"job0");
        pmix_bfrop.pack(reply,&kp,1,PMIX_KVAL);
        PMIX_VAL_FREE(kp->value);

        kp->key = PMIX_RANK;
        PMIX_VAL_SET(kp->value,uint32_t,2);
        pmix_bfrop.pack(reply,&kp,1,PMIX_KVAL);
        PMIX_VAL_FREE(kp->value);
        break;
    }
    case PMIX_FENCE_CMD:
    case PMIX_FENCENB_CMD:{
        int cnt = 1, rc, tmp, i;
        size_t np;
        size_t nranges;
        pmix_output(0, "executing fence");

        /* Save the input in the reply */
        /* In this scenario we have 3 processes each exporting:
         * - local-key-<rank>=1234<rank>: 12340/12341/12342. Our client is #2.
         * - remote-key-<rank>="Test string #<rank>"
         * - global-key-<rank>=1<rank>.15 */

        /* unpack the number of ranges */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&xfer, &nranges, &cnt, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
        }

        if (0 < nranges) {
            cnt = nranges;
            pmix_range_t *ranges = malloc(sizeof(pmix_range_t) * nranges);
            if( NULL == ranges ){
                rc = PMIX_ERR_OUT_OF_RESOURCE;
                PMIX_ERROR_LOG(rc);
            }
            if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&xfer, ranges, &cnt, PMIX_RANGE))) {
                PMIX_ERROR_LOG(rc);
            }
        }

        /* prepare reply */
        reply = OBJ_NEW(pmix_buffer_t);

        /* 1. Pack the status */
        tmp = PMIX_SUCCESS;
        pmix_bfrop.pack(reply,&tmp,1,PMIX_INT);

        /* 2. pack number of processes in scenario */
        np = 3;
        pmix_bfrop.pack(reply,&np,1,PMIX_SIZE);

        /* 3. verify incoming data */
        verify_kvps(&xfer);

        /* 4. pack the result of processing in reply */
        for(i=0;i<3;i++){
            pmix_bfrop.pack(reply, pmix_db[i],1, PMIX_MODEX);
        }
        break;
    }
/*
    case PMIX_GET_CMD:
        // unpack the id of the proc whose data is being requested
        cnt = 1;
        if (PMIX_SUCCESS != (rc = opal_dss.unpack(&xfer, &idreq, &cnt, OPAL_UINT64))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        opal_output_verbose(2, pmix_server_output, "%s recvd GET FROM PROC %s FOR PROC %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(&name),
                            ORTE_NAME_PRINT((orte_process_name_t*)&idreq) );

        // get the job and proc objects for the sender
        memcpy((char*)&name2, (char*)&idreq, sizeof(orte_process_name_t));
        if( NULL == (pm2 = pmix_server_handler_pm(name2)) ){
            // FIXME: do we need to respond with reject to the sender?
            rc = ORTE_ERR_NOT_FOUND;
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }

//        if we have not yet received data for this proc, then we just
//        need to track the request
        if (!ORTE_FLAG_TEST(pm2->proc, ORTE_PROC_FLAG_DATA_RECVD)) {
            if( (rc = _track_unknown_proc(pm2, peer, tag, &reply)) ){
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            if( reply ){
                goto reply_to_peer;
            }
            // nothing further to do as we are waiting for data
            goto cleanup;
        }

//        * regardless of where this proc is located, we need to ensure
//         * that the hostname it is on is *always* returned. Otherwise,
//         * the non-blocking fence operation will cause us to fail if
//         * the number of procs is below the cutoff as we will immediately
//         * attempt to retrieve the hostname for each proc, but they may
//         * not have posted their data by that time
        if (ORTE_FLAG_TEST(pm2->proc, ORTE_PROC_FLAG_LOCAL)) {
            rc = _reply_for_local_proc(pm, idreq, &reply);
        }else{
            rc = _reply_for_remote_proc(pm, idreq, &reply);
        }

        if( ORTE_SUCCESS != rc ){
            // In case of error we may steel want to notify the peer.
            if( NULL != reply ){
                goto reply_to_peer;
            }else{
                goto cleanup;
            }
        }
        goto reply_to_peer;
    case PMIX_GETATTR_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd GETATTR",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        // create the attrs buffer
        OBJ_CONSTRUCT(&buf, opal_buffer_t);

        // FIXME: Probably remote peer wants out answer anyway!
        //          We should pack ret in any case!
        // stuff the values corresponding to the list of supported attrs
        if (ORTE_SUCCESS != (ret = _proc_info(&buf, pm))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
        // return it
        reply = OBJ_NEW(opal_buffer_t);
        // pack the status
        if (PMIX_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        if (PMIX_SUCCESS == ret) {
            // pack the buffer
            bptr = &buf;
            if (PMIX_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&buf);
                goto cleanup;
            }
        }
        OBJ_DESTRUCT(&buf);OPAL
        goto reply_to_peer;

        */
    default:
            fprintf(stderr,"Bad command!");
        abort();
        return;
    }

    if( NULL != reply ){
        reply_to_client(reply, type, tag);
        OBJ_RELEASE(reply);
    }
    OBJ_DESTRUCT(&xfer);
}

