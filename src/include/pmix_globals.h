/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GLOBALS_H
#define PMIX_GLOBALS_H

#include "src/include/pmix_config.h"
#include "src/include/pmix_types.h"

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <event.h>

#include "pmix.h"
#include "pmix_common.h"
#include "pmix_tool.h"

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/event/pmix_event.h"
#include "src/runtime/pmix_init_util.h"
#include "src/threads/pmix_threads.h"

#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/psec/psec.h"
#include "src/mca/ptl/ptl.h"

#include "src/util/pmix_name_fns.h"

BEGIN_C_DECLS

/* some limits */
#define PMIX_MAX_CRED_SIZE     131072 // set max at 128kbytes
#define PMIX_MAX_ERR_CONSTANT  INT_MIN
#define PMIX_TAINT_INT_LIMIT   INT_MAX-2   // arbitrary limit to silence Coverity taint complaints
#define PMIX_TAINT_UINT_LIMIT  UINT_MAX-2  // arbitrary limit to silence Coverity taint complaints
#define PMIX_TAINT_SIZE_LIMIT  SIZE_MAX-2  // arbitrary limit to silence Coverity taint complaints


/* internal-only attributes */
#define PMIX_BFROPS_MODULE \
    "pmix.bfrops.mod" // (char*) name of bfrops plugin in-use by a given nspace
#define PMIX_PNET_SETUP_APP \
    "pmix.pnet.setapp" // (pmix_byte_object_t) blob containing info to be given to
                       //      pnet framework on remote nodes

// define some bit handling macros
#define PMIX_SET_BIT(a, f) \
    (a) |= (f)

#define PMIX_UNSET_BIT(a, f) \
    (a) &= ~(f)

#define PMIX_CHECK_BIT_IS_SET(a, f) \
    ((a) & (f))

#define PMIX_CHECK_BIT_NOT_SET(a, f) \
    !PMIX_CHECK_BIT_IS_SET(a, f)

#define PMIX_INFO_OP_COMPLETE       0x80000000
#define PMIX_INFO_OP_COMPLETED(m) \
      PMIX_SET_BIT((m)->flags, PMIX_INFO_OP_COMPLETE)
#define PMIX_INFO_OP_IS_COMPLETE(m) \
      PMIX_CHECK_BIT_IS_SET((m)->flags, PMIX_INFO_OP_COMPLETE)


/* define an internal-only object for creating
 * lists of names */
typedef struct {
    pmix_list_item_t super;
    pmix_name_t *pname;
} pmix_namelist_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_namelist_t);

/* define structs for holding entries in the
 * show-help matrix */
typedef struct {
    const char *topic;
    const char **content;
} pmix_show_help_entry_t;

typedef struct {
    const char *filename;
    pmix_show_help_entry_t *entries;
} pmix_show_help_file_t;

PMIX_EXPORT extern pmix_show_help_file_t pmix_show_help_data[];

/* define a struct for holding entries in the
 * dictionary of attributes */
typedef struct {
    uint32_t index;
    char *name;
    char *string;
    pmix_data_type_t type;
    char **description;
} pmix_regattr_input_t;
#define PMIX_REGATTR_INPUT_NEW(a, i, n, s, t, d)                            \
do {                                                                        \
    (a) = (pmix_regattr_input_t*)pmix_malloc(sizeof(pmix_regattr_input_t)); \
    if (NULL != (a)) {                                                      \
        memset((a), 0, sizeof(pmix_regattr_input_t));                       \
        (a)->index = (i);                                                   \
        if (NULL != (n)) {                                                    \
            (a)->name = strdup((n));                                       \
        }                                                                   \
        if (NULL != (s)) {                                                    \
            (a)->string = strdup((s));                                        \
        }                                                                   \
        (a)->type = (t);                                                    \
        if (NULL != (d)) {                                                    \
            (a)->description = PMIx_Argv_copy((d));                           \
        }                                                                   \
    }                                                                       \
} while(0)

/* define a struct for holding entries in the
 * dictionary of event strings */
typedef struct {
    uint32_t index;
    char *name;
    int32_t code;
} pmix_event_string_t;

/* define a struct for storing data in memory */
typedef struct {
    uint32_t index;
    pmix_value_t *value;
} pmix_qual_t;
#define PMIX_QUAL_NEW(d, k)                                 \
do {                                                        \
    (d) = (pmix_qual_t*)pmix_malloc(sizeof(pmix_qual_t));   \
    if (NULL != (d)) {                                      \
        (d)->index = k;                                     \
        (d)->value = NULL;                                  \
    }                                                       \
} while(0)
#define PMIX_QUAL_RELEASE(d)            \
do {                                    \
    if (NULL != (d)->value) {           \
        PMIX_VALUE_RELEASE((d)->value); \
    }                                   \
} while(0)

typedef struct {
    uint32_t index;
    uint32_t qualindex;
    pmix_value_t *value;
} pmix_dstor_t;

PMIX_EXPORT pmix_dstor_t *
pmix_dstor_new_tma(
    uint32_t index,
    pmix_tma_t *tma
);

PMIX_EXPORT void
pmix_dstor_release_tma(
    pmix_dstor_t *d,
    pmix_tma_t *tma
);

#define PMIX_DSTOR_NEW(d, k)                                \
do {                                                        \
    (d) = pmix_dstor_new_tma((k), NULL);                    \
} while(0)

#define PMIX_DSTOR_RELEASE(d)           \
do {                                    \
    pmix_dstor_release_tma((d), NULL);  \
} while(0)

/* define a struct for passing topology objects */
typedef struct {
    pmix_object_t super;
    char *source;
    void *object;
} pmix_topo_obj_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_topo_obj_t);

/* define a command type for communicating to the
 * pmix server */
typedef uint8_t pmix_cmd_t;

/* define some commands */
#define PMIX_REQ_CMD                      0
#define PMIX_ABORT_CMD                    1
#define PMIX_COMMIT_CMD                   2
#define PMIX_FENCENB_CMD                  3
#define PMIX_GETNB_CMD                    4
#define PMIX_FINALIZE_CMD                 5
#define PMIX_PUBLISHNB_CMD                6
#define PMIX_LOOKUPNB_CMD                 7
#define PMIX_UNPUBLISHNB_CMD              8
#define PMIX_SPAWNNB_CMD                  9
#define PMIX_CONNECTNB_CMD                10
#define PMIX_DISCONNECTNB_CMD             11
#define PMIX_NOTIFY_CMD                   12
#define PMIX_REGEVENTS_CMD                13
#define PMIX_DEREGEVENTS_CMD              14
#define PMIX_QUERY_CMD                    15
#define PMIX_LOG_CMD                      16
#define PMIX_ALLOC_CMD                    17
#define PMIX_JOB_CONTROL_CMD              18
#define PMIX_MONITOR_CMD                  19
#define PMIX_GET_CREDENTIAL_CMD           20
#define PMIX_VALIDATE_CRED_CMD            21
#define PMIX_IOF_PULL_CMD                 22
#define PMIX_IOF_PUSH_CMD                 23
#define PMIX_GROUP_CONSTRUCT_CMD          24
#define PMIX_GROUP_JOIN_CMD               25
#define PMIX_GROUP_INVITE_CMD             26
#define PMIX_GROUP_LEAVE_CMD              27
#define PMIX_GROUP_DESTRUCT_CMD           28
#define PMIX_IOF_DEREG_CMD                29
#define PMIX_FABRIC_REGISTER_CMD          30
#define PMIX_FABRIC_UPDATE_CMD            31
#define PMIX_COMPUTE_DEVICE_DISTANCES_CMD 32
#define PMIX_REFRESH_CACHE                33
#define PMIX_RESBLK_CMD                   34
#define PMIX_SESSION_CTRL_CMD             35
#define PMIX_REQ_SYSINFO_CMD              36
#define PMIX_RESOLVE_PEERS_CMD            37
#define PMIX_RESOLVE_NODE_CMD             38
// Client tells the server it has switched to a different GDS module
// (carries the module name) and re-requests its job data in that format.
// Appended at the end for wire compatibility; an older server that does
// not recognize it falls through to PMIX_ERR_NOT_SUPPORTED.
#define PMIX_GDS_FALLBACK_CMD             39


/* provide a "pretty-print" function for cmds */
const char *pmix_command_string(pmix_cmd_t cmd);

/* provide a hook to init tool data */
PMIX_EXPORT extern pmix_status_t pmix_tool_init_info(void);

/* define a set of flags to direct collection
 * of data during operations.
 *
 * The first three are internal: pmix_server_trkr_t.collect_type holds one
 * of them, and several places test it against PMIX_COLLECT_YES to decide
 * whether to collect at all.
 *
 * PMIX_MODEX_DELTA is different in kind. It is never assigned to
 * collect_type - it exists solely as a value of the per-server flag byte
 * carried in the modex envelope (see pmix_gds_base_store_modex), where it
 * says "this contribution holds only what changed since the contributing
 * process last took part in a collecting fence" rather than that process's
 * whole published set. Nothing emits it yet; see openpmix#4087 and
 * docs/how-things-work/modex.rst.
 *
 * Because it travels on the wire it is append-only, exactly like the
 * command enum above: give a new marker the next free value and never
 * renumber an existing one. */
typedef enum {
    PMIX_COLLECT_INVALID = -1,
    PMIX_COLLECT_NO,
    PMIX_COLLECT_YES,
    PMIX_MODEX_DELTA,
    PMIX_COLLECT_MAX
} pmix_collect_t;

/****    PEER STRUCTURES    ****/

/* clients can only talk to their server, and servers are
 * assumed to all have the same personality. Thus, each
 * process only needs to track a single set of personality
 * modules. All interactions between a client and its local
 * server, or between servers, are done thru these modules */
typedef struct pmix_personality_t {
    pmix_bfrop_buffer_type_t type;
    pmix_bfrops_module_t *bfrops;
    pmix_psec_module_t *psec;
    pmix_gds_base_module_t *gds;
} pmix_personality_t;

/* define a set of structs for tracking post-termination cleanup */
typedef struct pmix_epilog_t {
    uid_t uid;
    gid_t gid;
    pmix_list_t cleanup_dirs;
    pmix_list_t cleanup_files;
    pmix_list_t ignores;
} pmix_epilog_t;

typedef struct {
    pmix_list_item_t super;
    char *path;
} pmix_cleanup_file_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_cleanup_file_t);

typedef struct {
    pmix_list_item_t super;
    char *path;
    /* recurse and empty are mutually exclusive - PMIX_CLEANUP_RECURSIVE
     * removes every file and then the directories that held them, while
     * PMIX_CLEANUP_EMPTY removes only the directories that are empty and
     * leaves every file alone. pmix_server_job_ctrl refuses a request
     * asking for both */
    bool recurse;
    bool empty;
    bool leave_topdir;
} pmix_cleanup_dir_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_cleanup_dir_t);

/* define a struct to hold booleans controlling the
 * format/contents of the output */
typedef struct {
    bool set;
    bool xml;
    bool timestamp;
    bool tag;
    bool tag_detailed;
    bool tag_fullname;
    bool rank;
    char *file;
    char *directory;
    bool nocopy;
    bool merge;
    bool local_output;
    bool local_output_given;
    bool pattern;
    bool raw;
} pmix_iof_flags_t;

#define PMIX_IOF_FLAGS_STATIC_INIT  \
{                                   \
    .set = false,                   \
    .xml = false,                   \
    .timestamp = false,             \
    .tag = false,                   \
    .tag_detailed = false,          \
    .tag_fullname = false,          \
    .rank = false,                  \
    .file = NULL,                   \
    .directory = NULL,              \
    .nocopy = true,                 \
    .merge = false,                 \
    .local_output = false,          \
    .local_output_given = false,    \
    .pattern = false,               \
    .raw = false                    \
}

/* objects used by servers for tracking active nspaces */
typedef struct {
    pmix_list_item_t super;
    char *nspace;
    struct {
        uint8_t major;
        uint8_t minor;
        uint8_t release;
    } version;
    pmix_rank_t nprocs; // num procs in this nspace
    size_t nlocalprocs;
    bool all_registered;   // all local ranks have been defined
    bool version_stored;   // the version string used by this nspace has been stored
    bool job_info_recvd;   // job-level info has been received
    pmix_buffer_t *jobbkt; // packed version of jobinfo
    size_t ndelivered;     // count of #local clients that have received the jobinfo
    size_t nfinalized;     // count of #local clients that have finalized
    bool local_app_fini_fired; // the "all local processes finalized" callback has been
                               // fired for this nspace; guards against re-firing it once
                               // per rank while the nspace is torn down
    pmix_list_t ranks;     // list of pmix_rank_info_t for connection support of my clients
    /* Ranks whose entry above the host has taken back - the proc has
     * terminated. pmix_proclist_t, one per reaped LOCAL rank, kept until
     * this namespace is deregistered.
     *
     * Without it a direct-modex request naming one of them is
     * indistinguishable from one that arrived before the rank was
     * registered, and _dmodex_req() treats "I do not know that rank" as
     * "not yet" and parks the request forever. The asking process then
     * sits in PMIx_Get until something else kills the job. Reading a
     * peer that has exited is an application error, but it has to
     * SURFACE as one - a hang tells the developer nothing. */
    pmix_list_t departed;
    /* all members of an nspace are required to have the
     * same personality, but it can differ between nspaces.
     * Since servers may support clients from multiple nspaces,
     * track their respective compatibility modules here */
    pmix_personality_t compat;
    pmix_epilog_t epilog;   // things to do upon termination of all local clients
                            // from this nspace
    pmix_list_t setup_data; // list of pmix_kval_t containing info structs having blobs
                            // for setting up the local node for this nspace/application
    pmix_iof_flags_t iof_flags;   // output formatting flags
    pmix_list_t sinks;   // IOF write events for output to files or directories
} pmix_namespace_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_namespace_t);

/* define a caddy for quickly creating a list of pmix_namespace_t
 * objects for local, dedicated purposes */
typedef struct {
    pmix_list_item_t super;
    pmix_namespace_t *ns;
} pmix_nspace_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_nspace_caddy_t);

typedef struct {
    pmix_list_item_t super;
    pmix_namespace_t *ns;
    pmix_list_t envars;
} pmix_nspace_env_cache_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_nspace_env_cache_t);

typedef struct {
    pmix_list_item_t super;
    pmix_envar_t envar;
} pmix_envar_list_item_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_envar_list_item_t);


typedef struct pmix_rank_info_t {
    pmix_list_item_t super;
    int peerid; // peer object index into the local clients array on the server
    pmix_name_t pname;
    pid_t pid;
    uid_t realuid;
    uid_t uid;
    gid_t realgid;
    gid_t gid;
    bool modex_recvd;
    int proc_cnt;        // #clones of this rank we know about
    void *server_object; // pointer to rank-specific object provided by server
    /* Delta-modex bookkeeping, server side - see pmix_server_collect_data.
     *
     * pending_modex holds the PMIX_REMOTE-scope kvals this rank has
     * committed since it last contributed to a collecting fence, so that
     * contribution can carry what changed instead of everything the rank
     * has ever published. It lives here rather than on the peer because a
     * fork/exec'd clone shares its parent's rank_info, which is the same
     * identity the collection dedups on.
     *
     * modex_sig digests the participant set of the fence this rank last
     * contributed to, and modex_contributed says whether it means
     * anything yet. A delta is only sound for a fence over the same set:
     * contributing one to a fence over some *other* set would leave every
     * server holding only that set's procs never learning these keys. */
    pmix_list_t pending_modex;
    uint64_t modex_sig;
    bool modex_contributed;
    /* Keys this rank has deleted that its peers on other nodes have not
     * been told about yet.
     *
     * Removing the key from our store is not enough to reach them: the
     * modex is additive, so a later contribution simply not carrying the
     * key removes nothing at the far end. The deletion has to be *said*,
     * as an entry whose value is PMIX_UNDEF, which the receiving
     * datastore reads as "this key is gone". Announced once, with the
     * next contribution, and then dropped. */
    pmix_list_t pending_deletes;
} pmix_rank_info_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_rank_info_t);

/* define a very simple caddy for dealing with pmix_info_t
 * and pmix_query_t objects when transferring portions of arrays */
typedef struct {
    pmix_list_item_t super;
    pmix_info_t *info;
    size_t ninfo;
} pmix_info_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_info_caddy_t);

typedef struct {
    pmix_list_item_t super;
    pmix_info_t info;
} pmix_infolist_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_infolist_t);

typedef struct {
    pmix_list_item_t super;
    pmix_query_t query;
} pmix_querylist_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_querylist_t);

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t proc;
} pmix_proclist_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_proclist_t);

/* object for tracking peers - each peer can have multiple
 * connections. This can occur if the initial app executes
 * a fork/exec, and the child initiates its own connection
 * back to the PMIx server. Thus, the trackers should be "indexed"
 * by the socket, not the process nspace/rank */
typedef struct pmix_peer_t {
    pmix_object_t super;
    pmix_namespace_t *nptr; // point to the nspace object for this process
    pmix_gds_base_module_t *gds; // per-peer GDS module override; when NULL, the
                                 // nspace-level nptr->compat.gds is used. Lets a
                                 // single client fall back to another module
                                 // (e.g. hash) without affecting its peers.
    pmix_rank_info_t *info;
    pmix_proc_type_t proc_type;
    pmix_listener_protocol_t protocol;
    int proc_cnt;
    int index; // index into the local clients array on the server
    int sd;
    bool finalized;          // peer has called finalize
    bool stdin_producer;     // peer has pushed stdin to us, and is therefore
                             // somebody IOF flow control has to reach when the
                             // host tells us to stop taking stdin
    pmix_event_t send_event; /**< registration with event thread for send events */
    bool send_ev_active;
    pmix_event_t recv_event; /**< registration with event thread for recv events */
    bool recv_ev_active;
    pmix_list_t send_queue;    /**< list of messages to send */
    pmix_ptl_send_t *send_msg; /**< current send in progress */
    pmix_ptl_recv_t *recv_msg; /**< current recv in progress */
    int commit_cnt;
    pmix_epilog_t epilog; /**< things to be performed upon
                               termination of this peer */
    uint32_t dyn_tags_start; // lower limit of valid tags for sendrecvs to this peer
    uint32_t dyn_tags_current; // current tag for sendrecvs to this peer
    uint32_t dyn_tags_end; // upper limit of valid tags for sendrecvs to this peer
} pmix_peer_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_peer_t);

typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *peer;
} pmix_peerlist_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_peerlist_t);

typedef struct {
    pmix_object_t super;
    struct timeval tv;
    pmix_event_t ev;
    bool active;
    void *payload;
} pmix_timer_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_timer_t);

/* tracker for IOF requests */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    pmix_peer_t *requestor;
    size_t local_id;
    size_t remote_id;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_iof_flags_t flags;
    pmix_iof_channel_t channels;
    pmix_info_t *directives;
    size_t ndirs;
    pmix_iof_cbfunc_t cbfunc;
    pmix_hdlr_reg_cbfunc_t regcbfunc;
    void *cbdata;
} pmix_iof_req_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_req_t);

/* caddy for query requests */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    bool host_called;
    pmix_status_t status;
    pmix_query_t *queries;
    size_t nqueries;
    pmix_proc_t *targets;
    size_t ntargets;
    pmix_info_t *info;
    pmix_info_t *dirs;
    size_t ninfo;
    size_t ndirs;
    pmix_list_t results;
    size_t nreplies;
    size_t nrequests;
    pmix_byte_object_t bo;
    pmix_info_cbfunc_t cbfunc;
    pmix_value_cbfunc_t valcbfunc;
    pmix_release_cbfunc_t relcbfunc;
    pmix_credential_cbfunc_t credcbfunc;
    pmix_validation_cbfunc_t validcbfunc;
    void *cbdata;
} pmix_query_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_query_caddy_t);

typedef struct {
    pmix_list_item_t super;
    char *grpid;
    size_t ctxid;
    pmix_proc_t *members;
    size_t nmbrs;
    /* the group's failure policy is chosen at construct time but governs the
     * later destruct; the server holds no group state between the two
     * collectives, so we remember here whether PMIX_GROUP_NOTIFY_TERMINATION was
     * requested and re-attach it to the destruct request. */
    bool notterm;
} pmix_group_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_group_t);

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t proc;
    pmix_byte_object_t blob;  // packed blob of info provided by this proc
} pmix_grpinfo_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_grpinfo_t);

/* define a tracker for collective operations
 * - instanced in pmix_server_ops.c */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    bool event_active;
    bool host_called; // tracker has been passed up to host
    /* This tracker's completion function has been called, so the tracker is
     * spoken for: the completion thread-shifts, and when it runs it replies
     * to every participant on local_cbs, unlinks the tracker from
     * pmix_server_globals.collectives and releases it.
     *
     * Until that handler runs the tracker is still ON the collectives list
     * and still answers pmix_server_trk_complete() - local_cbs is drained
     * by the handler, nothing earlier - so without this flag anything that
     * walks the list in that window sees a tracker that looks complete and
     * unclaimed, and drives its completion a second time. Two completions
     * means two handlers for one tracker: the second reads and re-releases
     * memory the first freed, and unlinks through freed list links. The
     * host path is covered by host_called, but a strictly local collective
     * never sets that (and two error paths deliberately clear it), which is
     * exactly the common case. See the collectives directory guide. */
    bool completion_fired;
    bool local;       // operation is strictly local
    char *id;         // string identifier for the collective
    pmix_cmd_t type;
    pmix_proc_t pname;
    bool hybrid;            // true if participating procs are from more than one nspace
    pmix_proc_t *pcs;       // copy of the original array of participants
    size_t npcs;            // number of procs in the array
    pmix_list_t nslist;     // unique nspace list of participants
    pmix_lock_t lock;       // flag for waiting for completion
    bool def_complete;      // all local procs have been registered and the trk definition is complete
    pmix_list_t local_cbs;  // list of pmix_server_caddy_t for sending result to the local participants
                            //    Note: there may be multiple entries for a given proc if that proc
                            //    has fork/exec'd clones that are also participating
    pmix_list_t departed;   // local participants lost (connection dropped) BEFORE contributing.
                            //    Participation is tracked by identity: a peer that already
                            //    contributed (appears on local_cbs) is never moved here, so its
                            //    loss cannot complete the collective early nor discard its data.
                            //    See docs/how-things-work/collectives for the specification.
    uint32_t nlocal;        // number of local participants
    pmix_info_t *info;      // array of info structs
    size_t ninfo;           // number of info structs in array
    pmix_list_t grpinfo;    // list of group info to be distributed
    int grpop;              // the group operation being tracked
    pmix_collect_t collect_type; // whether or not data is to be returned at completion
    pmix_modex_cbfunc_t modexcbfunc;
    pmix_op_cbfunc_t op_cbfunc;
    pmix_info_cbfunc_t info_cbfunc;
    void *cbdata;
} pmix_server_trkr_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_server_trkr_t);

/* define an object for moving a send
 * request into the server's event base and
 * dealing with some request timeouts
 * - instanced in pmix_server_ops.c */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    bool event_active;
    pmix_server_trkr_t *trk;
    pmix_ptl_hdr_t hdr;
    pmix_peer_t *peer;
    pmix_info_t *info;
    size_t ninfo;
    pmix_query_t *query;
    char *key;
} pmix_server_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_server_caddy_t);

/****    THREAD-RELATED    ****/
/* define a caddy for thread-shifting operations */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    pmix_status_t *codes;
    size_t ncodes;
    uint32_t sessionid;
    pmix_name_t pname;
    pmix_proc_t *proc;
    pmix_peer_t *peer;
    const char *data;
    size_t ndata;
    const char *key;
    bool infocopy;
    pmix_info_t *info;
    size_t ninfo;
    bool dircopy;
    pmix_info_t *directives;
    size_t ndirs;
    pmix_pdata_t *pdata;
    size_t npdata;
    pmix_byte_object_t *bo;
    /* a reply packed before the shift, for the callbacks whose data is
     * only theirs to read until they return - released here if it was
     * never queued */
    pmix_buffer_t *buf;
    pmix_device_distance_t *dist;
    size_t ndist;
    pmix_data_range_t range;
    pmix_notification_fn_t evhdlr;
    pmix_iof_req_t *iofreq;
    pmix_kval_t *kv;
    pmix_value_t *vptr;
    pmix_server_caddy_t *cd;
    pmix_server_trkr_t *tracker;
    bool enviro;
    union {
        pmix_release_cbfunc_t relfn;
        pmix_hdlr_reg_cbfunc_t hdlrregcbfn;
        pmix_op_cbfunc_t opcbfn;
        pmix_modex_cbfunc_t modexcbfunc;
        pmix_info_cbfunc_t infocbfunc;
    } cbfunc;
    void *cbdata;
    void *relcbdata;
    size_t ref;
} pmix_shift_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_shift_caddy_t);

typedef struct {
    pmix_object_t super;
    pmix_proc_t p;
    bool pntrval;
    bool stval;
    bool optional;
    /* the caller asked for PMIX_IMMEDIATE. Recorded here, but acted on by
     * the server: the directive rides up in the caller's info array, and
     * the server is the party that can honor it by confining the search to
     * the data it already holds. We never add one the caller did not give */
    bool immediate;
    bool refresh_cache;
    pmix_scope_t scope;
    /* Which realm answers this request. Computed ONCE, by
     * pmix_gds_base_request_realm(), at the end of process_request();
     * the sessioninfo/nodeinfo/appinfo flags below are set FROM it
     * rather than derived alongside it, so there is one answer and the
     * rest of the client reads it. */
    pmix_realm_t realm;
    bool sessioninfo;
    bool sessiondirective;
    uint32_t sessionid;
    bool nodeinfo;
    bool nodedirective;
    char *hostname;
    uint32_t nodeid;
    bool appinfo;
    bool appdirective;
    uint32_t appnum;
    /* Is this request a PLAIN KEYED LOOKUP - one key, out of the asking
     * proc's own data, with nothing to resolve first?
     *
     * Only such a request can be answered on the caller's own thread,
     * which is what try_local_fetch() in src/client/pmix_client_get.c
     * uses this for. Anything that redirects the request to another
     * realm, or asks for more than one value, needs work that belongs on
     * the progress thread - resolving a realm can take further fetches
     * and a rebuild of the info array - so it goes the ordinary way.
     *
     * It describes the REQUEST only. Whether this process may take the
     * short circuit at all - the module's is_tsafe, whether the
     * PMIX_GET_ON_PROGRESS_THREAD development switch is set, whether we
     * are connected - is a separate question that try_local_fetch()
     * asks for itself.
     *
     * Computed at the end of process_request(), from everything parsed
     * above it. ANY NEW FIELD HERE that changes WHERE the library looks,
     * or HOW MUCH it returns, has to be accounted for there - it is a
     * dozen lines below where you will be adding the parse, deliberately,
     * because the test used to live in another function entirely and was
     * therefore easy to add a qualifier without noticing. */
    bool plain;
} pmix_get_logic_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_get_logic_t);

/* struct for tracking ops */
typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    bool checked;
    int status;
    pmix_status_t pstatus;
    pmix_scope_t scope;
    pmix_buffer_t data;
    union {
        pmix_ptl_cbfunc_t ptlfn;
        pmix_op_cbfunc_t opfn;
        pmix_value_cbfunc_t valuefn;
        pmix_lookup_cbfunc_t lookupfn;
        pmix_spawn_cbfunc_t spawnfn;
        pmix_hdlr_reg_cbfunc_t hdlrregfn;
        pmix_info_cbfunc_t infofn;
        pmix_device_dist_cbfunc_t distfn;
    } cbfunc;
    size_t errhandler_ref;
    void *cbdata;
    pmix_name_t pname;
    char *key;
    pmix_value_t *value;
    pmix_proc_t *proc;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *directives;
    size_t ndirs;
    pmix_device_distance_t *dist;
    pmix_byte_object_t *bo;
    bool infocopy;
    bool dircopy;
    size_t nvals;
    pmix_list_t kvs;
    bool copy;
    /* The realm this fetch is answered from, cached so it is computed
     * once. PMIX_GDS_FETCH_KV fills it in when a caller has not; a
     * caller that already knows (the client parse) sets it and the
     * macro leaves it alone. */
    pmix_realm_t realm;
    pmix_get_logic_t *lg;
    bool timer_running;
    pmix_fabric_t *fabric;
    pmix_topology_t *topo;
} pmix_cb_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_cb_t);

#define PMIX_THREADSHIFT(r, c)                                                      \
    do {                                                                            \
        pmix_event_assign(&((r)->ev), pmix_globals.evbase, -1, EV_WRITE, (c), (r)); \
        PMIX_POST_OBJECT((r));                                                      \
        pmix_event_active(&((r)->ev), EV_WRITE, 1);                                 \
    } while (0)

#define PMIX_THREADSHIFT_DELAY(r, c, t)                                  \
    do {                                                                 \
        struct timeval _tv = {0, 0};                                     \
        pmix_event_evtimer_set(pmix_globals.evbase, &(r)->ev, (c), (r)); \
        _tv.tv_sec = (int) (t);                                          \
        _tv.tv_usec = ((t) -_tv.tv_sec) * 1000000.0;                     \
        PMIX_POST_OBJECT((r));                                           \
        pmix_event_evtimer_add(&(r)->ev, &_tv);                          \
    } while (0)

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    /* timestamp receipt of the notification so we
     * can evict the oldest one if we get overwhelmed */
    time_t ts;
    /* what room of the hotel they are in */
    int room;
    pmix_status_t status;
    pmix_proc_t source;
    pmix_data_range_t range;
    bool staylocal;  // do not pass up to host environment
    /* For notification, we use the targets field to track
     * any custom range of procs that are to receive the
     * event.
     */
    pmix_proc_t *targets;
    size_t ntargets;
    size_t nleft; // number of targets left to be notified
    /* the procs we have already sent this event to. A cached event is
     * replayed to a peer every time that peer registers another handler
     * covering the code, and the live dispatch may have sent it to that
     * peer already - without this record each of those deliveries is a
     * duplicate for the client and another decrement of "nleft", which
     * evicts the event before the remaining targets have seen it */
    pmix_proc_t *notified;
    size_t nnotified;
    /* When generating a notification, the originator can
     * specify the range of procs affected by this event.
     * For example, when creating a JOB_TERMINATED event,
     * the RM can specify the nspace of the job that has
     * ended, thus allowing users to provide a different
     * callback object based on the nspace being monitored.
     * We use the "affected" field to track these values
     * when processing the event chain.
     */
    pmix_proc_t *affected;
    size_t naffected;
    /* track if the event generator stipulates that default
     * event handlers are/are not to be given the event */
    bool nondefault;
    /* carry along any other provided info so the individual
     * handlers can look at it */
    pmix_info_t *info;
    size_t ninfo;
    /* allow for a buffer to be carried across internal processing */
    pmix_buffer_t *buf;
    /* the final callback to be executed upon completion of the event */
    pmix_op_cbfunc_t cbfunc;
    void *cbdata;
} pmix_notify_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_notify_caddy_t);

typedef struct {
    pmix_object_t super;
    /** Points to key <--> index translation table. An entry's index is
     *  its slot number here, so this is the index -> entry direction and
     *  it owns the entries. */
    pmix_pointer_array_t *table;
    /** The string -> entry direction, keyed by the key string. Holds
     *  borrowed pointers to the same entries "table" owns, purely so a
     *  key does not have to be found by scanning. Anything that adds to,
     *  removes from, or renumbers "table" without going through
     *  pmix_hash_register_key() must call pmix_hash_keyindex_rebuild()
     *  afterwards to put the two back in step. */
    pmix_hash_table_t *lookup;
    /** Stores the next ID. */
    uint32_t next_id;
    /** The reserved/non-reserved dividing line THIS index was numbered
     *  against - the PMIX_INDEX_BOUNDARY of whatever built it.
     *
     *  Recorded rather than assumed because a reader may not be the
     *  builder. A gds/shmem3 segment carries its own key index, and the
     *  process that maps it can be a different PMIx release: the
     *  boundary is one past the highest attribute id ever assigned, so
     *  it GROWS with every release that adds an attribute. A reader
     *  splitting the ids with its own boundary therefore reads an older
     *  writer's private key - numbered from the older, lower boundary -
     *  as a reserved one, and resolves it to whichever attribute its own
     *  dictionary happens to hold at that id. The value is right and the
     *  key it is reported under is not.
     *
     *  Reserved ids themselves need no such care: they are append-only
     *  and never reused (contrib/dictionary_ids.txt), so an id below the
     *  writer's boundary means the same attribute in every release that
     *  has it, and one this build has never heard of resolves to nothing
     *  rather than to something wrong. */
    uint32_t boundary;
} pmix_keyindex_t;

/** How the process-global keyindex is sized.
 *
 * It holds the reserved dictionary - PMIX_INDEX_BOUNDARY entries - plus
 * whatever non-reserved keys the process goes on to register. It lives
 * on the ordinary heap, so exceeding this costs a rehash and nothing
 * more.
 *
 * A keyindex built inside a gds/shmem3 segment is NOT sized from this;
 * see pmix_keyindex_init(). */
#define PMIX_KEYINDEX_GLOBAL_SIZE 1024
#define PMIX_KEYINDEX_TABLE_BLOCK  128
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_keyindex_t);

/** Bytes an *empty* pmix_keyindex_t occupies: the object plus the pointer
 * array and lookup table its constructor builds, both at fixed capacity.
 * Reported rather than restated so a caller reserving space for one cannot
 * drift from keyindex_construct(). See pmix_hash_sizeof_key_entry() for
 * what each registered key adds on top. */
/** Size a keyindex for the number of keys it will hold.
 *
 * A keyindex is constructed EMPTY - both its children are NULL - and
 * this is what builds them. Splitting the two is what lets a keyindex
 * inside a gds/shmem3 segment be sized to that segment's own key count:
 * such an index carries only the non-reserved keys (the reserved ones
 * have ids every process agrees on), and an update segment typically has
 * a handful or none.
 *
 * It used to be built at a fixed 1024/2048 whatever it would hold, which
 * cost ~168 KB for an index holding one key - and, in the other
 * direction, silently rehashed past ~2055 keys. A rehash on the heap is
 * a slow path; inside a segment it allocates from a bump allocator with
 * nowhere to grow, past what the caller reserved, so it aborts the
 * server. Sizing it here removes both.
 *
 * Reserve space for one with pmix_keyindex_sizeof_storage(nkeys), and
 * pass that same nkeys here.
 */
PMIX_EXPORT pmix_status_t pmix_keyindex_init(pmix_keyindex_t *ki, size_t nkeys);

/** Storage a pmix_keyindex_init(ki, nkeys) will consume.
 *
 * A caller placing one in a gds/shmem3 segment has to reserve this
 * before anything is allocated in it, and being short is an abort rather
 * than a slow path - so ask rather than restate the arithmetic. */
PMIX_EXPORT size_t pmix_keyindex_sizeof_storage(size_t nkeys);

#define PMIX_KEYINDEX_STATIC_INIT                 \
{                                                 \
    .super = PMIX_OBJ_STATIC_INIT(pmix_object_t), \
    .table = NULL,                                \
    .lookup = NULL,                               \
    .next_id = PMIX_INDEX_BOUNDARY,               \
    .boundary = PMIX_INDEX_BOUNDARY               \
}

/****    GLOBAL STORAGE    ****/
/* define a global construct that includes values that must be shared
 * between various parts of the code library. This structure is
 * instanced and initialized in runtime/pmix_init.c */
typedef struct {
    // control atomics
    bool init_called;
    atomic_bool initialized;
    atomic_bool util_initialized;
    atomic_bool connected;
    atomic_bool progress_thread_stopped;
    // proc info
    pmix_proc_t myid;
    pmix_value_t myidval;
    pmix_value_t myrankval;
    pmix_peer_t *mypeer; // my own peer object
    uid_t realuid;       // real uid
    uid_t uid;           // my effective uid
    gid_t realgid;       // real gid
    gid_t gid;           // my effective gid
    char *hostname;      // my hostname
    char **aliases;      // aliases for my hostname
    uint32_t appnum;     // my appnum
    pid_t pid;           // my local pid
    uint32_t nodeid;     // my nodeid, if given
    uint32_t sessionid;  // my sessionid, if given
    int pindex;
    pmix_event_base_t *evbase;
    pmix_event_base_t *evauxbase;
    int debug_output;
    pmix_events_t events; // my event handler registrations.
    struct timeval event_window;
    pmix_list_t cached_events;         // events waiting in the window prior to processing
    pmix_pointer_array_t iof_requests; // array of pmix_iof_req_t IOF requests
    int max_events;                    // size of the notifications hotel
    int event_eviction_time;           // max time to cache notifications
    pmix_hotel_t notifications;        // hotel of pending notifications
    /* IOF controls */
    bool pushstdin;
    pmix_list_t stdin_targets; // list of pmix_namelist_t
    bool tag_output;
    bool xml_output;
    bool timestamp_output;
    size_t output_limit;
    pmix_list_t nspaces;
    pmix_topology_t topology;
    pmix_cpuset_t cpuset;
    bool external_topology;
    bool external_progress;
    pmix_iof_flags_t iof_flags;
    /* Output arriving for a namespace we cannot yet format for.
     *
     * A tool's spawn directives are parsed before the request goes out (see
     * pmix_server_spawn_parser() in PMIx_Spawn_nb, "helps to catch any early
     * output"), but they can only be recorded against the namespace once the
     * reply names it - and a proc that writes and exits immediately can get
     * its output to us first. Nothing in scope at that moment says which of
     * our in-flight spawns the output belongs to, and nothing can until the
     * reply arrives. So the bytes are held here instead of being formatted
     * on a guess, and the reply - which knows both the namespace and the
     * directives it was issued with - releases them.
     *
     * spawns_in_flight counts the spawns we have issued that carried output
     * directives and have not yet been answered; while it is non-zero there
     * is a reply coming that can name a namespace, so output for a namespace
     * we have no formatting for is worth holding. It is incremented on the
     * caller's thread ahead of the send and decremented on the progress
     * thread by the reply handler, hence the atomic. iof_pending and
     * iof_pending_bytes are progress-thread state, bounded by
     * iof_pending_limit; past the limit the stand-in below is used instead.
     *
     * spawn_iof_flags is that stand-in: the directives of the most recent
     * such spawn, kept only as the overflow answer and cleared once nothing
     * is in flight. It cannot distinguish concurrent spawns, which is why
     * the cache exists. Note its file and directory members are deliberately
     * left NULL - that copy does not own strings, it only carries the
     * formatting decisions. */
    pmix_atomic_size_t spawns_in_flight;
    pmix_list_t iof_pending;
    size_t iof_pending_bytes;
    size_t iof_pending_limit;
    /* How often, in milliseconds, to re-check whether our terminal has come
     * back to the foreground while our own stdin is suspended for being in
     * the background. See the note over stdin_resume_arm() in
     * src/common/pmix_iof.c for why this is a poll and not a SIGCONT
     * handler. The first check is _interval after we lose the terminal and
     * the delay doubles up to _max_interval, so a quick "fg" is answered
     * promptly and a process left in the background all day settles to the
     * ceiling. The timer exists ONLY while a host has asked us to forward
     * its stdin AND that stdin is suspended for being in the background:
     * nothing else ever arms it. */
    int iof_stdin_resume_interval;
    int iof_stdin_resume_max_interval;
    pmix_iof_flags_t spawn_iof_flags;
    pmix_keyindex_t keyindex;
    /* An IMMUTABLE view of the reserved half of the dictionary, built
     * by pmix_init_registered_attrs() before the progress thread
     * starts and never written again.
     *
     * It exists because the lock-free read path needs one. A gds module
     * that reports is_tsafe answers a keyed get on the APPLICATION'S
     * thread, and resolving a reserved key there used to read
     * pmix_globals.keyindex - which the progress thread keeps writing,
     * because a non-reserved key is registered into it the first time
     * anyone stores one. Registering grows the pointer array (realloc,
     * old block freed) and the lookup table (new table, old freed), so
     * a reader could be walking storage that had just been handed back.
     *
     * The reserved entries are complete before any thread exists and
     * never change afterwards, so a snapshot of them is safe to read
     * without synchronization forever. These BORROW the entries owned
     * by keyindex above - do not free them from here. */
    pmix_pointer_array_t *dict_by_id;
    pmix_hash_table_t *dict_by_name;
} pmix_globals_t;

/* provide access to a function to cleanup epilogs */
PMIX_EXPORT void pmix_execute_epilog(pmix_epilog_t *ep);

PMIX_EXPORT pmix_status_t pmix_notify_event_cache(pmix_notify_caddy_t *cd);

/* Record that a cached/in-flight notification has been sent to a proc,
 * and report whether it had already been sent there. Returns true if
 * this proc is already on the caddy's notified list - the caller must
 * then neither send the event again nor decrement "nleft" - and false
 * otherwise, having added it. Every path that delivers a notification
 * to a local peer on behalf of one caddy must go through this, because
 * more than one of them can pick up the same cached event for the same
 * peer: the live dispatch, the replay a REGEVENTS_CMD triggers (which
 * runs once per registration message, not once per peer), and the
 * replay a newly-connected tool triggers. */
PMIX_EXPORT bool pmix_notify_mark_notified(pmix_notify_caddy_t *cd,
                                           const pmix_proc_t *proc);

PMIX_EXPORT extern pmix_globals_t pmix_globals;
PMIX_EXPORT extern const char* PMIX_PROXY_VERSION;
PMIX_EXPORT extern const char* PMIX_PROXY_BUGREPORT;

PMIX_EXPORT void pmix_log_local_op(int sd, short args, void *cbdata_);

static inline bool pmix_check_node_info(const char *key)
{
    static const char *const keys[] = {
        PMIX_HOSTNAME,                  PMIX_HOSTNAME_ALIASES,
        PMIX_NODEID,                    PMIX_AVAIL_PHYS_MEMORY,
        PMIX_LOCAL_PEERS,               PMIX_LOCAL_PROCS,
        PMIX_LOCAL_CPUSETS,             PMIX_LOCAL_SIZE,
        PMIX_NODE_SIZE,                 PMIX_LOCALLDR,
        PMIX_NODE_OVERSUBSCRIBED,       PMIX_FABRIC_DEVICES,
        PMIX_FABRIC_COORDINATES,        PMIX_FABRIC_DEVICE,
        PMIX_FABRIC_DEVICE_INDEX,       PMIX_FABRIC_DEVICE_NAME,
        PMIX_FABRIC_DEVICE_VENDOR,      PMIX_FABRIC_DEVICE_BUS_TYPE,
        PMIX_FABRIC_DEVICE_VENDORID,    PMIX_FABRIC_DEVICE_DRIVER,
        PMIX_FABRIC_DEVICE_FIRMWARE,    PMIX_FABRIC_DEVICE_ADDRESS,
        PMIX_FABRIC_DEVICE_MTU,         PMIX_FABRIC_DEVICE_COORDINATES,
        PMIX_FABRIC_DEVICE_SPEED,       PMIX_FABRIC_DEVICE_STATE,
        PMIX_FABRIC_DEVICE_TYPE,        PMIX_FABRIC_DEVICE_PCI_DEVID,
        NULL
    };
    size_t n;

    /* Every key below is a reserved ("pmix"-prefixed) attribute, so an
     * application key - which is what a PMIx_Get is usually asking for
     * - can be turned away on a four-byte compare instead of walking
     * the table. This runs twice per keyed get: once in
     * process_request() and again in the gds fetch. */
    if (NULL == key || !PMIx_Check_reserved_key(key)) {
        return false;
    }
    for (n = 0; NULL != keys[n]; n++) {
        if (0 == strncmp(key, keys[n], PMIX_MAX_KEYLEN)) {
            return true;
        }
    }
    return false;
}

static inline bool pmix_check_app_info(const char *key)
{
    static const char *const keys[] = {
        PMIX_APP_SIZE,  PMIX_APPLDR,       PMIX_APP_ARGV,      PMIX_WDIR,
        PMIX_PSET_NAME, PMIX_PSET_MEMBERS, PMIX_APP_MAP_TYPE,  PMIX_APP_MAP_REGEX,
        NULL
    };
    size_t n;

    /* Every key below is a reserved ("pmix"-prefixed) attribute, so an
     * application key - which is what a PMIx_Get is usually asking for
     * - can be turned away on a four-byte compare instead of walking
     * the table. This runs twice per keyed get: once in
     * process_request() and again in the gds fetch. */
    if (NULL == key || !PMIx_Check_reserved_key(key)) {
        return false;
    }
    for (n = 0; NULL != keys[n]; n++) {
        if (0 == strncmp(key, keys[n], PMIX_MAX_KEYLEN)) {
            return true;
        }
    }
    return false;
}

static inline bool pmix_check_session_info(const char *key)
{
    static const char *const keys[] = {
        PMIX_SESSION_ID, PMIX_CLUSTER_ID,   PMIX_UNIV_SIZE,
        PMIX_TMPDIR,     PMIX_TDIR_RMCLEAN, PMIX_HOSTNAME_KEEP_FQDN,
        PMIX_RM_NAME,    PMIX_RM_VERSION,
        NULL
    };
    size_t n;

    /* Every key below is a reserved ("pmix"-prefixed) attribute, so an
     * application key - which is what a PMIx_Get is usually asking for
     * - can be turned away on a four-byte compare instead of walking
     * the table. This runs twice per keyed get: once in
     * process_request() and again in the gds fetch. */
    if (NULL == key || !PMIx_Check_reserved_key(key)) {
        return false;
    }
    for (n = 0; NULL != keys[n]; n++) {
        if (0 == strncmp(key, keys[n], PMIX_MAX_KEYLEN)) {
            return true;
        }
    }
    return false;
}

static inline bool pmix_check_special_key(const char *key)
{
    static const char *const keys[] = {
        PMIX_GROUP_CONTEXT_ID,
        PMIX_GROUP_LOCAL_CID,
        NULL
    };
    size_t n;

    /* Every key below is a reserved ("pmix"-prefixed) attribute, so an
     * application key - which is what a PMIx_Get is usually asking for
     * - can be turned away on a four-byte compare instead of walking
     * the table. This runs twice per keyed get: once in
     * process_request() and again in the gds fetch. */
    if (NULL == key || !PMIx_Check_reserved_key(key)) {
        return false;
    }
    for (n = 0; NULL != keys[n]; n++) {
        if (0 == strncmp(key, keys[n], PMIX_MAX_KEYLEN)) {
            return true;
        }
    }
    return false;
}

static inline bool pmix_check_local(const char *hostname)
{
    size_t n;

    /* the name being tested comes from wherever the caller got it -- a
     * host-supplied info key, an unpacked message -- and our own hostname is
     * not set until pmix_init has run. Neither is safe to hand to strcmp
     * unchecked. A name we cannot compare is not local. */
    if (NULL == hostname || NULL == pmix_globals.hostname) {
        return false;
    }

    // first do simple check
    if (0 == strcmp(pmix_globals.hostname, hostname)) {
        return true;
    }

    // now check aliases
    if (NULL != pmix_globals.aliases) {
        for (n=0; NULL != pmix_globals.aliases[n]; n++) {
            if (0 == strcmp(pmix_globals.aliases[n], hostname)) {
                return true;
            }
        }
    }
    return false;
}

#if PMIX_PICKY_COMPILERS
#define PMIX_HIDE_UNUSED_PARAMS(...)                \
    do {                                            \
        int __x = 3;                                \
        pmix_hide_unused_params(__x, __VA_ARGS__);  \
    } while(0)

PMIX_EXPORT void pmix_hide_unused_params(int x, ...);

#else
#define PMIX_HIDE_UNUSED_PARAMS(...)
#endif

#define PMIX_TRACE_KEY_ACTUAL(s, k, v)                  \
do {                                                    \
    if (0 == strcmp(s, k)) {                            \
        char *_v = PMIx_Value_string(v);                \
        pmix_output(0, "[%s:%s:%d] %s\n%s\n",           \
                    __FILE__, __func__, __LINE__,       \
                    PMIx_Get_attribute_name(k), _v);    \
        free(_v);                                       \
    }                                                   \
} while(0)

#define PMIX_TRACE_KEY(c, s, k, v)                          \
do {                                                        \
    if (0 == strcasecmp(c, "SERVER") &&                     \
        PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {         \
        PMIX_TRACE_KEY_ACTUAL(s, k, v);                     \
    } else if (0 == strcasecmp(c, "CLIENT") &&              \
           !PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {     \
           PMIX_TRACE_KEY_ACTUAL(s, k, v);                  \
    }                                                       \
} while (0)

END_C_DECLS

#endif /* PMIX_GLOBALS_H */
