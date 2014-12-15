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

#include "orte_config.h"
#include "orte/types.h"
#include "opal/types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <ctype.h>

#include "opal_stdint.h"
#include "opal/class/opal_list.h"
#include "opal/mca/base/mca_base_var.h"
#include "opal/util/opal_environ.h"
#include "opal/util/show_help.h"
#include "opal/util/error.h"
#include "opal/util/output.h"
#include "opal/opal_socket_errno.h"
#include "opal/util/if.h"
#include "opal/util/net.h"
#include "opal/util/argv.h"
#include "opal/class/opal_hash_table.h"
#include "opal/mca/dstore/dstore.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/grpcomm/grpcomm.h"
#include "orte/mca/rml/rml.h"
#include "orte/util/name_fns.h"
#include "orte/util/session_dir.h"
#include "orte/util/show_help.h"
#include "orte/runtime/orte_globals.h"

#include "pmix_server.h"
#include "pmix_server_internal.h"

/*
 * Local utility functions
 */
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static int pmix_server_start_listening(struct sockaddr_un *address);
static void pmix_server_recv(int status, orte_process_name_t* sender,
                             opal_buffer_t *buffer,
                             orte_rml_tag_t tg, void *cbdata);
static void pmix_server_release(int status,
                                opal_buffer_t *buffer,
                                void *cbdata);
static void pmix_server_dmdx_recv(int status, orte_process_name_t* sender,
                                  opal_buffer_t *buffer,
                                  orte_rml_tag_t tg, void *cbdata);
static void pmix_server_dmdx_resp(int status, orte_process_name_t* sender,
                                  opal_buffer_t *buffer,
                                  orte_rml_tag_t tg, void *cbdata);

char *pmix_server_uri = NULL;
opal_hash_table_t *pmix_server_peers = NULL;
int pmix_server_verbosity = -1;
int pmix_server_output = -1;
int pmix_server_local_handle = -1;
int pmix_server_remote_handle = -1;
int pmix_server_global_handle = -1;
opal_list_t pmix_server_pending_dmx_reqs;
static bool initialized = false;
static struct sockaddr_un address;
static int pmix_server_listener_socket = -1;
static bool pmix_server_listener_ev_active = false;
static opal_event_t pmix_server_listener_event;
static opal_list_t collectives;

void pmix_server_register(void)
{
    /* register a verbosity */
    pmix_server_verbosity = -1;
    (void) mca_base_var_register ("orte", "pmix", NULL, "server_verbose",
                                  "Debug verbosity for PMIx server",
                                  MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  OPAL_INFO_LVL_9, MCA_BASE_VAR_SCOPE_ALL,
                                  &pmix_server_verbosity);
    if (0 <= pmix_server_verbosity) {
        pmix_server_output = opal_output_open(NULL);
        opal_output_set_verbosity(pmix_server_output, pmix_server_verbosity);
    }
}

/*
 * Initialize global variables used w/in the server.
 */
int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;

    /* initialize the event library */
    
    /* setup the path to the rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "pmix");

    /* start listening */
    
    /* define the argv for the test client */
    pmix_argv_append_nosize(&client_argv, "client");

    /* pass our URI */
    asprintf(&tmp, "PMIX_SERVER_URI=0:%s", address.sun_path);
    pmix_argv_append_nosize(&client_env, tmp);
    free(tmp);
    /* pass an ID for the client */
    pmix_argv_append_nosize(&client_env, "PMIX_ID=1");
    /* pass a security credential */
    pmix_argv_append_nosize(&client_env, "PMIX_SEC_CRED=credential-string");
    
    /* fork/exec the test */

    /* cycle the event library until complete */
    
    return 0;
}

/*
 * start listening on our rendezvous file
 */
static int server_start_listening(struct sockaddr_un *address)
{
    int flags;
    opal_socklen_t addrlen;
    int sd = -1;

    /* create a listen socket for incoming connection attempts */
    sd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sd < 0) {
        pmix_output(0,"pmix_server_start_listening: socket() failed");
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(sd, (struct sockaddr*)address, addrlen) < 0) {
        pmix_output(0, "%s bind() failed");
        return -1;
    }
        
    /* setup listen backlog to maximum allowed by kernel */
    if (listen(sd, SOMAXCONN) < 0) {
        pmix_output(0, "pmix_server_init: listen()");
        return PMIX_ERROR;
    }
        
    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "pmix_server_init: fcntl(F_GETFL) failed");
        return PMIX_ERROR;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sd, F_SETFL, flags) < 0) {
        pmix_output(0, "pmix_server_component_init: fcntl(F_SETFL) failed");
        return PMIX_ERROR;
    }

    /* record this socket */
    pmix_server_listener_socket = sd;

    /* setup to listen via the event lib */
    pmix_server_listener_ev_active = true;
    event_set(pmix_event_base, &pmix_server_listener_event,
              pmix_server_listener_socket,
              OPAL_EV_READ|OPAL_EV_PERSIST,
              connection_handler,
              0);
    event_add(&pmix_server_listener_event, 0);

    pmix_output_verbose(2, pmix_server_output,
                        "%s pmix server listening on socket %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), sd);

    return ORTE_SUCCESS;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incoming_sd, short flags, void* cbdata)
{
    struct sockaddr addr;
    opal_socklen_t addrlen = sizeof(struct sockaddr);
    int sd, rc;
    pmix_server_hdr_t hdr;
    pmix_server_peer_t *peer;

    sd = accept(incoming_sd, (struct sockaddr*)&addr, &addrlen);
    pmix_output_verbose(2, pmix_server_output,
                        "connection_event_handler: working connection %d", sd);
    if (sd < 0) {
        if (EINTR == errno) {
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (EMFILE == errno) {
                /*
                 * Close incoming_sd so that orte_show_help will have a file
                 * descriptor with which to open the help file.  We will be
                 * exiting anyway, so we don't need to keep it open.
                 */
                CLOSE_THE_SOCKET(incoming_sd);
                pmix_output("Error: sys-limit-sockets";
            } else {
                pmix_output(0, "pmix_server_accept: accept() failed: %s (%d).", 
                            strerror(errno), errno);
            }
        }
        return;
    }

    /* get the handshake */
    if (ORTE_SUCCESS != (rc = pmix_server_recv_connect_ack(NULL, sd, &hdr))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* finish processing ident */
    if (PMIX_USOCK_IDENT == hdr.type) {
        /* set socket up to be non-blocking */
        if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
            pmix_output(0, "pmix_server_recv_connect: fcntl(F_GETFL) failed: %s (%d)",
                        strerror(errno), errno);
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(sd, F_SETFL, flags) < 0) {
                pmix_output(0, "pmix_server_recv_connect: fcntl(F_SETFL) failed: %s (%d)",
                            strerror(errno), errno);
            }
        }

        /* is the peer instance willing to accept this connection */
        peer->sd = sd;
        if (peer->state != PMIX_SERVER_CONNECTED) {
            pmix_server_peer_event_init(peer);

            if (pmix_server_send_connect_ack(peer) != ORTE_SUCCESS) {
                opal_output(0, "%s usock_peer_accept: "
                            "usock_peer_send_connect_ack to %s failed\n",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&(peer->name)));
                peer->state = PMIX_SERVER_FAILED;
                CLOSE_THE_SOCKET(sd);
                return;
            }

            pmix_server_peer_connected(peer);
            if (2 <= opal_output_get_verbosity(pmix_server_output)) {
                pmix_server_peer_dump(peer, "accepted");
            }
        } else {
            opal_output_verbose(2, pmix_server_output,
                        "%s usock:peer_accept ignored for peer %s in state %s on socket %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name),
                        pmix_server_state_print(peer->state), peer->sd);
            opal_hash_table_set_value_uint64(pmix_server_peers, sd, NULL);
            CLOSE_THE_SOCKET(sd);
            OBJ_RELEASE(peer);
        }
    }
}

char* pmix_server_state_print(pmix_server_state_t state)
{
    switch (state) {
    case PMIX_SERVER_UNCONNECTED:
        return "UNCONNECTED";
    case PMIX_SERVER_CLOSED:
        return "CLOSED";
    case PMIX_SERVER_RESOLVE:
        return "RESOLVE";
    case PMIX_SERVER_CONNECTING:
        return "CONNECTING";
    case PMIX_SERVER_CONNECT_ACK:
        return "ACK";
    case PMIX_SERVER_CONNECTED:
        return "CONNECTED";
    case PMIX_SERVER_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}


 static int usock_peer_send_blocking(pmix_server_peer_t* peer,
                                    int sd, void* data, size_t size);
static bool usock_peer_recv_blocking(pmix_server_peer_t* peer,
                                     int sd, void* data, size_t size);

int pmix_server_send_connect_ack(pmix_server_peer_t* peer)
{
    char *msg;
    pmix_server_hdr_t hdr;
    int rc;
    size_t sdsize;

    opal_output_verbose(2, pmix_server_output,
                        "%s SEND CONNECT ACK", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* send a handshake that includes our identifier */
    memcpy(&hdr.id, &zero, sizeof(pmix_identifier_t));
    hdr.type = PMIX_USOCK_IDENT;
    hdr.tag = UINT32_MAX;

    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = strlen(orte_version_string) + 1 + cred->size;

    /* create a space for our message */
    sdsize = (sizeof(hdr) + strlen(opal_version_string) + 1 + cred->size);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        return ORTE_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg+sizeof(hdr), opal_version_string, strlen(opal_version_string));
    memcpy(msg+sizeof(hdr)+strlen(opal_version_string)+1, cred->credential, cred->size);


    if (ORTE_SUCCESS != usock_peer_send_blocking(peer, pmix_server.sd, msg, sdsize)) {
        ORTE_ERROR_LOG(ORTE_ERR_UNREACH);
        return ORTE_ERR_UNREACH;
    }
    return ORTE_SUCCESS;
}

/*
 * Initialize events to be used by the peer instance for USOCK select/poll callbacks.
 */
void pmix_server_peer_event_init(pmix_server_peer_t* peer)
{
    if (pmix_server.sd >= 0) {
        opal_event_set(orte_event_base,
                       &pmix_server.recv_event,
                       pmix_server.sd,
                       OPAL_EV_READ|OPAL_EV_PERSIST,
                       pmix_server_recv_handler,
                       peer);
        opal_event_set_priority(&pmix_server.recv_event, ORTE_MSG_PRI);
        if (pmix_server.recv_ev_active) {
            opal_event_del(&pmix_server.recv_event);
            pmix_server.recv_ev_active = false;
        }
        
        opal_event_set(orte_event_base,
                       &pmix_server.send_event,
                       pmix_server.sd,
                       OPAL_EV_WRITE|OPAL_EV_PERSIST,
                       pmix_server_send_handler,
                       peer);
        opal_event_set_priority(&pmix_server.send_event, ORTE_MSG_PRI);
        if (pmix_server.send_ev_active) {
            opal_event_del(&pmix_server.send_event);
            pmix_server.send_ev_active = false;
        }
    }
}

/*
 * A blocking send on a non-blocking socket. Used to send the small amount of connection
 * information that identifies the peers endpoint.
 */
static int usock_peer_send_blocking(pmix_server_peer_t* peer,
                                    int sd, void* data, size_t size)
{
    unsigned char* ptr = (unsigned char*)data;
    size_t cnt = 0;
    int retval;

    opal_output_verbose(2, pmix_server_output,
                        "%s send blocking of %"PRIsize_t" bytes to socket %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        size, sd);

    while (cnt < size) {
        retval = send(sd, (char*)ptr+cnt, size-cnt, 0);
        if (retval < 0) {
            if (opal_socket_errno != EINTR && opal_socket_errno != EAGAIN && opal_socket_errno != EWOULDBLOCK) {
                opal_output(0, "%s usock_peer_send_blocking: send() to socket %d failed: %s (%d)\n",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), sd,
                            strerror(opal_socket_errno),
                            opal_socket_errno);
                pmix_server.state = PMIX_SERVER_FAILED;
                CLOSE_THE_SOCKET(pmix_server.sd);
                return ORTE_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s blocking send complete to socket %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), sd);

    return ORTE_SUCCESS;
}

/*
 *  Receive the peers globally unique process identification from a newly
 *  connected socket and verify the expected response. If so, move the
 *  socket to a connected state.
 */
int pmix_server_recv_connect_ack(int sd)
{
    char *msg;
    char *version;
    int rc;
    opal_sec_cred_t creds;
    pmix_server_peer_t *peer;
    pmix_server_hdr_t hdr;
    orte_process_name_t sender;

    opal_output_verbose(2, pmix_server_output,
                        "RECV CONNECT ACK FROM CLIENT ON SOCKET %d", sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_server_hdr_t));

    if (usock_peer_recv_blocking(peer, sd, &hdr, sizeof(pmix_server_hdr_t))) {
        if (NULL != peer) {
            /* If the peer state is CONNECT_ACK, then we were waiting for
             * the connection to be ack'd
             */
            if (pmix_server.state != PMIX_SERVER_CONNECT_ACK) {
                /* handshake broke down - abort this connection */
                opal_output(0, "RECV CONNECT BAD HANDSHAKE FROM CLIENT ON SOCKET %d", sd);
                pmix_server.state = PMIX_SERVER_FAILED;
                return ORTE_ERR_UNREACH;
            }
        }
    } else {
        /* unable to complete the recv */
        opal_output_verbose(2, pmix_server_output,
                            "unable to complete recv of connect-ack from client ON SOCKET %d", sd);
        return ORTE_ERR_UNREACH;
    }
    /* if the requestor wanted the header returned, then do so now */
    if (NULL != dhdr) {
        *dhdr = hdr;
    }

    if (hdr.type != PMIX_USOCK_IDENT) {
        opal_output(0, "usock_peer_recv_connect_ack: invalid header type: %d\n", hdr.type);
        if (NULL != peer) {
            pmix_server.state = PMIX_SERVER_FAILED;
            CLOSE_THE_SOCKET(pmix_server.sd);
        } else {
            CLOSE_THE_SOCKET(sd);
        }
        return ORTE_ERR_UNREACH;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack recvd from %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&pmix_server.name));

    memcpy(&sender, &hdr.id, sizeof(opal_identifier_t));
    /* if we don't already have it, get the peer */
    if (NULL == peer) {
        peer = pmix_server_peer_lookup(sd);
        if (NULL == peer) {
            opal_output_verbose(2, pmix_server_output,
                                "%s pmix_server_recv_connect: connection from new peer",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            peer = OBJ_NEW(pmix_server_peer_t);
            pmix_server.name = sender;
            pmix_server.state = PMIX_SERVER_ACCEPTING;
            pmix_server.sd = sd;
            if (OPAL_SUCCESS != opal_hash_table_set_value_uint64(pmix_server_peers, sd, peer)) {
                OBJ_RELEASE(peer);
                CLOSE_THE_SOCKET(sd);
                return ORTE_ERR_UNREACH;
            }
        } else if (PMIX_SERVER_CONNECTED == pmix_server.state ||
                   PMIX_SERVER_CONNECTING == pmix_server.state ||
                   PMIX_SERVER_CONNECT_ACK == pmix_server.state) {
            /* if I already have an established such a connection, then we need
             * to reject this connection */
            opal_output_verbose(2, pmix_server_output,
                                "%s EXISTING CONNECTION WITH %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&sender));
            if (pmix_server.recv_ev_active) {
                opal_event_del(&pmix_server.recv_event);
                pmix_server.recv_ev_active = false;
            }
            if (pmix_server.send_ev_active) {
                opal_event_del(&pmix_server.send_event);
                pmix_server.send_ev_active = false;
            }
            if (0 < pmix_server.sd) {
                CLOSE_THE_SOCKET(pmix_server.sd);
                pmix_server.sd = -1;
            }
            pmix_server.retries = 0;
        }
    } else {
        /* compare the peers name to the expected value */
        if (OPAL_EQUAL != orte_util_compare_name_fields(ORTE_NS_CMP_ALL, &pmix_server.name, &sender)) {
            opal_output(0, "%s usock_peer_recv_connect_ack: "
                        "received unexpected process identifier %s from %s\n",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&sender),
                        ORTE_NAME_PRINT(&(pmix_server.name)));
            pmix_server.state = PMIX_SERVER_FAILED;
            CLOSE_THE_SOCKET(pmix_server.sd);
            return ORTE_ERR_UNREACH;
        }
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack header from %s is okay",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&pmix_server.name));

    /* get the authentication and version payload */
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        pmix_server.state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(pmix_server.sd);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }
    if (!usock_peer_recv_blocking(peer, sd, msg, hdr.nbytes)) {
        /* unable to complete the recv */
        opal_output_verbose(2, pmix_server_output,
                            "%s unable to complete recv of connect-ack from %s ON SOCKET %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&pmix_server.name), pmix_server.sd);
        free(msg);
        return ORTE_ERR_UNREACH;
    }

    /* check that this is from a matching version */
    version = (char*)(msg);
    if (0 != strcmp(version, opal_version_string)) {
        opal_output(0, "%s usock_peer_recv_connect_ack: "
                    "received different version from %s: %s instead of %s\n",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&(pmix_server.name)),
                    version, opal_version_string);
        pmix_server.state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(pmix_server.sd);
        free(msg);
        return ORTE_ERR_UNREACH;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack version from %s matches ours",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&pmix_server.name));

    /* check security token */
    creds.credential = (char*)(msg + strlen(version) + 1);
    creds.size = hdr.nbytes - strlen(version) - 1;
    if (OPAL_SUCCESS != (rc = opal_sec.authenticate(&creds))) {
        ORTE_ERROR_LOG(rc);
    }
    free(msg);

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack %s authenticated",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&pmix_server.name));

    /* if the requestor wanted the header returned, then they
     * will complete their processing
     */
    if (NULL != dhdr) {
        return ORTE_SUCCESS;
    }

    /* connected */
    pmix_server_peer_connected(peer);
    if (2 <= opal_output_get_verbosity(pmix_server_output)) {
        pmix_server_peer_dump(peer, "connected");
    }
    return ORTE_SUCCESS;
}

/*
 *  Setup peer state to reflect that connection has been established,
 *  and start any pending sends.
 */
void pmix_server_peer_connected(pmix_server_peer_t* peer)
{
    /* ensure the recv event is active */
    if (!pmix_server.recv_ev_active) {
        event_add(&pmix_server.recv_event, 0);
        pmix_server.recv_ev_active = true;
    }

    /* initiate send of first message on queue */
    if (NULL == pmix_server.send_msg) {
        pmix_server.send_msg = (pmix_server_send_t*)
            pmix_list_remove_first(&pmix_server.send_queue);
    }
    if (NULL != pmix_server.send_msg && !pmix_server.send_ev_active) {
        event_add(&pmix_server.send_event, 0);
        pmix_server.send_ev_active = true;
    }
}

/*
 * A blocking recv on a non-blocking socket. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 */
static bool usock_peer_recv_blocking(int sd, void* data, size_t size)
{
    unsigned char* ptr = (unsigned char*)data;
    size_t cnt = 0;

    pmix_output_verbose(2, pmix_server_output,
                        "waiting for connect ack from client");

    while (cnt < size) {
        int retval = recv(sd, (char *)ptr+cnt, size-cnt, 0);

        /* remote closed connection */
        if (retval == 0) {
            pmix_output_verbose(2, pmix_server_output,
                                "usock_peer_recv_blocking: "
                                "client closed connection: peer state %d", pmix_server.state);
            pmix_server.state = PMIX_SERVER_FAILED;
            return false;
        }

        /* socket is non-blocking so handle errors */
        if (retval < 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (pmix_server.state == PMIX_SERVER_CONNECT_ACK) {
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
                    pmix_output_verbose(2, pmix_server_output,
                                        "connect ack received error %s from client",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        strerror(errno));
                    return false;
                } else {
                    pmix_output(0, "%s usock_peer_recv_blocking: recv() failed: %s (%d)\n",
                                strerror(errno), errno);
                    pmix_server.state = PMIX_SERVER_FAILED;
                    return false;
                }
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(2, pmix_server_output,
                        "connect ack received from client");
    return true;
}

static void complete_connect(pmix_server_peer_t *peer);

static int send_bytes(pmix_server_peer_t* peer)
{
    pmix_server_send_t* msg = peer->send_msg;
    int rc;

    while (0 < msg->sdbytes) {
        rc = write(peer->sd, msg->sdptr, msg->sdbytes);
        if (rc < 0) {
            if (opal_socket_errno == EINTR) {
                continue;
            } else if (opal_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return ORTE_ERR_RESOURCE_BUSY;
            } else if (opal_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return ORTE_ERR_WOULD_BLOCK;
            }
            /* we hit an error and cannot progress this message */
            opal_output(0, "%s->%s pmix_server_msg_send_bytes: write failed: %s (%d) [sd = %d]", 
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), 
                        ORTE_NAME_PRINT(&(peer->name)), 
                        strerror(opal_socket_errno),
                        opal_socket_errno,
                        peer->sd);
            return ORTE_ERR_COMM_FAILURE;
        }
        /* update location */
        msg->sdbytes -= rc;
        msg->sdptr += rc;
    }
    /* we sent the full data block */
    return ORTE_SUCCESS;
}

/*
 * A file descriptor is available/ready for send. Check the state
 * of the socket and take the appropriate action.
 */
void pmix_server_send_handler(int sd, short flags, void *cbdata)
{
    pmix_server_peer_t* peer = (pmix_server_peer_t*)cbdata;
    pmix_server_send_t* msg = peer->send_msg;
    int rc;

    opal_output_verbose(2, pmix_server_output,
                        "%s usock:send_handler called to send to peer %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name));

    switch (peer->state) {
    case PMIX_SERVER_CONNECTING:
    case PMIX_SERVER_CLOSED:
        opal_output_verbose(2, pmix_server_output,
                            "%s usock:send_handler %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            pmix_server_state_print(peer->state));
        complete_connect(peer);
        /* de-activate the send event until the connection
         * handshake completes
         */
        if (peer->send_ev_active) {
            opal_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        break;
    case PMIX_SERVER_CONNECTED:
        opal_output_verbose(2, pmix_server_output,
                            "%s usock:send_handler SENDING TO %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            (NULL == peer->send_msg) ? "NULL" : ORTE_NAME_PRINT(&peer->name));
        if (NULL != msg) {
            /* if the header hasn't been completely sent, send it */
            if (!msg->hdr_sent) {
                if (ORTE_SUCCESS == (rc = send_bytes(peer))) {
                    /* header is completely sent */
                    msg->hdr_sent = true;
                    /* setup to send the data */
                    if (NULL == msg->data) {
                        /* this was a zero-byte msg - nothing more to do */
                        OBJ_RELEASE(msg);
                        peer->send_msg = NULL;
                        goto next;
                    } else {
                        msg->sdptr = msg->data->base_ptr;
                        msg->sdbytes = msg->hdr.nbytes;
                    }
                    /* fall thru and let the send progress */
                } else if (ORTE_ERR_RESOURCE_BUSY == rc ||
                           ORTE_ERR_WOULD_BLOCK == rc) {
                    /* exit this event and let the event lib progress */
                    return;
                } else {
                    // report the error
                    opal_output(0, "%s-%s pmix_server_peer_send_handler: unable to send header",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&(peer->name)));
                    opal_event_del(&peer->send_event);
                    peer->send_ev_active = false;
                    OBJ_RELEASE(msg);
                    peer->send_msg = NULL;
                    goto next;
                }
            }
            /* progress the data transmission */
            if (msg->hdr_sent) {
                if (ORTE_SUCCESS == (rc = send_bytes(peer))) {
                    /* this message is complete */
                    OBJ_RELEASE(msg);
                    peer->send_msg = NULL;
                    /* fall thru to queue the next message */
                } else if (ORTE_ERR_RESOURCE_BUSY == rc ||
                           ORTE_ERR_WOULD_BLOCK == rc) {
                    /* exit this event and let the event lib progress */
                    return;
                } else {
                    // report the error
                    opal_output(0, "%s-%s pmix_server_peer_send_handler: unable to send message ON SOCKET %d",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&(peer->name)), peer->sd);
                    opal_event_del(&peer->send_event);
                    peer->send_ev_active = false;
                    OBJ_RELEASE(msg);
                    peer->send_msg = NULL;
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
            peer->send_msg = (pmix_server_send_t*)
                opal_list_remove_first(&peer->send_queue);
        }
        
        /* if nothing else to do unregister for send event notifications */
        if (NULL == peer->send_msg && peer->send_ev_active) {
            opal_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        break;

    default:
        opal_output(0, "%s-%s pmix_server_peer_send_handler: invalid connection state (%d) on socket %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&(peer->name)),
                    peer->state, peer->sd);
        if (peer->send_ev_active) {
            opal_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        break;
    }
}

static int read_bytes(pmix_server_peer_t* peer)
{
    int rc;

    /* read until all bytes recvd or error */
    while (0 < peer->recv_msg->rdbytes) {
        rc = read(peer->sd, peer->recv_msg->rdptr, peer->recv_msg->rdbytes);
        if (rc < 0) {
            if(opal_socket_errno == EINTR) {
                continue;
            } else if (opal_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return ORTE_ERR_RESOURCE_BUSY;
            } else if (opal_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return ORTE_ERR_WOULD_BLOCK;
            }
            /* we hit an error and cannot progress this message - report
             * the error back to the RML and let the caller know
             * to abort this message
             */
            opal_output_verbose(2, pmix_server_output,
                                "%s-%s pmix_server_msg_recv: readv failed: %s (%d)", 
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&(peer->name)),
                                strerror(opal_socket_errno),
                                opal_socket_errno);
            // pmix_server_peer_close(peer);
            // if (NULL != pmix_server.oob_exception_callback) {
            // pmix_server.oob_exception_callback(&peer->name, ORTE_RML_PEER_DISCONNECTED);
            //}
            return ORTE_ERR_COMM_FAILURE;
        } else if (rc == 0)  {
            /* the remote peer closed the connection - report that condition
             * and let the caller know
             */
            opal_output_verbose(2, pmix_server_output,
                                "%s-%s pmix_server_msg_recv: peer closed connection", 
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&(peer->name)));
            /* stop all events */
            if (peer->recv_ev_active) {
                opal_event_del(&peer->recv_event);
                peer->recv_ev_active = false;
            }
            if (peer->timer_ev_active) {
                opal_event_del(&peer->timer_event);
                peer->timer_ev_active = false;
            }
            if (peer->send_ev_active) {
                opal_event_del(&peer->send_event);
                peer->send_ev_active = false;
            }
            if (NULL != peer->recv_msg) {
                OBJ_RELEASE(peer->recv_msg);
                peer->recv_msg = NULL;
            }
            peer->state = PMIX_SERVER_CLOSED;
            CLOSE_THE_SOCKET(peer->sd);
            //if (NULL != pmix_server.oob_exception_callback) {
            //   pmix_server.oob_exception_callback(&peer->peer_name, ORTE_RML_PEER_DISCONNECTED);
            //}
            return ORTE_ERR_WOULD_BLOCK;
        }
        /* we were able to read something, so adjust counters and location */
        peer->recv_msg->rdbytes -= rc;
        peer->recv_msg->rdptr += rc;
    }

    /* we read the full data block */
    return ORTE_SUCCESS;
}

 /*
 * Dispatch to the appropriate action routine based on the state
 * of the connection with the peer.
 */
static void process_message(pmix_server_peer_t *peer)
{
    int rc, ret;
    int32_t cnt;
    pmix_cmd_t cmd;
    opal_buffer_t *reply, xfer, *bptr, buf, save, blocal, bremote;
    opal_value_t kv, *kvp, *kvp2, *kp;
    opal_identifier_t id, idreq;
    orte_process_name_t name;
    orte_job_t *jdata;
    orte_proc_t *proc;
    opal_list_t values;
    uint32_t tag;
    opal_pmix_scope_t scope;
    int handle;
    pmix_server_dmx_req_t *req, *nextreq;
    bool found;
    orte_grpcomm_signature_t *sig;
    char *local_uri;

    /* xfer the message to a buffer for unpacking */
    OBJ_CONSTRUCT(&xfer, opal_buffer_t);
    opal_dss.load(&xfer, peer->recv_msg->data, peer->recv_msg->hdr.nbytes);
    tag = peer->recv_msg->hdr.tag;
    id = peer->recv_msg->hdr.id;
    peer->recv_msg->data = NULL;  // protect the transferred data
    OBJ_RELEASE(peer->recv_msg);

    /* retrieve the cmd */
    cnt = 1;
    if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, &cmd, &cnt, PMIX_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&xfer);
        return;
    }
    opal_output_verbose(2, pmix_server_output,
                        "%s recvd pmix cmd %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd);
    switch(cmd) {
    case PMIX_ABORT_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd ABORT",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* unpack the status */
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, &ret, &cnt, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        /* send the release so the proc can depart */
        reply = OBJ_NEW(opal_buffer_t);
        /* pack the tag */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &tag, 1, OPAL_UINT32))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
        OBJ_DESTRUCT(&xfer);
        return;
    case PMIX_FENCE_CMD:
    case PMIX_FENCENB_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd %s FROM PROC %s ON TAG %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            (PMIX_FENCENB_CMD == cmd) ? "FENCE_NB" : "FENCE",
                            OPAL_NAME_PRINT(id), tag);
        
        /* get the number of procs in this fence collective */
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, &sig->sz, &cnt, OPAL_SIZE))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(sig);
            goto reply_fence;
        }
        /* if a signature was provided, get it */
        if (0 < sig->sz) {
            sig->signature = (orte_process_name_t*)malloc(sig->sz * sizeof(orte_process_name_t));
            cnt = sig->sz;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, sig->signature, &cnt, OPAL_UINT64))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(sig);
                goto reply_fence;
            }
        }
        /* get the URI for this process */
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, &local_uri, &cnt, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(sig);
            goto reply_fence;
        }
        /* if not NULL, then update our connection info as we might need
         * to send this proc a message at some point */
        if (NULL != local_uri) {
            orte_rml.set_contact_info(local_uri);
            free(local_uri);
        }
        /* if data was given, unpack and store it in the pmix dstore - it is okay
         * if there was no data, it's just a fence */
        cnt = 1;
        while (OPAL_SUCCESS == (rc = opal_dss.unpack(&xfer, &scope, &cnt, PMIX_SCOPE_T))) {
            found = true;  // at least one block of data is present
            /* unpack the buffer */
            cnt = 1;
            if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, &bptr, &cnt, OPAL_BUFFER))) {
                OPAL_ERROR_LOG(rc);
                OBJ_DESTRUCT(&xfer);
                OBJ_RELEASE(sig);
                return;
            }
            /* prep the value_t */
            OBJ_CONSTRUCT(&kv, opal_value_t);
            kv.key = strdup("modex");
            kv.type = OPAL_BYTE_OBJECT;
            kv.data.bo.bytes = (uint8_t*)bptr->base_ptr;
            kv.data.bo.size = bptr->bytes_used;
            if (PMIX_LOCAL == scope) {
                /* store it in the local-modex dstore handle */
                opal_output_verbose(2, pmix_server_output,
                                    "%s recvd LOCAL modex of size %d for proc %s",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (int)kv.data.bo.size,
                                    ORTE_NAME_PRINT(&peer->name));
                handle = pmix_server_local_handle;
                /* local procs will want this data */
                if (OPAL_SUCCESS != (rc = opal_dss.pack(&blocal, &bptr, 1, OPAL_BUFFER))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_DESTRUCT(&xfer);
                    OBJ_RELEASE(sig);
                    OBJ_DESTRUCT(&blocal);
                    OBJ_DESTRUCT(&bremote);
                    return;
                }
            } else if (PMIX_REMOTE == scope) {
                /* store it in the remote-modex dstore handle */
                opal_output_verbose(2, pmix_server_output,
                                    "%s recvd REMOTE modex of size %d for proc %s",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (int)kv.data.bo.size,
                                    ORTE_NAME_PRINT(&peer->name));
                handle = pmix_server_remote_handle;
                /* remote procs will want this data */
                if (OPAL_SUCCESS != (rc = opal_dss.pack(&bremote, &bptr, 1, OPAL_BUFFER))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_DESTRUCT(&xfer);
                    OBJ_RELEASE(sig);
                    OBJ_DESTRUCT(&blocal);
                    OBJ_DESTRUCT(&bremote);
                    return;
                }
            } else {
                /* must be for global dissemination */
                opal_output_verbose(2, pmix_server_output,
                                    "%s recvd GLOBAL modex of size %d for proc %s",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (int)kv.data.bo.size,
                                    ORTE_NAME_PRINT(&peer->name));
                handle = pmix_server_global_handle;
                /* local procs will want this data */
                if (OPAL_SUCCESS != (rc = opal_dss.pack(&blocal, &bptr, 1, OPAL_BUFFER))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_DESTRUCT(&xfer);
                    OBJ_RELEASE(sig);
                    OBJ_DESTRUCT(&blocal);
                    OBJ_DESTRUCT(&bremote);
                    return;
                }
                /* remote procs will want this data */
                if (OPAL_SUCCESS != (rc = opal_dss.pack(&bremote, &bptr, 1, OPAL_BUFFER))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_DESTRUCT(&xfer);
                    OBJ_RELEASE(sig);
                    OBJ_DESTRUCT(&blocal);
                    OBJ_DESTRUCT(&bremote);
                    return;
                }
            }
            if (OPAL_SUCCESS != (rc = opal_dstore.store(handle, &id, &kv))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&kv);
                OBJ_DESTRUCT(&xfer);
                OBJ_RELEASE(sig);
                return;
            }
            bptr->base_ptr = NULL;  // protect the data region
            OBJ_RELEASE(bptr);
            OBJ_DESTRUCT(&kv);
            cnt = 1;
        }
        if (OPAL_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            OPAL_ERROR_LOG(rc);
        }
        /* mark that we recvd data for this proc */
        ORTE_FLAG_SET(proc, ORTE_PROC_FLAG_DATA_RECVD);
        /* see if anyone is waiting for it - we send a response even if no data
         * was actually provided so we don't hang if no modex data is being given */
        OPAL_LIST_FOREACH_SAFE(req, nextreq, &pmix_server_pending_dmx_reqs, pmix_server_dmx_req_t) {
            if (id == req->target) {
                /* yes - deliver a copy */
                reply = OBJ_NEW(opal_buffer_t);
                if (NULL == req->proxy) {
                    /* pack the status */
                    ret = OPAL_SUCCESS;
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        return;
                    }
                    /* always pass the hostname */
                    OBJ_CONSTRUCT(&buf, opal_buffer_t);
                    OBJ_CONSTRUCT(&kv, opal_value_t);
                    kv.key = strdup(PMIX_HOSTNAME);
                    kv.type = OPAL_STRING;
                    kv.data.string = strdup(orte_process_info.nodename);
                    kp = &kv;
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(&buf, &kp, 1, OPAL_VALUE))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        OBJ_DESTRUCT(&buf);
                        OBJ_DESTRUCT(&kv);
                        return;
                    }
                    OBJ_DESTRUCT(&kv);
                    /* pack the hostname blob */
                    bptr = &buf;
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        OBJ_DESTRUCT(&xfer);
                        OBJ_DESTRUCT(&buf);
                        return;
                    }
                    OBJ_DESTRUCT(&buf);
                    /* pass the local blob(s) */
                    opal_dss.copy_payload(reply, &blocal);
                    /* use the PMIX send to return the data */
                    PMIX_SERVER_QUEUE_SEND(req->peer, req->tag, reply);
                } else {
                    /* pack the id of the requested proc */
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &id, 1, OPAL_UINT64))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        OBJ_DESTRUCT(&xfer);
                        OBJ_RELEASE(sig);
                        return;
                    }
                    /* pack the status */
                    ret = OPAL_SUCCESS;
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        return;
                    }
                    /* always pass the hostname */
                    OBJ_CONSTRUCT(&buf, opal_buffer_t);
                    OBJ_CONSTRUCT(&kv, opal_value_t);
                    kv.key = strdup(PMIX_HOSTNAME);
                    kv.type = OPAL_STRING;
                    kv.data.string = strdup(orte_process_info.nodename);
                    kp = &kv;
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(&buf, &kp, 1, OPAL_VALUE))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        OBJ_DESTRUCT(&buf);
                        OBJ_DESTRUCT(&kv);
                        return;
                    }
                    OBJ_DESTRUCT(&kv);
                    /* pack the hostname blob */
                    bptr = &buf;
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(reply);
                        OBJ_DESTRUCT(&xfer);
                        OBJ_DESTRUCT(&buf);
                        return;
                    }
                    OBJ_DESTRUCT(&buf);
                    /* pass the remote blob(s) */
                    opal_dss.copy_payload(reply, &bremote);
                    /* use RML to send the response */
                    orte_rml.send_buffer_nb(&req->proxy->name, reply,
                                            ORTE_RML_TAG_DIRECT_MODEX_RESP,
                                            orte_rml_send_callback, NULL);
                }
            }
            opal_list_remove_item(&pmix_server_pending_dmx_reqs, &req->super);
            OBJ_RELEASE(req);
        }
        OBJ_DESTRUCT(&blocal);
        OBJ_DESTRUCT(&bremote);

        /* send notification to myself */
        reply = OBJ_NEW(opal_buffer_t);
        /* pack the id of the sender */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &id, 1, OPAL_UINT64))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            OBJ_RELEASE(sig);
            goto reply_fence;
        }
        /* pack the socket of the sender */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &peer->sd, 1, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            OBJ_RELEASE(sig);
            goto reply_fence;
        }
        /* pass the tag that this sender is sitting on */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &tag, 1, OPAL_UINT32))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            OBJ_RELEASE(sig);
            goto reply_fence;
        }
        /* pack the signature */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &sig, 1, ORTE_SIGNATURE))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            OBJ_RELEASE(sig);
            goto reply_fence;
        }
        OBJ_RELEASE(sig);
        /* include any data that is to be globally shared */
        if (found && 0 < save.bytes_used) {
            bptr = &save;
            if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                goto reply_fence;
            }
        }
        OBJ_DESTRUCT(&save);
        /* send it to myself for processing */
        orte_rml.send_buffer_nb(ORTE_PROC_MY_NAME, reply,
                                ORTE_RML_TAG_DAEMON_COLL,
                                orte_rml_send_callback, NULL);
        return;
    reply_fence:
        if (PMIX_FENCE_CMD == cmd) {
            /* send a release message back to the sender so they don't hang */
            reply = OBJ_NEW(opal_buffer_t);
            /* pack the tag */
            if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &tag, 1, OPAL_UINT32))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                OBJ_DESTRUCT(&xfer);
                return;
            }
            PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
        }
        OBJ_DESTRUCT(&xfer);
        return;

    case PMIX_GET_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd GET",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* unpack the id of the proc whose data is being requested */
        cnt = 1;
        if (OPAL_SUCCESS != (rc = opal_dss.unpack(&xfer, &idreq, &cnt, OPAL_UINT64))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        /* lookup the proc object */
        memcpy((char*)&name, (char*)&idreq, sizeof(orte_process_name_t));
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd GET FROM PROC %s FOR PROC %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT((orte_process_name_t*)&id),
                            ORTE_NAME_PRINT(&name));
        if (NULL == (jdata = orte_get_job_data_object(name.jobid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, name.vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        /* if we have not yet received data for this proc, then we just
         * need to track the request */
        if (!ORTE_FLAG_TEST(proc, ORTE_PROC_FLAG_DATA_RECVD)) {
            /* are we already tracking it? */
            found = false;
            OPAL_LIST_FOREACH(req, &pmix_server_pending_dmx_reqs, pmix_server_dmx_req_t) {
                if (idreq == req->target) {
                    /* yes, so we don't need to send another request, but
                     * we do need to track that this peer also wants
                     * a copy */
                    found = true;
                    break;
                }
            }
            /* track the request */
            req = OBJ_NEW(pmix_server_dmx_req_t);
            OBJ_RETAIN(peer);  // just to be safe
            req->peer = peer;
            req->target = idreq;
            req->tag = tag;
            opal_list_append(&pmix_server_pending_dmx_reqs, &req->super);
            if (!found) {
                /* this is a new tracker - see if we need to send a data
                 * request to some remote daemon to resolve it */
                if (!ORTE_FLAG_TEST(proc, ORTE_PROC_FLAG_LOCAL)) {
                    /* nope - who is hosting this proc */
                    if (NULL == proc->node || NULL == proc->node->daemon) {
                        /* we are hosed - pack an error and return it */
                        reply = OBJ_NEW(opal_buffer_t);
                        ret = ORTE_ERR_NOT_FOUND;
                        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
                            ORTE_ERROR_LOG(rc);
                            OBJ_RELEASE(reply);
                            return;
                        }
                        PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
                        return;
                    }
                    /* setup the request */
                    reply = OBJ_NEW(opal_buffer_t);
                    /* pack the proc we want info about */
                    if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &idreq, 1, OPAL_UINT64))) {
                        ORTE_ERROR_LOG(rc);
                        return;
                    }
                    /* send the request - the recv will come back elsewhere
                     * and reply to the original requestor */
                    orte_rml.send_buffer_nb(&proc->node->daemon->name, reply,
                                            ORTE_RML_TAG_DIRECT_MODEX,
                                            orte_rml_send_callback, NULL);
                }
            }
            /* nothing further to do as we are waiting for data */
            return;
        }

        /* regardless of where this proc is located, we need to ensure
         * that the hostname it is on is *always* returned. Otherwise,
         * the non-blocking fence operation will cause us to fail if
         * the number of procs is below the cutoff as we will immediately
         * attempt to retrieve the hostname for each proc, but they may
         * not have posted their data by that time */
        if (ORTE_FLAG_TEST(proc, ORTE_PROC_FLAG_LOCAL)) {
            opal_output_verbose(2, pmix_server_output,
                                "%s recvd GET PROC %s IS LOCAL",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&name));
            kvp = NULL;
            kvp2 = NULL;
            /* retrieve the local blob for that proc */
            OBJ_CONSTRUCT(&values, opal_list_t);
            if (OPAL_SUCCESS == (ret = opal_dstore.fetch(pmix_server_local_handle, &idreq, "modex", &values))) {
                kvp = (opal_value_t*)opal_list_remove_first(&values);
            }
            OPAL_LIST_DESTRUCT(&values);
            /* retrieve the global blob for that proc */
            OBJ_CONSTRUCT(&values, opal_list_t);
            if (OPAL_SUCCESS == (ret = opal_dstore.fetch(pmix_server_global_handle, &idreq, "modex", &values))) {
                kvp2 = (opal_value_t*)opal_list_remove_first(&values);
            }
            OPAL_LIST_DESTRUCT(&values);
            /* return it */
            reply = OBJ_NEW(opal_buffer_t);
            /* pack the status */
            if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                OBJ_DESTRUCT(&xfer);
                return;
            }
            /* pass the hostname */
            OBJ_CONSTRUCT(&buf, opal_buffer_t);
            OBJ_CONSTRUCT(&kv, opal_value_t);
            kv.key = strdup(PMIX_HOSTNAME);
            kv.type = OPAL_STRING;
            kv.data.string = strdup(orte_process_info.nodename);
            kp = &kv;
            if (OPAL_SUCCESS != (rc = opal_dss.pack(&buf, &kp, 1, OPAL_VALUE))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                OBJ_DESTRUCT(&xfer);
                OBJ_DESTRUCT(&buf);
                OBJ_DESTRUCT(&kv);
                return;
            }
            OBJ_DESTRUCT(&kv);
            /* pack the blob */
            bptr = &buf;
            if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                OBJ_DESTRUCT(&xfer);
                OBJ_DESTRUCT(&buf);
                return;
            }
            OBJ_DESTRUCT(&buf);
            /* local blob */
            if (NULL != kvp) {
                opal_output_verbose(2, pmix_server_output,
                                    "%s passing local blob of size %d from proc %s to proc %s",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (int)kvp->data.bo.size,
                                    ORTE_NAME_PRINT(&name),
                                    ORTE_NAME_PRINT(&peer->name));
                OBJ_CONSTRUCT(&buf, opal_buffer_t);
                opal_dss.load(&buf, kvp->data.bo.bytes, kvp->data.bo.size);
                /* protect the data */
                kvp->data.bo.bytes = NULL;
                kvp->data.bo.size = 0;
                bptr = &buf;
                if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    OBJ_DESTRUCT(&xfer);
                    OBJ_DESTRUCT(&buf);
                    return;
                }
                OBJ_DESTRUCT(&buf);
                OBJ_RELEASE(kvp);
            }
            /* global blob */
            if (NULL != kvp2) {
                opal_output_verbose(2, pmix_server_output,
                                    "%s passing global blob of size %d from proc %s to proc %s",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (int)kvp2->data.bo.size,
                                    ORTE_NAME_PRINT(&name),
                                    ORTE_NAME_PRINT(&peer->name));
                OBJ_CONSTRUCT(&buf, opal_buffer_t);
                opal_dss.load(&buf, kvp2->data.bo.bytes, kvp2->data.bo.size);
                /* protect the data */
                kvp2->data.bo.bytes = NULL;
                kvp2->data.bo.size = 0;
                bptr = &buf;
                if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                    OBJ_DESTRUCT(&xfer);
                    OBJ_DESTRUCT(&buf);
                    return;
                }
                OBJ_DESTRUCT(&buf);
                OBJ_RELEASE(kvp2);
            }
            PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
            OBJ_DESTRUCT(&xfer);
            return;
        }

        opal_output_verbose(2, pmix_server_output,
                            "%s recvd GET PROC %s IS NON-LOCAL",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&name));
        OBJ_DESTRUCT(&xfer);  // done with this
        /* since we already have this proc's data, we know that the
         * entire blob is stored in the remote handle - so get it */
        OBJ_CONSTRUCT(&values, opal_list_t);
        if (OPAL_SUCCESS == (ret = opal_dstore.fetch(pmix_server_remote_handle, &idreq, "modex", &values))) {
            kvp = (opal_value_t*)opal_list_remove_first(&values);
            OPAL_LIST_DESTRUCT(&values);
            opal_output_verbose(2, pmix_server_output,
                                "%s passing blob of size %d from remote proc %s to proc %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                (int)kvp->data.bo.size,
                                ORTE_NAME_PRINT(&name),
                                ORTE_NAME_PRINT(&peer->name));
            OBJ_CONSTRUCT(&buf, opal_buffer_t);
            opal_dss.load(&buf, kvp->data.bo.bytes, kvp->data.bo.size);
            /* protect the data */
            kvp->data.bo.bytes = NULL;
            kvp->data.bo.size = 0;
            OBJ_RELEASE(kvp);
            reply = OBJ_NEW(opal_buffer_t);
            /* pack the status */
            if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                OBJ_DESTRUCT(&buf);
                return;
            }
            /* xfer the data - the blobs are in the buffer,
             * so don't repack them. They will include the remote
             * hostname, so don't add it again */
            opal_dss.copy_payload(reply, &buf);
            OBJ_DESTRUCT(&buf);
            PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
            return;
        }
        OPAL_LIST_DESTRUCT(&values);
        /* if we get here, then the data should have been there, but wasn't found
         * for some bizarre reason - pass back an error to ensure we don't block */
        reply = OBJ_NEW(opal_buffer_t);
        /* pack the error status */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            return;
        }
        PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
        return;

    case PMIX_GETATTR_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd GETATTR",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* create the attrs buffer */
        OBJ_CONSTRUCT(&buf, opal_buffer_t);
        /* look up this proc */
        memcpy((char*)&name, (char*)&id, sizeof(orte_process_name_t));
        if (NULL == (jdata = orte_get_job_data_object(name.jobid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            OBJ_DESTRUCT(&buf);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, name.vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            OBJ_DESTRUCT(&buf);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        /* mark the proc as having registered */
        ORTE_ACTIVATE_PROC_STATE(&proc->name, ORTE_PROC_STATE_REGISTERED);
        /* stuff the values corresponding to the list of supported attrs */
        if (ORTE_SUCCESS != (ret = stuff_proc_values(&buf, jdata, proc))) {
            ORTE_ERROR_LOG(ret);
            OBJ_DESTRUCT(&buf);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        /* return it */
        reply = OBJ_NEW(opal_buffer_t);
        /* pack the status */
        if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &ret, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(reply);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        if (OPAL_SUCCESS == ret) {
            /* pack the buffer */
            bptr = &buf;
            if (OPAL_SUCCESS != (rc = opal_dss.pack(reply, &bptr, 1, OPAL_BUFFER))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(reply);
                OBJ_DESTRUCT(&xfer);
                OBJ_DESTRUCT(&buf);
                return;
            }
        }
        OBJ_DESTRUCT(&buf);
        PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
        OBJ_DESTRUCT(&xfer);
        return;

    case PMIX_FINALIZE_CMD:
        opal_output_verbose(2, pmix_server_output,
                            "%s recvd FINALIZE",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* look up this proc */
        memcpy((char*)&name, (char*)&id, sizeof(orte_process_name_t));
        if (NULL == (jdata = orte_get_job_data_object(name.jobid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            OBJ_DESTRUCT(&buf);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, name.vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            OBJ_DESTRUCT(&buf);
            OBJ_DESTRUCT(&xfer);
            return;
        }
        /* mark the proc as having deregistered */
        ORTE_FLAG_SET(proc, ORTE_PROC_FLAG_HAS_DEREG);
        /* send the release */
        reply = OBJ_NEW(opal_buffer_t);
        PMIX_SERVER_QUEUE_SEND(peer, tag, reply);
        OBJ_DESTRUCT(&xfer);
        break;

    default:
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OBJ_DESTRUCT(&xfer);
        return;
    }
}

void pmix_server_recv_handler(int sd, short flags, void *cbdata)
{
    pmix_server_peer_t* peer = (pmix_server_peer_t*)cbdata;
    int rc;

    opal_output_verbose(2, pmix_server_output,
                        "%s:usock:recv:handler called for peer %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name));

    switch (peer->state) {
    case PMIX_SERVER_CONNECT_ACK:
        if (ORTE_SUCCESS == (rc = pmix_server_peer_recv_connect_ack(peer, peer->sd, NULL))) {
            opal_output_verbose(2, pmix_server_output,
                                "%s:usock:recv:handler starting send/recv events",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            /* we connected! Start the send/recv events */
            if (!peer->recv_ev_active) {
                opal_event_add(&peer->recv_event, 0);
                peer->recv_ev_active = true;
            }
            if (peer->timer_ev_active) {
                opal_event_del(&peer->timer_event);
                peer->timer_ev_active = false;
            }
            /* if there is a message waiting to be sent, queue it */
            if (NULL == peer->send_msg) {
                peer->send_msg = (pmix_server_send_t*)opal_list_remove_first(&peer->send_queue);
            }
            if (NULL != peer->send_msg && !peer->send_ev_active) {
                opal_event_add(&peer->send_event, 0);
                peer->send_ev_active = true;
            }
            /* update our state */
            peer->state = PMIX_SERVER_CONNECTED;
        } else {
            opal_output_verbose(2, pmix_server_output,
                                "%s UNABLE TO COMPLETE CONNECT ACK WITH %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&peer->name));
            opal_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
            return;
        }
        break;
    case PMIX_SERVER_CONNECTED:
        opal_output_verbose(2, pmix_server_output,
                            "%s:usock:recv:handler CONNECTED",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* allocate a new message and setup for recv */
        if (NULL == peer->recv_msg) {
            opal_output_verbose(2, pmix_server_output,
                                "%s:usock:recv:handler allocate new recv msg",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            peer->recv_msg = OBJ_NEW(pmix_server_recv_t);
            if (NULL == peer->recv_msg) {
                opal_output(0, "%s-%s pmix_server_peer_recv_handler: unable to allocate recv message\n",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&(peer->name)));
                return;
            }
            /* start by reading the header */
            peer->recv_msg->rdptr = (char*)&peer->recv_msg->hdr;
            peer->recv_msg->rdbytes = sizeof(pmix_server_hdr_t);
        }
        /* if the header hasn't been completely read, read it */
        if (!peer->recv_msg->hdr_recvd) {
            opal_output_verbose(2, pmix_server_output,
                                "%s:usock:recv:handler read hdr",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            if (ORTE_SUCCESS == (rc = read_bytes(peer))) {
                /* completed reading the header */
                peer->recv_msg->hdr_recvd = true;
                /* if this is a zero-byte message, then we are done */
                if (0 == peer->recv_msg->hdr.nbytes) {
                    opal_output_verbose(2, pmix_server_output,
                                        "%s RECVD ZERO-BYTE MESSAGE FROM %s for tag %d",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        ORTE_NAME_PRINT(&peer->name), peer->recv_msg->hdr.tag);
                    peer->recv_msg->data = NULL;  // make sure
                    peer->recv_msg->rdptr = NULL;
                    peer->recv_msg->rdbytes = 0;
                } else {
                    opal_output_verbose(2, pmix_server_output,
                                        "%s:usock:recv:handler allocate data region of size %lu",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (unsigned long)peer->recv_msg->hdr.nbytes);
                    /* allocate the data region */
                    peer->recv_msg->data = (char*)malloc(peer->recv_msg->hdr.nbytes);
                    /* point to it */
                    peer->recv_msg->rdptr = peer->recv_msg->data;
                    peer->recv_msg->rdbytes = peer->recv_msg->hdr.nbytes;
                }
                /* fall thru and attempt to read the data */
            } else if (ORTE_ERR_RESOURCE_BUSY == rc ||
                       ORTE_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                /* close the connection */
                opal_output_verbose(2, pmix_server_output,
                                    "%s:usock:recv:handler error reading bytes - closing connection",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                peer->state = PMIX_SERVER_CLOSED;
                CLOSE_THE_SOCKET(peer->sd);
                return;
            }
        }

        if (peer->recv_msg->hdr_recvd) {
            /* continue to read the data block - we start from
             * wherever we left off, which could be at the
             * beginning or somewhere in the message
             */
            if (ORTE_SUCCESS == (rc = read_bytes(peer))) {
                /* we recvd all of the message */
                opal_output_verbose(2, pmix_server_output,
                                    "%s RECVD COMPLETE MESSAGE FROM %s OF %d BYTES TAG %d",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    ORTE_NAME_PRINT((orte_process_name_t*)&(peer->recv_msg->hdr.id)),
                                    (int)peer->recv_msg->hdr.nbytes,
                                    peer->recv_msg->hdr.tag);
                /* process the message */
                process_message(peer);
            } else if (ORTE_ERR_RESOURCE_BUSY == rc ||
                       ORTE_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                // report the error
                opal_output(0, "%s-%s pmix_server_peer_recv_handler: unable to recv message",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&(peer->name)));
                /* turn off the recv event */
                opal_event_del(&peer->recv_event);
                peer->recv_ev_active = false;
                peer->state = PMIX_SERVER_CLOSED;
                CLOSE_THE_SOCKET(peer->sd);
                return;
            }
        }
        break;
    default: 
        opal_output(0, "%s-%s pmix_server_peer_recv_handler: invalid socket state(%d)", 
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&(peer->name)),
                    peer->state);
        // pmix_server_peer_close(peer);
        break;
    }
}

/*
 * A blocking recv on a non-blocking socket. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 */
static bool peer_recv_blocking(pmix_server_peer_t* peer, int sd,
                               void* data, size_t size)
{
    unsigned char* ptr = (unsigned char*)data;
    size_t cnt = 0;

    opal_output_verbose(2, pmix_server_output,
                        "%s waiting for connect ack from %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&(peer->name)));

    while (cnt < size) {
        int retval = recv(sd, (char *)ptr+cnt, size-cnt, 0);

        /* remote closed connection */
        if (retval == 0) {
            opal_output_verbose(2, pmix_server_output,
                                "%s-%s tcp_peer_recv_blocking: "
                                "peer closed connection: peer state %d",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&(peer->name)),
                                (NULL == peer) ? 0 : peer->state);
            if (NULL != peer) {
                peer->state = PMIX_SERVER_FAILED;
            }
            CLOSE_THE_SOCKET(sd);
            return false;
        }

        /* socket is non-blocking so handle errors */
        if (retval < 0) {
            if (opal_socket_errno != EINTR && 
                opal_socket_errno != EAGAIN && 
                opal_socket_errno != EWOULDBLOCK) {
                if (peer->state == PMIX_SERVER_CONNECT_ACK) {
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
                    opal_output_verbose(2, pmix_server_output,
                                        "%s connect ack received error %s from %s",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        strerror(opal_socket_errno),
                                        (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&(peer->name)));
                    return false;
                } else {
                    opal_output(0, 
                                "%s tcp_peer_recv_blocking: "
                                "recv() failed for %s: %s (%d)\n",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&(peer->name)),
                                strerror(opal_socket_errno),
                                opal_socket_errno);
                    if (NULL != peer) {
                        peer->state = PMIX_SERVER_FAILED;
                    }
                    CLOSE_THE_SOCKET(sd);
                    return false;
                }
            }
            continue;
        }
        cnt += retval;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect ack received from %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&(peer->name)));
    return true;
}

/*
 *  Receive the peers globally unique process identification from a newly
 *  connected socket and verify the expected response. If so, move the
 *  socket to a connected state.
 */
int pmix_server_peer_recv_connect_ack(pmix_server_peer_t* pr,
                                      int sd, pmix_server_hdr_t *dhdr)
{
    char *msg;
    char *version;
    int rc;
    opal_sec_cred_t creds;
    pmix_server_hdr_t hdr;
    pmix_server_peer_t *peer;
    uint64_t *ui64;
    orte_process_name_t sender;

    opal_output_verbose(2, pmix_server_output,
                        "%s RECV CONNECT ACK FROM %s ON SOCKET %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (NULL == pr) ? "UNKNOWN" : ORTE_NAME_PRINT(&pr->name), sd);

    peer = pr;
    /* get the header */
    if (peer_recv_blocking(peer, sd, &hdr, sizeof(pmix_server_hdr_t))) {
        if (NULL != peer) {
            /* If the peer state is CONNECT_ACK, then we were waiting for
             * the connection to be ack'd
             */
            if (peer->state != PMIX_SERVER_CONNECT_ACK) {
                /* handshake broke down - abort this connection */
                opal_output(0, "%s RECV CONNECT BAD HANDSHAKE (%d) FROM %s ON SOCKET %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), peer->state,
                            ORTE_NAME_PRINT(&(peer->name)), sd);
                peer->state = PMIX_SERVER_CLOSED;
                CLOSE_THE_SOCKET(peer->sd);
                return ORTE_ERR_UNREACH;
            }
        }
    } else {
        /* unable to complete the recv */
        opal_output_verbose(2, pmix_server_output,
                            "%s unable to complete recv of connect-ack from %s ON SOCKET %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&peer->name), sd);
        return ORTE_ERR_UNREACH;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack recvd from %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : ORTE_NAME_PRINT(&peer->name));

    /* if the requestor wanted the header returned, then do so now */
    if (NULL != dhdr) {
        *dhdr = hdr;
    }

    if (hdr.type != PMIX_USOCK_IDENT) {
        opal_output(0, "%s tcp_peer_recv_connect_ack: invalid header type: %d\n", 
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), hdr.type);
        if (NULL != peer) {
            peer->state = PMIX_SERVER_FAILED;
        }
        CLOSE_THE_SOCKET(sd);
        return ORTE_ERR_UNREACH;
    }

    /* if we don't already have it, get the peer */
    if (NULL == peer) {
        memcpy(&sender, &hdr.id, sizeof(opal_identifier_t));
        peer = pmix_server_peer_lookup(sd);
        if (NULL == peer) {
            opal_output_verbose(2, pmix_server_output,
                                "%s pmix_server_recv_connect: connection from new peer",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            peer = OBJ_NEW(pmix_server_peer_t);
            peer->sd = sd;
            peer->name = sender;
            peer->state = PMIX_SERVER_ACCEPTING;
            ui64 = (uint64_t*)(&peer->name);
            if (OPAL_SUCCESS != opal_hash_table_set_value_uint64(pmix_server_peers, (*ui64), peer)) {
                OBJ_RELEASE(peer);
                CLOSE_THE_SOCKET(sd);
                return ORTE_ERR_UNREACH;
            }
        } else if (PMIX_SERVER_CONNECTED == peer->state ||
                   PMIX_SERVER_CONNECTING == peer->state ||
                   PMIX_SERVER_CONNECT_ACK == peer->state) {
            opal_output_verbose(2, pmix_server_output,
                                "%s EXISTING CONNECTION WITH %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&sender));
            if (peer->recv_ev_active) {
                opal_event_del(&peer->recv_event);
                peer->recv_ev_active = false;
            }
            if (peer->send_ev_active) {
                opal_event_del(&peer->send_event);
                peer->send_ev_active = false;
            }
            if (0 < peer->sd) {
                CLOSE_THE_SOCKET(peer->sd);
                peer->sd = -1;
            }
            CLOSE_THE_SOCKET(sd);
        }
    } else {
        /* compare the peers name to the expected value */
        if (OPAL_EQUAL != orte_util_compare_name_fields(ORTE_NS_CMP_ALL, &peer->name, &sender)) {
            opal_output(0, "%s tcp_peer_recv_connect_ack: "
                        "received unexpected process identifier %s from %s\n",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&(sender)),
                        ORTE_NAME_PRINT(&(peer->name)));
            peer->state = PMIX_SERVER_FAILED;
            CLOSE_THE_SOCKET(peer->sd);
            return ORTE_ERR_UNREACH;
        }
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack header from %s is okay",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name));

    /* get the authentication and version payload */
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        peer->state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(peer->sd);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }
    if (!peer_recv_blocking(peer, sd, msg, hdr.nbytes)) {
        /* unable to complete the recv */
        opal_output_verbose(2, pmix_server_output,
                            "%s unable to complete recv of connect-ack from %s ON SOCKET %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&peer->name), peer->sd);
        free(msg);
        return ORTE_ERR_UNREACH;
    }

    /* check that this is from a matching version */
    version = (char*)(msg);
    if (0 != strcmp(version, orte_version_string)) {
        opal_output(0, "%s tcp_peer_recv_connect_ack: "
                    "received different version from %s: %s instead of %s\n",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&(peer->name)),
                    version, orte_version_string);
        peer->state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(peer->sd);
        free(msg);
        return ORTE_ERR_UNREACH;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack version from %s matches ours",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name));

    /* check security token */
    creds.credential = (char*)(msg + strlen(version) + 1);
    creds.size = hdr.nbytes - strlen(version) - 1;
    if (OPAL_SUCCESS != (rc = opal_sec.authenticate(&creds))) {
        ORTE_ERROR_LOG(rc);
    }
    free(msg);

    opal_output_verbose(2, pmix_server_output,
                        "%s connect-ack %s authenticated",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name));

    /* if the requestor wanted the header returned, then they
     * will complete their processing
     */
    if (NULL != dhdr) {
        return ORTE_SUCCESS;
    }

    /* connected */
    pmix_server_peer_connected(peer);
    if (2 <= opal_output_get_verbosity(pmix_server_output)) {
        pmix_server_peer_dump(peer, "connected");
    }
    return ORTE_SUCCESS;
}

/*
 * Check the status of the connection. If the connection failed, will retry
 * later. Otherwise, send this processes identifier to the peer on the
 * newly connected socket.
 */
static void complete_connect(pmix_server_peer_t *peer)
{
    int so_error = 0;
    opal_socklen_t so_length = sizeof(so_error);

    opal_output_verbose(2, pmix_server_output,
                        "%s:usock:complete_connect called for peer %s on socket %d",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&peer->name), peer->sd);

    /* check connect completion status */
    if (getsockopt(peer->sd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &so_length) < 0) {
        opal_output(0, "%s usock_peer_complete_connect: getsockopt() to %s failed: %s (%d)\n", 
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&(peer->name)),
                    strerror(opal_socket_errno),
                    opal_socket_errno);
        peer->state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(peer->sd);
        return;
    }

    if (so_error == EINPROGRESS) {
        opal_output_verbose(2, pmix_server_output,
                            "%s:usock:send:handler still in progress",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return;
    } else if (so_error == ECONNREFUSED || so_error == ETIMEDOUT) {
        opal_output_verbose(2, pmix_server_output,
                            "%s-%s usock_peer_complete_connect: connection failed: %s (%d)",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&(peer->name)),
                            strerror(so_error),
                            so_error);
        peer->state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(peer->sd);
        return;
    } else if (so_error != 0) {
        /* No need to worry about the return code here - we return regardless
           at this point, and if an error did occur a message has already been
           printed for the user */
        opal_output_verbose(2, pmix_server_output,
                            "%s-%s usock_peer_complete_connect: "
                            "connection failed with error %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&(peer->name)), so_error);
        peer->state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(peer->sd);
        return;
    }

    opal_output_verbose(2, pmix_server_output,
                        "%s usock_peer_complete_connect: "
                        "sending ack to %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&(peer->name)));

    if (pmix_server_send_connect_ack(peer) == ORTE_SUCCESS) {
        peer->state = PMIX_SERVER_CONNECT_ACK;
        opal_output_verbose(2, pmix_server_output,
                            "%s usock_peer_complete_connect: "
                            "setting read event on connection to %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&(peer->name)));
        
        if (!peer->recv_ev_active) {
            opal_event_add(&peer->recv_event, 0);
            peer->recv_ev_active = true;
        }
    } else {
        opal_output(0, "%s usock_peer_complete_connect: unable to send connect ack to %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&(peer->name)));
        peer->state = PMIX_SERVER_FAILED;
        CLOSE_THE_SOCKET(peer->sd);
    }
}
