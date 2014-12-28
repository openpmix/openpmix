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
#include "src/buffer_ops/types.h"
#include "src/buffer_ops/types.h"
#include "src/util/error.h"


struct event_base *server_base = NULL;
struct event *listen_ev = NULL;
int listen_fd = -1;
static int start_listening(struct sockaddr_un *address);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);

event_t *event;
pmix_usock_hdr_t hdr;
char *data = NULL;
bool hdr_rcvd = false;
char *rptr;
size_t rbytes = 0;

bool service = true;

void process_message();

/*
 * Initialize global variables used w/in the server.
 */
int main(int argc, char **argv)
{
    struct sockaddr_un address;
    char **client_env=NULL;
    char **client_argv=NULL;

    pmix_bfrop_open();

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
    event = event_new(server_base, sd,
                      EV_READ|EV_PERSIST, message_handler, NULL);
    event_add(event,NULL);
    usock_set_nonblocking(sd);
    printf("New client connection accepted\n");
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
            fprintf(stderr,"Error communicating with the client\n");
            // TODO: make sure to kill the client
            abort();
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
                fprintf(stderr,"Cannot allocate memory!\n");
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
            fprintf(stderr,"Error communicating with the client\n");
            // TODO: make sure to kill the client
            exit(0);
        }
    }
}


inline static int verify_kvps(pmix_buffer_t *xfer)
{
    pmix_buffer_t *bptr;
    pmix_scope_t scope = 0;
    int rc, cnt = 1;
    pmix_kval_t *kvp = NULL;

    // We expect 3 keys here:
    // 1. for the local scope:  "local-key" = "12345" (INT)
    // 2. for the remote scope: "remote-key" = "Test string" (STRING)
    // 3. for the global scope: "global-key" = "10.15" (FLOAT)
    // Should test more cases in future

    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(xfer, &scope, &cnt, PMIX_SCOPE))) {
        /* unpack the buffer */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(xfer, &bptr, &cnt, PMIX_BUFFER))) {
            fprintf(stderr,"Cannot unpack\n");
            abort();
        }
        if (PMIX_LOCAL == scope) {
            int keys = 0;
            while( 1 ){
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &kvp, &cnt, PMIX_KVAL))) {
                    break;
                }
                keys++;
                if( keys == 1){
                    if( strcmp(kvp->key, "local-key") || kvp->value->type != PMIX_INT
                            || kvp->value->data.integer != 12345 ){
                        fprintf(stderr,"Error receiving LOCAL key\n");
                        abort();
                    }
                }
            }
        } else if (PMIX_REMOTE == scope) {
            int keys = 0;
            while( 1 ){
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &kvp, &cnt, PMIX_VALUE))) {
                    break;
                }
                keys++;
                if( keys == 1){
                    if( strcmp(kvp->key, "remote-key") || kvp->value->type != PMIX_STRING
                            || strcmp(kvp->value->data.string,"Test string") ){
                        fprintf(stderr,"Error receiving REMOTE key\n");
                        abort();
                    }
                }
            }
        } else {
            int keys = 0;
            while( 1 ){
                cnt = 1;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(bptr, &kvp, &cnt, PMIX_VALUE))) {
                    break;
                }
                keys++;
                if( keys == 1){
                    if( strcmp(kvp->key, "global-key") || kvp->value->type != PMIX_FLOAT ||
                            kvp->value->data.fval != (float)10.15 ){
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

void process_message()
{
    int rc, ret;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_buffer_t *reply = NULL;
    pmix_buffer_t xfer, *bptr, buf, save, blocal, bremote;
    uint64_t id, idreq;

    /* xfer the message to a buffer for unpacking */
    OBJ_CONSTRUCT(&xfer, pmix_buffer_t);
    buf.base_ptr = (char*)data;
    buf.bytes_allocated = buf.bytes_used = hdr.nbytes;
    buf.unpack_ptr = buf.base_ptr;
    buf.pack_ptr = ((char*)buf.base_ptr) + buf.bytes_used;

    uint32_t tag = hdr.tag;
    id = hdr.id;
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
    /*
    case PMIX_FINALIZE_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd FINALIZE",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        service = false;
        reply = OBJ_NEW(opal_buffer_t);
        // FIXME: Do we need to pack the tag?
        goto reply_to_peer;
    case PMIX_ABORT_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd ABORT",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        // unpack the status
        cnt = 1;
        if (PMIX_SUCCESS != (rc = opal_dss.unpack(&xfer, &ret, &cnt, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
//         don't bother to unpack the message - we ignore this for now as the
//         proc should have emitted it for itself
        pmix_server_abort_pm(pm, ret);

        reply = OBJ_NEW(opal_buffer_t);
        // pack the tag
        if (PMIX_SUCCESS != (rc = opal_dss.pack(reply, &tag, 1, OPAL_UINT32))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        goto reply_to_peer;
*/
    case PMIX_FENCE_CMD:
    case PMIX_FENCENB_CMD:{
        char *local_uri;
        int cnt = 1;
        pmix_scope_t scope;

        pmix_output(0, "executing fence");

        /* TODO: Process signature when it will be supported */

        /* Read URL. Drop it for now. TODO: verify it in future */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(&xfer, &local_uri, &cnt, PMIX_STRING))) {
            abort();
        }

        verify_kvps(&xfer);
/*
        if (PMIX_FENCE_CMD == cmd) {
            // send a release message back to the sender so they don't hang
            reply = OBJ_NEW(pmix_buffer_t);
            // pack the tag
            if (PMIX_SUCCESS != (rc = opal_dss.pack(reply, &tag, 1, OPAL_UINT32))) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            goto reply_to_peer;
        }
 */
        goto cleanup;
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
/*
reply_to_peer:
    PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
    reply = NULL; // Drop it so it won't be released at cleanup
*/
cleanup:
    if( NULL != reply ){
        OBJ_RELEASE(reply);
    }
    OBJ_DESTRUCT(&xfer);

}
