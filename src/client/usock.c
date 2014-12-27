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
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "pmix_config.h"
#include "pmix_client.h"
#include "src/include/types.h"
#include "pmix_stdint.h"
#include "pmix_socket_errno.h"

#include "src/util/error.h"
#include "usock.h"

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

void pmix_usock_dump(const char* msg);

static int usock_create_socket(void)
{
    int sd = -1;
    pmix_output_verbose(1, pmix_client_globals.debug_output,
                         "pmix:usock:peer creating socket to server");

    sd = socket(PF_UNIX, SOCK_STREAM, 0);

    if (sd < 0) {
        pmix_output(0, "usock_peer_create_socket: socket() failed: %s (%d)\n",
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
        return PMIX_ERR_UNREACH;
    }
    return sd;
}

int usock_set_nonblocking(int sd)
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

int usock_set_blocking(int sd)
{
    int flags;
     /* setup the socket as non-blocking */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "usock_peer_connect: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
    } else {
        flags &= (~O_NONBLOCK);
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
static int usock_send_blocking(int sd, char *ptr, size_t size)
{
    size_t cnt = 0;
    int retval;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
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

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "blocking send complete to socket %d",
                        pmix_client_globals.sd);
    return PMIX_SUCCESS;
}

/*
 * A blocking recv on a non-blocking socket. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 */
static int usock_recv_blocking(int sd, char *data, size_t size)
{
    size_t cnt = 0;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "waiting for connect ack from server");

    while (cnt < size) {
        int retval = recv(sd, (char *)data+cnt, size-cnt, 0);

        /* remote closed connection */
        if (retval == 0) {
            pmix_output_verbose(2, pmix_client_globals.debug_output,
                                "usock_recv_blocking: server closed connection");
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
                pmix_output_verbose(2, pmix_client_globals.debug_output,
                                    "connect ack received error %s from server",
                                    strerror(pmix_socket_errno));
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "connect ack received from server");
    return PMIX_SUCCESS;
}

int pmix_usock_connect(struct sockaddr *addr, int max_retries)
{
    int rc, sd=-1;
    pmix_socklen_t addrlen = 0;
    int retries = 0;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "usock_peer_try_connect: attempting to connect to server");
    addrlen = sizeof(struct sockaddr_un);
    while( retries < max_retries ){
        retries++;
        /* Create the new socket */
        if ( 0 > (sd = usock_create_socket() ) ) {
            continue;
        }
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "usock_peer_try_connect: attempting to connect to server on socket %d",
                            pmix_client_globals.sd);
        /* try to connect */
        if (connect(sd, addr, addrlen) < 0) {
            if ( pmix_socket_errno == ETIMEDOUT ) {
                /* The server may be too busy to accept new connections */
                pmix_output_verbose(2, pmix_client_globals.debug_output,
                                    "timeout connecting to server");
                goto repeat;
            }

            /* Some kernels (Linux 2.6) will automatically software
               abort a connection that was ECONNREFUSED on the last
               attempt, without even trying to establish the
               connection.  Handle that case in a semi-rational
               way by trying twice before giving up */
            if (ECONNABORTED == pmix_socket_errno) {
                pmix_output_verbose(2, pmix_client_globals.debug_output,
                                    "connection to server aborted by OS - retrying");
                goto repeat;
            }
        }

        /* send our globally unique process identifier to the server */
        if (PMIX_SUCCESS != (rc = usock_send_connect_ack(sd))) {
            goto repeat;
        }

        /* receive ack from the server */
        if (PMIX_SUCCESS != (rc = usock_recv_connect_ack(sd))) {
            goto repeat;
        }
        break;
repeat:
        CLOSE_THE_SOCKET(sd);
        continue;
    }

    if(retries == max_retries || sd < 0){
        /* We were unsuccessful in establishing this connection, and are
         * not likely to suddenly become successful */
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "sock_peer_try_connect: Connection across to server succeeded");
    usock_set_nonblocking(sd);
    return sd;
}

int usock_send_connect_ack(int sd)
{
    char *msg;
    pmix_usock_hdr_t hdr;
    size_t sdsize;
    // TODO: Deal with security
    //opal_sec_cred_t *cred;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "SEND CONNECT ACK");

    /* setup the header */
    hdr.id = pmix_client_globals.id;
    hdr.tag = UINT32_MAX;
    hdr.type = PMIX_USOCK_IDENT;

    /* get our security credential */
//    if (PMIX_SUCCESS != (rc = opal_sec.get_my_credential(opal_dstore_internal, &OPAL_PROC_MY_NAME, &cred))) {
//        return rc;
//    }

    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = strlen(PMIX_VERSION) + 1; // + cred->size;

    /* create a space for our message */
    sdsize = (sizeof(hdr) + strlen(PMIX_VERSION) + 1 /* + cred->size */);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg+sizeof(hdr), PMIX_VERSION, strlen(PMIX_VERSION));
    //memcpy(msg+sizeof(hdr)+strlen(opal_version_string)+1, cred->credential, cred->size);


    if (PMIX_SUCCESS != usock_send_blocking(sd, msg, sdsize)) {
        // TODO: Process all peer state changes if need
        free(msg);
        return PMIX_ERR_UNREACH;
    }
    free(msg);
    return PMIX_SUCCESS;
}


/*
 *  Receive the peers globally unique process identification from a newly
 *  connected socket and verify the expected response. If so, move the
 *  socket to a connected state.
 */
int usock_recv_connect_ack(int sd)
{
    char *msg;
    char *version;
    int rc;
//    opal_sec_cred_t creds;
    pmix_usock_hdr_t hdr;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "RECV CONNECT ACK FROM SERVER ON SOCKET %d",
                        pmix_client_globals.sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));

    if ( PMIX_SUCCESS != (rc = usock_recv_blocking(sd, (char*)&hdr, sizeof(pmix_usock_hdr_t))) ) {
        return rc;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "connect-ack recvd from server");

    /* compare the servers name to the expected value */
    if (hdr.id != pmix_client_globals.server) {
        pmix_output(0, "usock_peer_recv_connect_ack: "
                    "received unexpected process identifier %"PRIu64" from server: expected %"PRIu64"",
                    hdr.id, pmix_client_globals.server);
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "connect-ack header from server is okay");

    /* get the authentication and version payload */
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    if ( PMIX_SUCCESS != usock_recv_blocking(sd, msg, hdr.nbytes)) {
        /* unable to complete the recv */
        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "unable to complete recv of connect-ack from server ON SOCKET %d",
                            pmix_client_globals.sd);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    /* check that this is from a matching version */
    version = (char*)(msg);
    if (0 != strcmp(version, PMIX_VERSION)) {
        pmix_output(0, "usock_peer_recv_connect_ack: "
                    "received different version from server: %s instead of %s",
                    version, PMIX_VERSION);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "connect-ack version from server matches ours");

    /* check security token */
//    creds.credential = (char*)(msg + strlen(version) + 1);
//    creds.size = hdr.nbytes - strlen(version) - 1;
//    if (PMIX_SUCCESS != (rc = opal_sec.authenticate(&creds))) {
//        PMIX_ERROR_LOG(rc);
//    }
    free(msg);

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "connect-ack from server authenticated");

    if (2 <= pmix_output_get_verbosity(pmix_client_globals.debug_output)) {
        pmix_usock_dump("connected");
    }
    return PMIX_SUCCESS;
}

/*
 * Routine for debugging to print the connection state and socket options
 */
void pmix_usock_dump(const char* msg)
{
    char buff[255];
    int nodelay,flags;

    if ((flags = fcntl(pmix_client_globals.sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "usock_peer_dump: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno),
                    pmix_socket_errno);
    }
                                                                                                            
#if defined(USOCK_NODELAY)
    optlen = sizeof(nodelay);
    if (getsockopt(pmix_client_globals.sd, IPPROTO_USOCK, USOCK_NODELAY, (char *)&nodelay, &optlen) < 0) {
        pmix_output(0, "usock_peer_dump: USOCK_NODELAY option: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
    }
#else
    nodelay = 0;
#endif

    snprintf(buff, sizeof(buff), "%s: nodelay %d flags %08x\n",
             msg, nodelay, flags);
    pmix_output(0, "%s", buff);
}
/*
char* pmix_usock_state_print(pmix_usock_state_t state)
{
    switch (state) {
    case PMIX_USOCK_UNCONNECTED:
        return "UNCONNECTED";

//    case PMIX_USOCK_CLOSED:
//        return "CLOSED";
//    case PMIX_USOCK_RESOLVE:
//        return "RESOLVE";
//    case PMIX_USOCK_CONNECTING:
//        return "CONNECTING";
//    case PMIX_USOCK_CONNECT_ACK:
//        return "ACK";

    case PMIX_USOCK_CONNECTED:
        return "CONNECTED";

//    case PMIX_USOCK_FAILED:
//        return "FAILED";

    default:
        return "UNKNOWN";
    }
}

*/
