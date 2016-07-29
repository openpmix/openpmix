/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2016 Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GLOBALS_H
#define PMIX_GLOBALS_H

#include <src/include/pmix_config.h>

#include <src/include/types.h>

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include PMIX_EVENT_HEADER

#include <pmix_common.h>

#include "src/mca/bfrops/bfrops.h"
#include "src/mca/psec/psec.h"
#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/event/pmix_event.h"

BEGIN_C_DECLS

/* some limits */
#define PMIX_MAX_CRED_SIZE      131072              // set max at 128kbytes
#define PMIX_MAX_ERR_CONSTANT   INT_MIN


/****   ENUM DEFINITIONS    ****/
/* define a command type for communicating to the
 * pmix server */
#define PMIX_CMD PMIX_UINT32

/* define some commands */
typedef uint8_t pmix_cmd_t;
#define PMIX_REQ_CMD             0
#define PMIX_ABORT_CMD           1
#define PMIX_COMMIT_CMD          2
#define PMIX_FENCENB_CMD         3
#define PMIX_GETNB_CMD           4
#define PMIX_FINALIZE_CMD        5
#define PMIX_PUBLISHNB_CMD       6
#define PMIX_LOOKUPNB_CMD        7
#define PMIX_UNPUBLISHNB_CMD     8
#define PMIX_SPAWNNB_CMD         9
#define PMIX_CONNECTNB_CMD      10
#define PMIX_DISCONNECTNB_CMD   11
#define PMIX_NOTIFY_CMD         12
#define PMIX_REGEVENTS_CMD      13
#define PMIX_DEREGEVENTS_CMD    14
#define PMIX_QUERY_CMD          15

/* define a set of flags to direct collection
 * of data during operations */
typedef uint8_t pmix_collect_t;
#define PMIX_COLLECT_INVALID    0
#define PMIX_COLLECT_NO         1
#define PMIX_COLLECT_YES        2
#define PMIX_COLLECT_MAX        3


/****    MESSAGING STRUCTURES    ****/
/* header for messages */
typedef struct {
    int pindex;
    uint32_t tag;
    size_t nbytes;
} pmix_usock_hdr_t;

// forward declaration
struct pmix_peer_t;

/* internally used cbfunc */
typedef void (*pmix_usock_cbfunc_t)(struct pmix_peer_t *peer, pmix_usock_hdr_t *hdr,
                                    pmix_buffer_t *buf, void *cbdata);

/* usock structure for sending a message */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    pmix_usock_hdr_t hdr;
    pmix_buffer_t *data;
    bool hdr_sent;
    char *sdptr;
    size_t sdbytes;
} pmix_usock_send_t;
PMIX_CLASS_DECLARATION(pmix_usock_send_t);

/* usock structure for recving a message */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    struct pmix_peer_t *peer;
    int sd;
    pmix_usock_hdr_t hdr;
    char *data;
    bool hdr_recvd;
    char *rdptr;
    size_t rdbytes;
} pmix_usock_recv_t;
PMIX_CLASS_DECLARATION(pmix_usock_recv_t);


/****    PEER STRUCTURES    ****/
/* objects for tracking active nspaces */
typedef struct {
    pmix_object_t super;
    size_t nlocalprocs;
    bool all_registered;         // all local ranks have been defined
    pmix_info_t *info;           // copy of the job-level info to be delivered to each proc
    size_t ninfo;
    pmix_buffer_t *job_info;     // packed version of the job-level info for passing to procs of this nspace
    pmix_list_t ranks;           // list of pmix_rank_info_t for connection support of my clients
    pmix_hash_table_t mylocal;   // hash_table for storing data PUT with local/global scope by my clients
    pmix_hash_table_t myremote;  // hash_table for storing data PUT with remote/global scope by my clients
    pmix_hash_table_t remote;    // hash_table for storing data PUT with remote/global scope recvd from remote clients via modex
} pmix_server_nspace_t;
PMIX_CLASS_DECLARATION(pmix_server_nspace_t);

typedef struct {
    pmix_list_item_t super;
    char nspace[PMIX_MAX_NSLEN+1];
    pmix_list_t nodes;               // list of pmix_nrec_t nodes that house procs in this nspace
    pmix_hash_table_t internal;      // hash_table for storing job-level/internal data related to this nspace
    pmix_hash_table_t modex;         // hash_table of received modex data
    pmix_server_nspace_t *server;    // isolate these so the client doesn't instantiate them
} pmix_nspace_t;
PMIX_CLASS_DECLARATION(pmix_nspace_t);

typedef struct pmix_rank_info_t {
    pmix_list_item_t super;
    struct pmix_peer_t *peer;
    pmix_nspace_t *nptr;
    int rank;
    uid_t uid;
    gid_t gid;
    bool modex_recvd;
    int proc_cnt;              // #clones of this rank we know about
    void *server_object;       // pointer to rank-specific object provided by server
} pmix_rank_info_t;
PMIX_CLASS_DECLARATION(pmix_rank_info_t);

/* object for tracking the communication-related characteristics
 * of a given peer */
typedef struct pmix_peer_comm_t {
    pmix_bfrop_buffer_type_t type;
    pmix_bfrops_module_t *bfrops;
    pmix_psec_module_t *sec;
} pmix_peer_comm_t;

/* object for tracking peers - each peer can have multiple
 * connections. This can occur if the initial app executes
 * a fork/exec, and the child initiates its own connection
 * back to the PMIx server. Thus, the trackers should be "indexed"
 * by the socket, not the process nspace/rank */
typedef struct pmix_peer_t {
    pmix_object_t super;
    pmix_rank_info_t *info;
    int proc_cnt;
    void *server_object;
    int index;
    int sd;
    pmix_event_t send_event;    /**< registration with event thread for send events */
    bool send_ev_active;
    pmix_event_t recv_event;    /**< registration with event thread for recv events */
    bool recv_ev_active;
    pmix_list_t send_queue;      /**< list of messages to send */
    pmix_usock_send_t *send_msg; /**< current send in progress */
    pmix_usock_recv_t *recv_msg; /**< current recv in progress */
    pmix_peer_comm_t comm;
} pmix_peer_t;
PMIX_CLASS_DECLARATION(pmix_peer_t);


typedef struct {
    pmix_list_item_t super;
    char *name;              // name of the node
    char *procs;             // comma-separated list of proc ranks on that node
} pmix_nrec_t;
PMIX_CLASS_DECLARATION(pmix_nrec_t);

/* define an object for moving a send
 * request into the server's event base */
typedef struct {
    pmix_object_t super;
    int sd;
} pmix_snd_caddy_t;
PMIX_CLASS_DECLARATION(pmix_snd_caddy_t);

/* define an object for moving a send
 * request into the server's event base */
typedef struct {
    pmix_list_item_t super;
    pmix_usock_hdr_t hdr;
    pmix_peer_t *peer;
    pmix_snd_caddy_t snd;
} pmix_server_caddy_t;
PMIX_CLASS_DECLARATION(pmix_server_caddy_t);

/* caddy for query requests */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    volatile bool active;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *directives;
    size_t ndirs;
    pmix_info_cbfunc_t cbfunc;
    pmix_release_cbfunc_t relcbfunc;
    void *cbdata;
} pmix_query_caddy_t;
PMIX_CLASS_DECLARATION(pmix_query_caddy_t);

/* define a tracker for collective operations */
typedef struct {
    pmix_list_item_t super;
    pmix_cmd_t type;
    bool hybrid;                    // true if participating procs are from more than one nspace
    pmix_proc_t *pcs;               // copy of the original array of participants
    size_t   npcs;                  // number of procs in the array
    volatile bool active;           // flag for waiting for completion
    bool def_complete;              // all local procs have been registered and the trk definition is complete
    pmix_list_t ranks;              // list of pmix_rank_info_t of the local participants
    pmix_list_t local_cbs;          // list of pmix_server_caddy_t for sending result to the local participants
    uint32_t nlocal;                // number of local participants
    uint32_t local_cnt;             // number of local participants who have contributed
    pmix_info_t *info;              // array of info structs
    size_t ninfo;                   // number of info structs in array
    pmix_collect_t collect_type;    // whether or not data is to be returned at completion
    pmix_modex_cbfunc_t modexcbfunc;
    pmix_op_cbfunc_t op_cbfunc;
} pmix_server_trkr_t;
PMIX_CLASS_DECLARATION(pmix_server_trkr_t);


/****    THREAD-RELATED    ****/
 /* define a caddy for thread-shifting operations */
 typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    volatile bool active;
    pmix_status_t status;
    pmix_status_t *codes;
    size_t ncodes;
    const char *nspace;
    int rank;
    const char *data;
    size_t ndata;
    const char *key;
    pmix_info_t *info;
    size_t ninfo;
    pmix_notification_fn_t evhdlr;
    pmix_kval_t *kv;
    pmix_value_t *vptr;
    pmix_server_caddy_t *cd;
    pmix_server_trkr_t *tracker;
    bool enviro;
    union {
       pmix_release_cbfunc_t relfn;
       pmix_evhdlr_reg_cbfunc_t evregcbfn;
       pmix_op_cbfunc_t opcbfn;
       pmix_evhdlr_reg_cbfunc_t errregcbfn;
    }cbfunc;
    void *cbdata;
    size_t ref;
 } pmix_shift_caddy_t;
PMIX_CLASS_DECLARATION(pmix_shift_caddy_t);

/* define a very simple caddy for dealing with pmix_info_t
 * objects when transferring portions of arrays */
typedef struct {
    pmix_list_item_t super;
    pmix_info_t *info;
} pmix_info_caddy_t;
PMIX_CLASS_DECLARATION(pmix_info_caddy_t);

#define PMIX_THREADSHIFT(r, c)                       \
 do {                                                 \
    (r)->active = true;                               \
    event_assign(&((r)->ev), pmix_globals.evbase,     \
                 -1, EV_WRITE, (c), (r));             \
    event_active(&((r)->ev), EV_WRITE, 1);            \
} while (0)


#define PMIX_WAIT_FOR_COMPLETION(a)             \
    do {                                        \
        while ((a)) {                           \
            usleep(10);                         \
        }                                       \
    } while (0)

/* define a process type */
typedef enum {
    PMIX_PROC_UNDEF,
    PMIX_PROC_CLIENT,
    PMIX_PROC_SERVER,
    PMIX_PROC_TOOL
} pmix_proc_type_t;

/****    GLOBAL STORAGE    ****/
/* define a global construct that includes values that must be shared
 * between various parts of the code library. Both the client
 * and server libraries must instance this structure */
typedef struct {
    int init_cntr;                       // #times someone called Init - #times called Finalize
    pmix_proc_t myid;
    pmix_peer_t *mypeer;                 // my own peer object
    uid_t uid;                           // my effective uid
    gid_t gid;                           // my effective gid
    int pindex;
    pmix_event_base_t *evbase;
    bool external_evbase;
    int debug_output;
    pmix_events_t events;                // my event handler registrations.
    pmix_proc_type_t proc_type;          // what type of process am I (client, server, tool)
    bool connected;
    pmix_list_t nspaces;                 // list of pmix_nspace_t for the nspaces we know about
    pmix_buffer_t *cache_local;          // data PUT by me to local scope
    pmix_buffer_t *cache_remote;         // data PUT by me to remote scope
} pmix_globals_t;


extern pmix_globals_t pmix_globals;

END_C_DECLS

#endif /* PMIX_GLOBALS_H */
