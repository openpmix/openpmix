/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_CLIENT_H
#define PMIX_CLIENT_H

#include "pmix_config.h"

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
#include "src/api/pmix.h"

BEGIN_C_DECLS

extern int pmix_client_debug_output;
extern pmix_identifier_t pmix_client_myid;

/**
 * the state of the connection to the server
 */
typedef enum {
    PMIX_USOCK_UNCONNECTED,
    PMIX_USOCK_CLOSED,
    PMIX_USOCK_RESOLVE,
    PMIX_USOCK_CONNECTING,
    PMIX_USOCK_CONNECT_ACK,
    PMIX_USOCK_CONNECTED,
    PMIX_USOCK_FAILED,
    PMIX_USOCK_ACCEPTING
} pmix_usock_state_t;

/* define a command type for communicating to the
 * pmix server */
#define PMIX_CMD PMIX_UINT8

/* define some commands */
typedef enum {
    PMIX_ABORT_CMD,
    PMIX_FENCE_CMD,
    PMIX_FENCENB_CMD,
    PMIX_PUT_CMD,
    PMIX_GET_CMD,
    PMIX_GETNB_CMD,
    PMIX_FINALIZE_CMD,
    PMIX_GETATTR_CMD,
    PMIX_PUBLISH_CMD,
    PMIX_LOOKUP_CMD,
    PMIX_UNPUBLISH_CMD,
    PMIX_SPAWN_CMD
} pmix_cmd_t;

/* define some message types */
#define PMIX_USOCK_IDENT  1
#define PMIX_USOCK_USER   2

/* internally used cbfunc */
typedef void (*pmix_usock_cbfunc_t)(pmix_buffer_t *buf, void *cbdata);

/* header for messages */
typedef struct {
    pmix_identifier_t id;
    uint8_t type;
    uint32_t tag;
    size_t nbytes;
} pmix_usock_hdr_t;

/* usock structure for sending a message */
typedef struct {
    pmix_list_item_t super;
    event_t ev;
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
    event_t ev;
    pmix_usock_hdr_t hdr;
    char *data;
    bool hdr_recvd;
    char *rdptr;
    size_t rdbytes;
} pmix_usock_recv_t;
OBJ_CLASS_DECLARATION(pmix_usock_recv_t);

/* usock struct for posting send/recv request */
typedef struct {
    pmix_object_t super;
    event_t ev;
    pmix_buffer_t *bfr;
    pmix_usock_cbfunc_t cbfunc;
    void *cbdata;
} pmix_usock_sr_t;
OBJ_CLASS_DECLARATION(pmix_usock_sr_t);

/* usock structure for tracking posted recvs */
typedef struct {
    pmix_list_item_t super;
    event_t ev;
    uint32_t tag;
    pmix_usock_cbfunc_t cbfunc;
    void *cbdata;
} pmix_usock_posted_recv_t;
OBJ_CLASS_DECLARATION(pmix_usock_posted_recv_t);


/* usock struct for tracking ops */
typedef struct {
    pmix_object_t super;
    event_t ev;
    volatile bool active;
    pmix_buffer_t data;
    pmix_cbfunc_t cbfunc;
    void *cbdata;
} pmix_cb_t;
OBJ_CLASS_DECLARATION(pmix_cb_t);


typedef struct {
    pmix_buffer_t *cache_local;
    pmix_buffer_t *cache_remote;
    pmix_buffer_t *cache_global;
    event_base_t *evbase;
    pmix_identifier_t id;
    pmix_identifier_t server;
    char *uri;
    struct sockaddr_un address;
    int sd;
    int max_retries;
    int retries;                  // number of times we have tried to connect to this address
    pmix_usock_state_t state;
    event_t op_event;             // used for connecting and operations other than read/write
    uint32_t tag;                 // current tag
    event_t send_event;           // registration with event thread for send events
    bool send_ev_active;
    event_t recv_event;           // registration with event thread for recv events
    bool recv_ev_active;
    event_t timer_event;          // timer for retrying connection failures
    bool timer_ev_active;
    pmix_list_t send_queue;       // list of pmix_usock_sent_t to be sent
    pmix_usock_send_t *send_msg;  // current send in progress
    pmix_usock_recv_t *recv_msg;  // current recv in progress
    pmix_list_t posted_recvs;     // list of pmix_usock_posted_recv_t
} pmix_client_globals_t;

extern pmix_client_globals_t pmix_client_globals;


/* module-level shared functions */
extern void pmix_usock_process_msg(int fd, short flags, void *cbdata);
extern void pmix_usock_send_recv(int fd, short args, void *cbdata);
extern void pmix_usock_send_handler(int sd, short flags, void *cbdata);
extern void pmix_usock_recv_handler(int sd, short flags, void *cbdata);
extern char* pmix_usock_state_print(pmix_usock_state_t state);
extern void pmix_usock_dump(const char* msg);
extern int usock_send_connect_ack(void);
extern void pmix_client_call_errhandler(int error);


/* internal convenience macros */
#define PMIX_ACTIVATE_SEND_RECV(b, cb, d)                               \
    do {                                                                \
        pmix_usock_sr_t *ms;                                            \
        pmix_output_verbose(5, pmix_debug_output,                       \
                            "[%s:%d] post send to server",              \
                            __FILE__, __LINE__);                        \
        ms = OBJ_NEW(pmix_usock_sr_t);                                  \
        ms->bfr = (b);                                                  \
        ms->cbfunc = (cb);                                              \
        ms->cbdata = (d);                                               \
        event_assign(&((ms)->ev), pmix_client_evbase, -1,               \
                  EV_WRITE, pmix_usock_send_recv, (ms));                \
        event_active(&((ms)->ev), EV_WRITE, 1);                         \
    } while(0);

#define PMIX_ACTIVATE_POST_MSG(ms)                                      \
    do {                                                                \
        pmix_output_verbose(5, pmix_debug_output,                       \
                            "[%s:%d] post msg",                         \
                            __FILE__, __LINE__);                        \
        event_assign( &ms->ev, pmix_client_evbase,-1,                   \
                  EV_WRITE, pmix_usock_process_msg, ms);                \
        event_active(&ms->ev, EV_WRITE, 1);                             \
    } while(0);

#define CLOSE_THE_SOCKET(socket)                                \
    do {                                                        \
        shutdown(socket, 2);                                    \
        close(socket);                                          \
        /* notify the error handler */                          \
        pmix_client_call_errhandler(PMIX_ERR_COMM_FAILURE);     \
    } while(0)


#define PMIX_WAIT_FOR_COMPLETION(a)             \
    do {                                        \
        while ((a)) {                           \
            usleep(10);                         \
        }                                       \
    } while (0);

END_C_DECLS

#endif /* MCA_PMIX_CLIENT_H */
