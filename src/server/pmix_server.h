/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_CLIENT_H
#define PMIX_CLIENT_H

#include "pmix_config.h"
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
#include "src/api/pmix.h"
#include "src/usock/usock.h"

BEGIN_C_DECLS

extern int pmix_client_debug_output;
extern uint64_t pmix_client_myid;


/* usock struct for tracking ops */
typedef struct {
    pmix_object_t super;
    event_t ev;
    volatile bool active;
    pmix_buffer_t data;
    pmix_cbfunc_t cbfunc;
    void *cbdata;
    char *namespace;
    int rank;
    char *key;
} pmix_cb_t;
OBJ_CLASS_DECLARATION(pmix_cb_t);

typedef struct {
    char namespace[PMIX_MAX_VALLEN];
    pmix_buffer_t *cache_local;
    pmix_buffer_t *cache_remote;
    pmix_buffer_t *cache_global;
    event_base_t *evbase;
    uint64_t id;
    uint64_t server;
    char *uri;
    struct sockaddr_un address;
    int sd;
    int max_retries;
    //int retries;                  // number of times we have tried to connect to this address
    //pmix_usock_state_t state;     // use sd as indicator of 2 possible states: connected/unconnected
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
    int debug_level;              // debug level
    int debug_output;             // debug output channel
} pmix_client_globals_t;

extern pmix_client_globals_t pmix_client_globals;


/* module-level shared functions */
extern void pmix_client_call_errhandler(int error);

#define PMIXAIT_FOR_COMPLETION(a)               \
    do {                                        \
        while ((a)) {                           \
            usleep(10);                         \
        }                                       \
    } while (0);

END_C_DECLS

#endif /* MCA_PMIX_CLIENT_H */
