/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/include/types.h"
#include "pmix_stdint.h"
#include "pmix_socket_errno.h"

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

#include "src/api/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "event.h"

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"

#include "pmix_server.h"

// usock forward declarations
static int pmix_usock_connect(struct sockaddr *addr, int max_retries);
static void pmix_usock_process_msg(int fd, short flags, void *cbdata);
static void pmix_usock_send_recv(int fd, short args, void *cbdata);
static void pmix_usock_send_handler(int sd, short flags, void *cbdata);
static void pmix_usock_recv_handler(int sd, short flags, void *cbdata);
static void pmix_usock_dump(const char* msg);
static int usock_send_connect_ack(int sd);
static int usock_recv_connect_ack(int sd);
static int usock_set_nonblocking(int sd);
static int usock_set_blocking(int sd);
static int send_bytes(int sd, char **buf, size_t *remain);
static int read_bytes(int sd, char **buf, size_t *remain);

// local variables
static int init_cntr = 0;
static char *local_uri = NULL;
static struct sockaddr_un address;
static int server;
static pmix_errhandler_fn_t errhandler = NULL;

// global variables
int PMIx_Comm_Init()
{
    char **uri, *evar;
    int rc;

    ++init_cntr;
    if (1 < init_cntr) {
        return PMIX_SUCCESS;
    }

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }
    
    setup_globals();
    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        pmix_client_globals.debug_level = strtol(evar, NULL, 10);
        pmix_client_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_client_globals.debug_output,
                                  pmix_client_globals.debug_level);
    }
    pmix_bfrop_open();

    
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: init called");

    /* if we don't have a path to the daemon rendezvous point,
     * then we need to return an error */
    if (NULL == (evar = getenv("PMIX_SERVER_URI"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_SERVER_NOT_AVAIL;
    }
    pmix_client_globals.uri = strdup(evar);
    
    /* we also require the namespace */
    if (NULL == (evar = getenv("PMIX_NAMESPACE"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_INVALID_NAMESPACE;
    }
    (void)strncpy(namespace, evar, PMIX_MAX_VALLEN);
    (void)strncpy(pmix_client_globals.namespace, evar, PMIX_MAX_VALLEN);

    /* we also require our rank */
    if (NULL == (evar = getenv("PMIX_RANK"))) {
        /* let the caller know that the server isn't available yet */
        return PMIX_ERR_DATA_VALUE_NOT_FOUND;
    }
    *rank = strtol(evar, NULL, 10);

    /* setup the path to the daemon rendezvous point */
    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix: constructing component fields with server %s",
                        pmix_client_globals.uri);

    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    uri = pmix_argv_split(pmix_client_globals.uri, ':');
    if (2 != pmix_argv_count(uri)) {
        return PMIX_ERROR;
    }
    /* if the rendezvous file doesn't exist, that's an error */
    if (0 != access(uri[1], R_OK)) {
        return PMIX_ERR_NOT_FOUND;
    }
    server = strtoull(uri[0], NULL, 10);
    snprintf(address.sun_path, sizeof(address.sun_path)-1, "%s", uri[1]);
    pmix_argv_free(uri);

    pmix_client_globals.address = address;

    /* create an event base and progress thread for us */
    if (NULL == (pmix_client_globals.evbase = pmix_start_progress_thread())) {
        return PMIX_ERROR;
    }

    /* connect to the server */
    if (PMIX_SUCCESS != (rc = connect_to_server())){
        return rc;
    }
    
    return PMIX_SUCCESS;
}

int PMIx_Comm_Finalize(void)
{
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    pmix_cmd_t cmd = PMIX_FINALIZE_CMD;
    int rc;

    if (1 != init_cntr) {
        --init_cntr;
       return PMIX_SUCCESS;
    }
    init_cntr = 0;

    pmix_output_verbose(2, pmix_client_globals.debug_output,
                        "pmix:native finalize called");

    if (NULL == local_uri) {
        /* nothing was setup, so return */
        return PMIX_SUCCESS;
    }

    if ( 0 <= pmix_client_globals.sd ) {
        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        msg = OBJ_NEW(pmix_buffer_t);
        /* pack the cmd */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(msg);
            return rc;
        }

        /* create a callback object as we need to pass it to the
         * recv routine so we know which callback to use when
         * the return message is recvd */
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;

        pmix_output_verbose(2, pmix_client_globals.debug_output,
                            "pmix:native sending finalize sync to server");

        /* push the message into our event base to send to the server */
        PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

        /* wait for the ack to return */
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        OBJ_RELEASE(cb);
    }

    if (NULL != pmix_client_globals.evbase) {
        pmix_stop_progress_thread(pmix_client_globals.evbase);
        pmix_client_globals.evbase = NULL;
    }

    if (0 <= pmix_client_globals.sd) {
        CLOSE_THE_SOCKET(pmix_client_globals.sd);
    }

    pmix_bfrop_close();

    return PMIX_SUCCESS;
}

void PMIx_free_value_data(pmix_value_t *val)
{
    size_t n;
    char **str;
    
    if (PMIX_STRING == val->type &&
        NULL != val->data.string) {
        free(val->data.string);
        return;
    }
    if (PMIX_ARRAY == val->type) {
        if (NULL == val->data.array.array) {
            return;
        }
        if (PMIX_STRING == val->data.array.type) {
            str = (char**)val->data.array.array;
            for (n=0; n < val->data.array.size; n++) {
                if (NULL != str[n]) {
                    free(str[n]);
                }
            }
        }
        free(val->data.array.array);
    }
    /* all other types have no malloc'd storage */
}



#include "src/usock/usock.c"
#include "src/usock/usock_sendrecv.c"
