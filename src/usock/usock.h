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

#ifndef USOCK_H
#define USOCK_H

#include "pmix_config.h"
#include "src/include/types.h"
#include "src/api/pmix_common.h"

#include <unistd.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif
#include <event.h>

#include "src/buffer_ops/buffer_ops.h"
#include "src/class/pmix_list.h"

/* define a command type for communicating to the
 * pmix server */
#define PMIX_CMD PMIX_UINT32

/* define some commands */
typedef enum {
    PMIX_ABORT_CMD,
    PMIX_FENCE_CMD,
    PMIX_FENCENB_CMD,
    PMIX_GET_CMD,
    PMIX_GETNB_CMD,
    PMIX_FINALIZE_CMD,
    PMIX_PUBLISH_CMD,
    PMIX_LOOKUP_CMD,
    PMIX_UNPUBLISH_CMD,
    PMIX_SPAWN_CMD,
    PMIX_CONNECT_CMD,
    PMIX_DISCONNECT_CMD
} pmix_cmd_t;

/* define some message types */
#define PMIX_USOCK_IDENT  1
#define PMIX_USOCK_USER   2

/* header for messages */
typedef struct {
    char namespace[PMIX_MAX_NSLEN];
    int rank;
    uint8_t type;
    uint32_t tag;
    size_t nbytes;
} pmix_usock_hdr_t;

/* internally used cbfunc */
typedef void (*pmix_usock_cbfunc_t)(int sd, pmix_usock_hdr_t *hdr,
                                    pmix_buffer_t *buf, void *cbdata);

/* usock structure for sending a message */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    pmix_usock_hdr_t hdr;
    char *data;
    bool hdr_sent;
    char *sdptr;
    size_t sdbytes;
} pmix_usock_send_t;
OBJ_CLASS_DECLARATION(pmix_usock_send_t);

/* usock structure for recving a message */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    int sd;
    pmix_usock_hdr_t hdr;
    char *data;
    bool hdr_recvd;
    char *rdptr;
    size_t rdbytes;
} pmix_usock_recv_t;
OBJ_CLASS_DECLARATION(pmix_usock_recv_t);

/* usock structure for tracking posted recvs */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    uint32_t tag;
    pmix_usock_cbfunc_t cbfunc;
    void *cbdata;
} pmix_usock_posted_recv_t;
OBJ_CLASS_DECLARATION(pmix_usock_posted_recv_t);

/* object for tracking peers - each peer can have multiple
 * connections. This can occur if the initial app executes
 * a fork/exec, and the child initiates its own connection
 * back to the PMIx server. Thus, the trackers should be "indexed"
 * by the socket, not the process namespace/rank */
typedef struct pmix_peer_t {
    pmix_list_item_t super;
    char namespace[PMIX_MAX_NSLEN];
    int rank;
    int sd;
    pmix_event_t send_event;    /**< registration with event thread for send events */
    bool send_ev_active;
    pmix_event_t recv_event;    /**< registration with event thread for recv events */
    bool recv_ev_active;
    pmix_list_t send_queue;      /**< list of messages to send */
    pmix_usock_send_t *send_msg; /**< current send in progress */
    pmix_usock_recv_t *recv_msg; /**< current recv in progress */
} pmix_peer_t;
OBJ_CLASS_DECLARATION(pmix_peer_t);

/* usock struct for posting send/recv request */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_peer_t *peer;
    pmix_buffer_t *bfr;
    pmix_usock_cbfunc_t cbfunc;
    void *cbdata;
} pmix_usock_sr_t;
OBJ_CLASS_DECLARATION(pmix_usock_sr_t);

/* usock struct for tracking ops */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    volatile bool active;
    pmix_usock_cbfunc_t internal_cbfunc;
    pmix_buffer_t data;
    pmix_cbfunc_t cbfunc;
    void *cbdata;
    char *namespace;
    int rank;
    char *key;
} pmix_cb_t;
OBJ_CLASS_DECLARATION(pmix_cb_t);

/* internal convenience macros */
#define PMIX_ACTIVATE_SEND_RECV(p, b, cb, d)                            \
    do {                                                                \
        int rc = -1;                                                    \
        pmix_usock_sr_t *ms;                                            \
        pmix_output_verbose(5, pmix_globals.debug_output,               \
                            "[%s:%d] post send to server",              \
                            __FILE__, __LINE__);                        \
        ms = OBJ_NEW(pmix_usock_sr_t);                                  \
        ms->peer = (p);                                                 \
        ms->bfr = (b);                                                  \
        ms->cbfunc = (cb);                                              \
        ms->cbdata = (d);                                               \
        rc = event_assign(&((ms)->ev), pmix_globals.evbase, -1,         \
                          EV_WRITE, pmix_usock_send_recv, (ms));        \
        pmix_output_verbose(10, pmix_globals.debug_output,              \
                            "event_assign returned %d", rc);            \
        event_active(&((ms)->ev), EV_WRITE, 1);                         \
    } while(0);

#define PMIX_ACTIVATE_POST_MSG(ms)                                      \
    do {                                                                \
        pmix_output_verbose(5, pmix_globals.debug_output,               \
                            "[%s:%d] post msg",                         \
                            __FILE__, __LINE__);                        \
        event_assign(&((ms)->ev), pmix_globals.evbase, -1,              \
                     EV_WRITE, pmix_usock_process_msg, (ms));           \
        event_active(&((ms)->ev), EV_WRITE, 1);                         \
    } while(0);

#define CLOSE_THE_SOCKET(socket)                                \
    do {                                                        \
        shutdown(socket, 2);                                    \
        close(socket);                                          \
    } while(0)


#define PMIX_WAIT_FOR_COMPLETION(a)             \
    do {                                        \
        while ((a)) {                           \
            usleep(10);                         \
        }                                       \
    } while (0);

/* usock common variables */
typedef struct {
    pmix_list_t posted_recvs;     // list of pmix_usock_posted_recv_t
} pmix_usock_globals_t;
extern pmix_usock_globals_t pmix_usock_globals;

/* usock common functions */
void pmix_usock_init(void);
void pmix_usock_finalize(void);
int pmix_usock_set_nonblocking(int sd);
int pmix_usock_send_blocking(int sd, char *ptr, size_t size);
int pmix_usock_recv_blocking(int sd, char *data, size_t size);
void pmix_usock_send_recv(int sd, short args, void *cbdata);
void pmix_usock_send_handler(int sd, short flags, void *cbdata);
void pmix_usock_recv_handler(int sd, short flags, void *cbdata);
void pmix_usock_process_msg(int fd, short flags, void *cbdata);

#endif // USOCK_H
