/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2016-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 */

#ifndef PMIX_SERVER_OPS_H
#define PMIX_SERVER_OPS_H

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

#include "src/include/pmix_config.h"
#include "pmix_common.h"
#include "pmix_server.h"

#include "src/class/pmix_hotel.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_hash.h"

#define PMIX_IOF_HOTEL_SIZE 256
#define PMIX_IOF_MAX_STAY   300000000

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_server_trkr_t *trk;
} pmix_trkr_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_trkr_caddy_t);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_peer_t *peer;
    char *nspace;
    pmix_status_t status;
    pmix_status_t *codes;
    size_t ncodes;
    /* number of leading entries of "codes" that this request marked as
     * newly active - the only ones an event registration may give back
     * if its host up-call is subsequently refused */
    size_t nactive;
    pmix_proc_t proc;
    pmix_proc_t *procs;
    size_t nprocs;
    uid_t uid;
    gid_t gid;
    void *server_object;
    int nlocalprocs;
    uint32_t sessionid;
    pmix_info_t *info;
    size_t ninfo;
    pmix_resource_unit_t *units;
    size_t nunits;
    bool copied;
    char **keys;
    pmix_app_t *apps;
    size_t napps;
    pmix_iof_channel_t channels;
    pmix_iof_flags_t flags;
    /* nothing in the spawn request decided which output channels the child
     * job should have forwarded, so the child inherits its parent's - see
     * pmix_server_spawn_parser() and pmix_server_process_iof() */
    bool inherit_iof;
    bool xoff;      // IOF flow control: suspend (true) or resume (false)
    pmix_byte_object_t *bo;
    size_t nbo;
    pmix_op_cbfunc_t opcbfunc;
    pmix_dmodex_response_fn_t cbfunc;
    pmix_setup_application_cbfunc_t setupcbfunc;
    pmix_lookup_cbfunc_t lkcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    void *cbdata;
    /* an IOF pull registration waiting for the host's approval - retained,
     * so the approval can tell whether its slot still holds it */
    pmix_iof_req_t *iofreq;
} pmix_setup_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_setup_caddy_t);

typedef struct {
    pmix_list_item_t super;
    pmix_setup_caddy_t *cd;
} pmix_dmdx_remote_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_dmdx_remote_t);

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t proc;     // id of proc whose data is being requested
    pmix_list_t loc_reqs; // list of pmix_dmdx_request_t elem is keeping track of
                          // all local ranks that are interested in this namespace-rank
    pmix_info_t *info;    // array of info structs for this request
    size_t ninfo;         // number of info structs
    bool requested;       // the host has already been asked for this target
    /* who is asking: requests are shared only by requesters of the same
     * uid, gid and job, since the server holding the data decides for
     * that identity - see docs/security-plan.rst */
    uid_t uid;
    gid_t gid;
    pmix_nspace_t reqns;
} pmix_dmdx_local_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_dmdx_local_t);

typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    bool event_active;
    pmix_dmdx_local_t *lcd;
    char *key;
    pmix_modex_cbfunc_t cbfunc; // cbfunc to be executed when data is available
    void *cbdata;
} pmix_dmdx_request_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_dmdx_request_t);

/* event/error registration book keeping */
typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *peer;
    bool enviro_events;
    pmix_proc_t *affected;
    size_t naffected;
} pmix_peer_events_info_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_peer_events_info_t);

typedef struct {
    pmix_list_item_t super;
    pmix_list_t peers; // list of pmix_peer_events_info_t
    int code;
    /* number of registrations the server itself has made for this
     * code - registrations by local clients are counted by "peers" */
    size_t nmine;
    /* true if we have asked our host to forward this code to us */
    bool active;
} pmix_regevents_info_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_regevents_info_t);

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t source;
    pmix_iof_channel_t channel;
    pmix_byte_object_t *bo;
    pmix_info_t *info;
    size_t ninfo;
} pmix_iof_cache_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_cache_t);

typedef struct {
    pmix_list_item_t super;
    char *name;
    pmix_proc_t *members;
    size_t nmembers;
} pmix_pset_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pset_t);

typedef struct {
    bool module_set;    // pmix_host_server has been set
    pmix_list_t nspaces;          // list of pmix_nspace_t for the nspaces we know about
    pmix_pointer_array_t clients; // array of pmix_peer_t local clients
    /* array of synthetic pmix_peer_t objects created solely to carry the
     * bfrops module of a foreign nspace we have been asked to pack data
     * for. These are NOT local clients: they have no rank_info, no socket
     * and no process behind them, so they are kept out of the clients
     * array - everything that walks that array (monitoring, cleanup
     * targeting, tool identity checks, peer lookup) reads peer->info as a
     * matter of course. See _findpeer() in src/common/pmix_data.c */
    pmix_pointer_array_t peer_cache;
    pmix_list_t collectives;      // list of active pmix_server_trkr_t
    pmix_list_t remote_pnd; // list of pmix_dmdx_remote_t awaiting arrival of data fror servicing
                            // remote req's
    pmix_list_t local_reqs;     // list of pmix_dmdx_local_t awaiting arrival of data from local neighbours
    pmix_list_t gdata;  // cache of data given to me for passing to all clients
    pmix_list_t events; // list of pmix_regevents_info_t registered events
    pmix_list_t iof;    // IO to be forwarded to clients
    pmix_list_t iof_residuals;  // leftover bytes waiting for newline
    pmix_list_t psets;  // list of known psets and memberships
    size_t max_iof_cache; // max number of IOF messages to cache
    bool tool_connections_allowed;
    char *tmpdir;             // temporary directory for this server
    char *system_tmpdir;      // system tmpdir
    bool fence_localonly_opt; // local-only fence optimization
    pmix_list_t grp_collectives;  // group-op collectives
    pmix_pointer_array_t monitors;  // monitoring operations
    // verbosity for server get operations
    int get_output;
    int get_verbose;
    // verbosity for server connect operations
    int connect_output;
    int connect_verbose;
    // verbosity for server fence operations
    int fence_output;
    int fence_verbose;
    // verbosity for server pub operations
    int pub_output;
    int pub_verbose;
    // verbosity for server spawn operations
    int spawn_output;
    int spawn_verbose;
    // verbosity for server event operations
    int event_output;
    int event_verbose;
    // verbosity for server iof operations
    int iof_output;
    int iof_verbose;
    // verbosity for basic server functions
    int base_output;
    int base_verbose;
    // verbosity for server group operations
    int group_output;
    int group_verbose;
} pmix_server_globals_t;

/* Build the switchyard's per-command caddy. A failed allocation leaves
 * (c) NULL and takes no reference: this used to write through the NULL
 * on the very next line, so one allocation failure killed the server and
 * every client it was hosting. Every dispatch arm tests (c) and answers
 * the requester PMIX_ERR_NOMEM instead. */
#define PMIX_GDS_CADDY(c, p, t)                  \
    do {                                         \
        (c) = PMIX_NEW(pmix_server_caddy_t);     \
        if (PMIX_LIKELY(NULL != (c))) {          \
            (c)->hdr.tag = (t);                  \
            PMIX_RETAIN((p));                    \
            (c)->peer = (p);                     \
        } else {                                 \
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);      \
        }                                        \
    } while (0)

/* The caddy carries the tracker across an async hop onto the progress
 * thread, so it takes a reference for the duration - anything else that
 * runs in between (a departing participant completing the collective, for
 * one) would otherwise release the tracker out from under the handler.
 * The caddy's destructor gives the reference back. Note the reference
 * keeps the object alive; it does NOT keep it on the collectives list, so
 * a handler must still check that it is there before acting on it. */
/* Both macros tolerate the allocation failing: the caddy pointer is left
 * NULL and the tracker keeps the reference it had. Writing (c)->trk
 * unconditionally was a NULL dereference on the progress thread - the
 * collectives sweep in pmix_server_registration.c is the only user, and
 * it runs while holding no lock it could release. The collective does
 * not get driven in that case, which is a hang rather than a crash;
 * callers that can do better should test (c). */
#define PMIX_SETUP_COLLECTIVE(c, t)        \
    do {                                   \
        (c) = PMIX_NEW(pmix_trkr_caddy_t); \
        if (NULL != (c)) {                 \
            PMIX_RETAIN((t));              \
            (c)->trk = (t);                \
        }                                  \
    } while (0)

#define PMIX_EXECUTE_COLLECTIVE(c, t, f)                                                \
    do {                                                                                \
        PMIX_SETUP_COLLECTIVE(c, t);                                                    \
        if (NULL != (c)) {                                                              \
            pmix_event_assign(&((c)->ev), pmix_globals.evbase, -1, EV_WRITE, (f), (c)); \
            pmix_event_active(&((c)->ev), EV_WRITE, 1);                                 \
        }                                                                               \
    } while (0)

PMIX_EXPORT void pmix_pending_nspace_requests(pmix_namespace_t *nptr);
PMIX_EXPORT pmix_status_t pmix_pending_resolve(pmix_namespace_t *nptr, pmix_rank_t rank,
                                               pmix_status_t status, pmix_scope_t scope,
                                               pmix_dmdx_local_t *lcd);

/* Fail every requester parked on this direct-modex tracker with the given
 * status, then unlink and release the tracker. Each parked request holds
 * its own reference on the tracker and owns the server caddy of the client
 * waiting behind it, so a caller that simply releases the tracker leaks
 * both and leaves those clients waiting forever. */
PMIX_EXPORT void pmix_server_fail_local_reqs(pmix_dmdx_local_t *lcd,
                                             pmix_status_t status);

/* The mirror of the above for the other deferral list. remote_pnd holds
 * the host's own PMIx_server_dmodex_request calls, parked until the
 * local client they name commits its data - and committing is the only
 * thing that ever takes one off the list again. When that client departs
 * instead, answer the host with a status: it is holding a request on
 * behalf of a remote server whose client is blocked in PMIx_Get, and
 * nothing else will ever tell it otherwise. Matches on either the
 * departing peer or a proc (whose rank may be PMIX_RANK_WILDCARD for a
 * whole namespace), exactly as pmix_server_purge_events does. */
PMIX_EXPORT void pmix_server_fail_remote_pnd(pmix_peer_t *peer,
                                             pmix_proc_t *proc,
                                             pmix_status_t status);

/* Answer one parked request with a status, unlink it and release it. */
PMIX_EXPORT void pmix_server_fail_remote_req(pmix_dmdx_remote_t *dcd,
                                             pmix_status_t status);

/* Answer every request still parked on remote_pnd. Both roles that own
 * the list owe this at teardown for the same reason the local_reqs drain
 * above is owed: a bare PMIX_LIST_DESTRUCT frees the caddies without ever
 * calling the pmix_dmodex_response_fn_t the host supplied, so the host is
 * left holding a request it will never hear about again. */
PMIX_EXPORT void pmix_server_drain_remote_pnd(pmix_status_t status);

PMIX_EXPORT pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                            pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_commit(pmix_peer_t *peer, pmix_buffer_t *buf);

PMIX_EXPORT pmix_status_t pmix_server_fence(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                            pmix_modex_cbfunc_t modexcbfunc);

PMIX_EXPORT pmix_status_t pmix_server_get(pmix_buffer_t *buf, pmix_modex_cbfunc_t cbfunc,
                                          void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_publish(pmix_peer_t *peer, pmix_buffer_t *buf,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_lookup(pmix_peer_t *peer, pmix_buffer_t *buf,
                                             pmix_lookup_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_unpublish(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_spawn(pmix_peer_t *peer, pmix_buffer_t *buf,
                                            pmix_spawn_cbfunc_t cbfunc, void *cbdata);
/* Parse a spawn request's output-forwarding directives. On return,
 * *inherit is true when nothing in the request decided which channels to
 * forward, which is the caller's signal to give the child job its
 * parent's settings instead - see pmix_server_process_iof(). */
PMIX_EXPORT void pmix_server_spawn_parser(pmix_peer_t *peer,
                                          pmix_iof_channel_t *channels,
                                          pmix_iof_flags_t *flags,
                                          bool *inherit,
                                          pmix_info_t *info,
                                          size_t ninfo);
PMIX_EXPORT pmix_status_t pmix_server_process_iof(pmix_setup_caddy_t *cd,
                                                  char nspace[]);

PMIX_EXPORT void pmix_server_spcbfunc(pmix_status_t status, char nspace[], void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_connect(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                              pmix_op_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_disconnect(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                                 pmix_op_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_notify_error(pmix_status_t status, pmix_proc_t procs[],
                                                   size_t nprocs, pmix_proc_t error_procs[],
                                                   size_t error_nprocs, pmix_info_t info[],
                                                   size_t ninfo, pmix_op_cbfunc_t cbfunc,
                                                   void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_register_events(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                      pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT void pmix_server_deregister_events(pmix_peer_t *peer, pmix_buffer_t *buf);

/* Mark an array about to be passed up to the host with the identity of
 * the peer the request came from, so it holds exactly one PMIX_USERID and
 * one PMIX_GRPID:
 *  - PMIX_USERID is the uid recorded for the peer at connection; any the
 *    requester supplied is dropped - unless it is marked
 *    PMIX_INFO_RELAYED, a server relaying the request for the process that
 *    made it, and then it is kept, still marked.
 *  - PMIX_GRPID likewise, if relayed. Otherwise it is the requester's
 *    choice if it supplied one - a group to charge the work to, which the
 *    host decides whether to accept - given as a number or a group name,
 *    and always passed on as a uint32; otherwise the gid recorded at
 *    connection. Only the first supplied PMIX_GRPID counts. If it cannot
 *    be resolved the array is left as it was and the error is returned
 *    (PMIX_ERR_NOT_FOUND for an unknown name, PMIX_ERR_BAD_PARAM for a
 *    value of the wrong type).
 * The pair is appended at the end of the array. The array is replaced by
 * a new one - *info and *ninfo are updated, and the old array is released
 * - so the caller must own it. A NULL/zero array is fine and comes back
 * holding just the pair. */
PMIX_EXPORT pmix_status_t pmix_server_add_requester_id(pmix_peer_t *peer, pmix_info_t **info,
                                                       size_t *ninfo);

/* Access to a namespace by user and group - see docs/security-plan.rst and
 * pmix_server_access.c.
 *
 * A host applies the same rule to what it does for clients and tools by
 * keeping its own copies - a pmix_access_t for each job, built with
 * pmix_server_access_load() from the info it registers the job with, and a
 * pmix_user_t for each requester - and calling pmix_server_access_check()
 * with them from its own thread. Those functions, and the pmix_user_t and
 * pmix_access_t helpers, touch nothing but their arguments; everything
 * else here runs on the progress thread. PMIX_CAP_ACCESS_CHECK says they
 * exist. */

/* A user, and every group it belongs to - primary and supplementary */
typedef struct {
    pmix_list_item_t super;
    uid_t uid;
    gid_t *gids;
    uint16_t ngids;
} pmix_user_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_user_t);

/* A new user record for uid, its groups not yet looked up -
 * pmix_server_access_check() looks them up when it needs them */
PMIX_EXPORT pmix_user_t *pmix_server_user_create(uid_t uid);

/* Look the user's groups up again. A user with no account belongs to
 * none. */
PMIX_EXPORT pmix_status_t pmix_server_user_refresh(pmix_user_t *user);

/* The rule: may requester access job? PMIX_SUCCESS for root, the user
 * this process runs as, the job's owner (our own user when it has none),
 * a user its access list names, or a member of a group it names - else
 * PMIX_ERR_NO_PERMISSIONS. A requester found in none of the job's groups
 * has its groups looked up again, in place, in case they changed, before
 * it is refused. */
PMIX_EXPORT pmix_status_t pmix_server_access_check(pmix_user_t *requester,
                                                   const pmix_access_t *job);

/* A job's owner and access list for a host's own copy: construct it,
 * load it from the info the job is registered with (PMIX_USERID,
 * PMIX_GRPID, PMIX_ACCESS_PERMISSIONS or its PMIX_ACCESS_USERIDS /
 * PMIX_ACCESS_GRPIDS, at the top level or in a PMIX_JOB_INFO_ARRAY; names
 * are resolved), and destruct it. A malformed access list returns
 * PMIX_ERR_BAD_PARAM and changes nothing; a list the info does not name
 * is kept as it was. */
PMIX_EXPORT void pmix_server_access_construct(pmix_access_t *acc);
PMIX_EXPORT pmix_status_t pmix_server_access_load(pmix_access_t *acc, const pmix_info_t *info,
                                                  size_t ninfo);
PMIX_EXPORT void pmix_server_access_destruct(pmix_access_t *acc);

/* The server's user records. A user is added, its groups looked up then,
 * when the host registers it (PMIx_server_register_resources with
 * PMIX_USERID), registers a job it owns, or when it connects as a tool -
 * once per user, and never for root or our own user, who need no groups.
 * pmix_server_user_get() returns the record, making one whose groups are
 * looked up at its first check for a requester nobody registered (one on
 * another node). Records are kept until the host deregisters the user -
 * pmix_server_user_remove(), on PMIx_server_deregister_resources naming
 * the PMIX_USERID - or the server finalizes. */
PMIX_EXPORT void pmix_server_user_add(uid_t uid);

/* A user the host registers (PMIx_server_register_resources with
 * PMIX_USERID): with gids - the groups the host says it belongs to, from a
 * PMIX_GRPID in the same call - the record takes them, replacing any it
 * had, and nothing is looked up; without, as pmix_server_user_add() */
PMIX_EXPORT pmix_status_t pmix_server_user_register(uid_t uid, const gid_t *gids, size_t ngids);

/* The groups a PMIX_GRPID names: one group, or a data array of them, each a
 * number or a name. *gids is allocated; the caller frees it */
PMIX_EXPORT pmix_status_t pmix_server_gids_from_value(const pmix_value_t *val, gid_t **gids,
                                                      size_t *ngids);
PMIX_EXPORT pmix_user_t *pmix_server_user_get(uid_t uid);
PMIX_EXPORT void pmix_server_user_remove(uid_t uid);

/* pmix_server_access_check for nptr, with the server's record for uid */
PMIX_EXPORT bool pmix_server_access_permitted(uid_t uid, const pmix_namespace_t *nptr);

/* the same for a connected peer, by the identity it connected with - and
 * a job's own processes, and anyone reading the server's own namespace,
 * are always allowed */
PMIX_EXPORT bool pmix_server_peer_permitted(const pmix_peer_t *peer, const pmix_namespace_t *nptr);

/* For data this server answers itself: as pmix_server_peer_permitted, but a
 * namespace the host never registered with us (see pmix_access_t.registered)
 * is not ours to judge, and is allowed - its data comes from the host */
PMIX_EXPORT bool pmix_server_peer_may_access(const pmix_peer_t *peer,
                                             const pmix_namespace_t *nptr);

/* the same, by namespace name - an empty or unknown name is allowed, since
 * it names no job this server holds */
PMIX_EXPORT bool pmix_server_peer_may_access_nspace(const pmix_peer_t *peer, const char *nspace);

/* Record the owner and access list a host registration gives for nptr -
 * PMIX_USERID, PMIX_GRPID and PMIX_ACCESS_PERMISSIONS (or its
 * PMIX_ACCESS_USERIDS / PMIX_ACCESS_GRPIDS), at the top level or inside a
 * PMIX_JOB_INFO_ARRAY. Names must already be resolved. An owner id that is
 * not a number is ignored, as it always has been; a malformed access list
 * returns PMIX_ERR_BAD_PARAM and changes nothing. */
PMIX_EXPORT pmix_status_t pmix_server_access_set(pmix_namespace_t *nptr, const pmix_info_t *info,
                                                 size_t ninfo);

/* Set nptr's owner from a source of the given strength - a weaker source
 * never replaces a stronger one (see pmix_owner_source_t) */
PMIX_EXPORT void pmix_server_access_set_owner(pmix_namespace_t *nptr, uid_t uid, gid_t gid,
                                              pmix_owner_source_t source);

/* Data held on another node - see docs/security-plan.rst. A job not
 * registered here whose data we fetched from the server holding it records
 * the requester identities that server approved (pmix_access_t apv_*) */
PMIX_EXPORT pmix_status_t pmix_server_access_approve(pmix_namespace_t *nptr, uid_t uid, gid_t gid);

/* May the peer be answered from what we hold for nptr? For a registered job,
 * pmix_server_peer_permitted. For one we only hold a copy of, the job's own
 * processes, root, our own user, and the identities its holder approved -
 * anyone else must be asked about by the holder */
PMIX_EXPORT bool pmix_server_peer_may_use_copy(const pmix_peer_t *peer,
                                               const pmix_namespace_t *nptr);

/* the same, by namespace name - a name we hold nothing for is allowed,
 * since there is nothing here to answer from */
PMIX_EXPORT bool pmix_server_peer_may_use_copy_nspace(const pmix_peer_t *peer, const char *nspace);

/* The holder's check for a remote requester, named in a direct-modex
 * request by PMIX_USERID, PMIX_GRPID and PMIX_REQUESTOR: success when no
 * requester is named (the host asking for itself), for the job's own
 * processes, or when the rule allows; else PMIX_ERR_NO_PERMISSIONS */
PMIX_EXPORT pmix_status_t pmix_server_access_check_remote(const pmix_namespace_t *nptr,
                                                          const pmix_info_t *info, size_t ninfo);

/* Name the requester on directives going to our host for a direct modex:
 * any PMIX_USERID, PMIX_GRPID or PMIX_REQUESTOR already there is replaced
 * by the peer's connection uid and gid and its process ID. The array is
 * replaced; the old one freed */
PMIX_EXPORT pmix_status_t pmix_server_access_identify(const pmix_peer_t *peer, pmix_info_t **info,
                                                      size_t *ninfo);

/* the connected client or tool that proc is, or NULL */
PMIX_EXPORT pmix_peer_t *pmix_server_access_find_peer(const pmix_proc_t *proc);

/* Who a request acting on processes is for.
 *   - a connected client or tool: *peer is it, *uid its user;
 *   - a request the host relays for a user (its directives carry
 *     PMIX_USERID): *uid is that user;
 *   - otherwise the host's own: *host is set and *uid is our own user.
 * A request marked as relayed (PMIX_MONITOR_PROXY) that does not say for
 * whom is refused with PMIX_ERR_NO_PERMISSIONS - it would otherwise pass
 * as the host's own, which is not restricted. */
PMIX_EXPORT pmix_status_t pmix_server_access_requester(const pmix_proc_t *requestor,
                                                       const pmix_info_t *directives,
                                                       size_t ndirs, pmix_peer_t **peer,
                                                       bool *host, uid_t *uid);

/* May that requester act on this job? The host may act on any */
PMIX_EXPORT bool pmix_server_access_requester_may(pmix_peer_t *peer, bool host, uid_t uid,
                                                  pmix_namespace_t *nptr);

/* release the server's user records */
PMIX_EXPORT void pmix_server_access_finalize(void);

/* A PMIX_USERID or PMIX_GRPID - and each entry of a PMIX_ACCESS_USERIDS or
 * PMIX_ACCESS_GRPIDS list - may be given as a name, and is resolved to its
 * number where it enters the library, so that only the number is used from
 * there on - internally and with the host. For an array passed in that is
 * not ours to change: if it (or an info array nested in it) gives any of
 * them by name, *out is a copy with the names resolved, which the
 * caller releases with PMIx_Info_free(*out, *nout); otherwise *out is NULL
 * and the array can be used as it is. A name that does not resolve returns
 * PMIX_ERR_NOT_FOUND. */
PMIX_EXPORT pmix_status_t pmix_server_normalize_ids(const pmix_info_t *info, size_t ninfo,
                                                    pmix_info_t **out, size_t *nout);

/* For an up-call that has no process argument (iof_pull, register_events):
 * name the requesting peer in the array with PMIX_REQUESTOR, in place of
 * any the requester supplied. Replaces the array exactly as
 * pmix_server_add_requester_id does. */
PMIX_EXPORT pmix_status_t pmix_server_add_requester_proc(pmix_peer_t *peer, pmix_info_t **info,
                                                         size_t *ninfo);

PMIX_EXPORT pmix_status_t pmix_server_query(pmix_peer_t *peer, pmix_buffer_t *buf,
                                            pmix_info_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_log(pmix_peer_t *peer, pmix_buffer_t *buf,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_alloc(pmix_peer_t *peer, pmix_buffer_t *buf,
                                            pmix_info_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_job_ctrl(pmix_peer_t *peer, pmix_buffer_t *buf,
                                               pmix_info_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_monitor(pmix_peer_t *peer, pmix_buffer_t *buf,
                                              pmix_info_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_get_credential(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                     pmix_credential_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_validate_credential(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                          pmix_validation_cbfunc_t cbfunc,
                                                          void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_iofreg(pmix_peer_t *peer, pmix_buffer_t *buf,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_iofstdin(pmix_peer_t *peer, pmix_buffer_t *buf,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_iofdereg(pmix_peer_t *peer, pmix_buffer_t *buf,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_group(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                            pmix_group_operation_t op);
PMIX_EXPORT pmix_status_t pmix_server_group_join(pmix_server_caddy_t *cd,
                                                 pmix_buffer_t *buf,
                                                 pmix_op_cbfunc_t cbfunc);
PMIX_EXPORT pmix_status_t pmix_server_group_invite(pmix_server_caddy_t *cd,
                                                   pmix_buffer_t *buf,
                                                   pmix_op_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_event_recvd_from_client(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                              pmix_op_cbfunc_t cbfunc,
                                                              void *cbdata);
PMIX_EXPORT void pmix_server_execute_collective(int sd, short args, void *cbdata);

/* Fail a collective that can no longer be completed: answer every local
 * participant with the given status, then unlink and release the tracker.
 * Every arm that abandons a tracker owes this - a tracker simply released
 * hangs each participant parked on it, since the caddy destructor sends
 * nothing. Implemented in pmix_server_registration.c beside
 * pmix_server_execute_collective; also used by pmix_server_trk_peer_lost. */
PMIX_EXPORT void pmix_server_fail_collective(pmix_server_trkr_t *trk,
                                             pmix_status_t status);

PMIX_EXPORT pmix_status_t pmix_server_initialize(void);

/* Generic completion callback used by the blocking form of the public
 * server APIs: it records the status where the waiting caller can read
 * it and wakes the caller's lock. The cbdata must be a pmix_lock_t. */
PMIX_EXPORT void pmix_server_lock_opcbfunc(pmix_status_t status, void *cbdata);

PMIX_EXPORT void pmix_server_message_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                                             pmix_buffer_t *buf, void *cbdata);

/* Receive callback registered with the PTL for output forwarded to us
 * by our host or another server - see PMIx_server_init. */
PMIX_EXPORT void pmix_server_iof_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                                         pmix_buffer_t *buf, void *cbdata);

/* Discard what a departed peer (or namespace) left behind: its event and
 * IOF registrations, cached notifications naming it, and the direct-modex
 * trackers waiting on data it will now never publish.
 *
 * "status" is what those waiters are told, and it is the caller's to say
 * because only the caller knows what happened.  A peer that FINALIZED took
 * its data with it and nothing was lost - the answer is that the key is not
 * there.  A peer whose connection dropped is the case LOST_CONNECTION
 * describes. */
PMIX_EXPORT void pmix_server_purge_events(pmix_peer_t *peer, pmix_proc_t *proc,
                                          pmix_status_t status);

/* Record that the server itself has registered for the given event
 * codes. Only system (environmental) codes are tracked as those are the
 * only ones we ask our host to forward. The codes our host is not
 * already forwarding are sorted to the front of the array - the array is
 * treated as an unordered set everywhere else, so the reordering is
 * harmless - and their number is returned. A return of zero means every
 * requested code is already being forwarded and the host need not be
 * called. */
PMIX_EXPORT size_t pmix_server_activate_events(pmix_status_t *codes, size_t ncodes);

/* Release a registration the server itself made for the given event
 * codes. Any code that is left without a registrant - neither the server
 * nor any local client - is dropped, and our host is told to stop
 * forwarding it. Also used to undo a pmix_server_activate_events call
 * whose host up-call was rejected. */
PMIX_EXPORT void pmix_server_deactivate_events(pmix_status_t *codes, size_t ncodes);

/* If no registrant remains for the code tracked by this object - neither
 * a local client nor the server itself - then remove it from the server's
 * event registration store, tell our host to stop forwarding the code if
 * we had asked it to start, and release it. Returns true if the object
 * was released. */
PMIX_EXPORT bool pmix_server_prune_reginfo(pmix_regevents_info_t *reginfo);

/* Handle the departure of a cleanly-finalized local client peer whose
 * socket has dropped: decrement the rank's live-process count and leave
 * the peer in place as an inert finalized "tombstone" at its existing
 * clients slot (info->peerid unchanged, slot not nulled), to be reclaimed
 * at the next reconnect for the rank or at namespace deregistration. Only
 * a stranded peer that a newer connection has already displaced
 * (info->peerid no longer names it) is freed here. Deferring the free and
 * never moving a live peerid keeps concurrent spawn/connect/disconnect
 * collectives and direct-modex gets - which resolve ranks through
 * info->peerid - from racing peer teardown. See
 * docs/how-things-work/init-finalize.rst. */
PMIX_EXPORT void pmix_server_peer_finalized(pmix_peer_t *peer);

PMIX_EXPORT pmix_status_t pmix_server_fabric_register(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                                      pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_fabric_update(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                                    pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_fabric_get_vertex_info(pmix_server_caddy_t *cd,
                                                             pmix_buffer_t *buf,
                                                             pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_fabric_get_device_index(pmix_server_caddy_t *cd,
                                                              pmix_buffer_t *buf,
                                                              pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_device_dists(pmix_server_caddy_t *cd,
                                                   pmix_buffer_t *buf,
                                                   pmix_device_dist_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_refresh_cache(pmix_server_caddy_t *cd,
                                                    pmix_buffer_t *buf,
                                                    pmix_op_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_resblk(pmix_server_caddy_t *cd,
                                             pmix_buffer_t *buf,
                                             pmix_op_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_session_ctrl(pmix_server_caddy_t *cd,
                                                   pmix_buffer_t *buf,
                                                   pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT pmix_status_t pmix_server_resolve_peers(pmix_server_caddy_t *cd,
                                                    pmix_buffer_t *buf,
                                                    pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT void pmix_server_locally_resolve_peers(int sd, short args, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_resolve_node(pmix_server_caddy_t *cd,
                                                   pmix_buffer_t *buf,
                                                   pmix_info_cbfunc_t cbfunc);

PMIX_EXPORT void pmix_server_locally_resolve_node(int sd, short args, void *cbdata);

PMIX_EXPORT pmix_status_t pmix_server_process_grpinfo(size_t ctxid,
                                                      pmix_info_t *pinfo,
                                                      size_t npinfo);

/* An info that is supposed to carry an array of some element type carries
 * whatever the sender actually put there. Everything screened with this
 * arrives either off the wire from a local client or down from the host,
 * so reading the union on the strength of the key alone means
 * dereferencing a pointer the sender chose - or walking past the end of a
 * correctly-tagged array of some other type. Confirm the value is a data
 * array, of the element type we are about to cast it to, and long enough
 * to index. */
PMIX_EXPORT bool pmix_server_valid_darray(const pmix_info_t *info,
                                          pmix_data_type_t type,
                                          size_t minsz);

/* The host-server completion callbacks the switchyard hands to its
 * up-calls. Each one may run in the host's thread context, so each does
 * nothing but thread-shift onto the progress thread, landing in a static
 * handler that packs the reply and queues it. They live beside the
 * command families they answer - operation completions in
 * pmix_server_op_replies.c, host-supplied results in
 * pmix_server_info_replies.c - and server_switchyard is their only
 * caller. */
PMIX_EXPORT void pmix_server_modex_cbfunc(pmix_status_t status, const char *data, size_t ndata,
                                          void *cbdata, pmix_release_cbfunc_t relfn,
                                          void *relcbdata);
PMIX_EXPORT void pmix_server_get_cbfunc(pmix_status_t status, const char *data, size_t ndata,
                                        void *cbdata, pmix_release_cbfunc_t relfn,
                                        void *relcbdata);
PMIX_EXPORT void pmix_server_cnct_cbfunc(pmix_status_t status, void *cbdata);
PMIX_EXPORT void pmix_server_discnct_cbfunc(pmix_status_t status, void *cbdata);
PMIX_EXPORT void pmix_server_spawn_cbfunc(pmix_status_t status, char *nspace, void *cbdata);
PMIX_EXPORT void pmix_server_lookup_cbfunc(pmix_status_t status, pmix_pdata_t pdata[],
                                           size_t ndata, void *cbdata);
PMIX_EXPORT void pmix_server_events_cbfunc(pmix_status_t status, void *cbdata);
PMIX_EXPORT void pmix_server_iofreg_cbfunc(pmix_status_t status, void *cbdata);
PMIX_EXPORT void pmix_server_iofdereg_cbfunc(pmix_status_t status, void *cbdata);

PMIX_EXPORT void pmix_server_alloc_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                          void *cbdata, pmix_release_cbfunc_t release_fn,
                                          void *release_cbdata);
PMIX_EXPORT void pmix_server_query_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                          void *cbdata, pmix_release_cbfunc_t release_fn,
                                          void *release_cbdata);
PMIX_EXPORT void pmix_server_sessctrl_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                             void *cbdata, pmix_release_cbfunc_t release_fn,
                                             void *release_cbdata);
PMIX_EXPORT void pmix_server_jctrl_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                          void *cbdata, pmix_release_cbfunc_t release_fn,
                                          void *release_cbdata);
PMIX_EXPORT void pmix_server_monitor_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                            void *cbdata, pmix_release_cbfunc_t release_fn,
                                            void *release_cbdata);
PMIX_EXPORT void pmix_server_cred_cbfunc(pmix_status_t status, pmix_byte_object_t *credential,
                                         pmix_info_t info[], size_t ninfo, void *cbdata);
PMIX_EXPORT void pmix_server_validate_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                             void *cbdata);
PMIX_EXPORT void pmix_server_fabric_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                                           void *cbdata, pmix_release_cbfunc_t release_fn,
                                           void *release_cbdata);
PMIX_EXPORT void pmix_server_dist_cbfunc(pmix_status_t status, pmix_device_distance_t *dist,
                                         size_t ndist, void *cbdata,
                                         pmix_release_cbfunc_t release_fn, void *release_cbdata);
PMIX_EXPORT void pmix_server_respeers_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                             void *cbdata, pmix_release_cbfunc_t release_fn,
                                             void *release_cbdata);
PMIX_EXPORT void pmix_server_resnodes_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                                             void *cbdata, pmix_release_cbfunc_t release_fn,
                                             void *release_cbdata);

PMIX_EXPORT extern pmix_server_module_t pmix_host_server;
PMIX_EXPORT extern pmix_server_globals_t pmix_server_globals;

static inline pmix_peer_t* pmix_get_peer_object(const pmix_proc_t *proc)
{
    pmix_peer_t *peer;
    int n;

    for (n=0; n < pmix_server_globals.clients.size; n++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == peer) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(proc->nspace, peer->info->pname.nspace) &&
            proc->rank == peer->info->pname.rank) {
            return peer;
        }
    }
    return NULL;
}

// Utilities
PMIX_EXPORT pmix_server_trkr_t *pmix_server_get_tracker(char *id, pmix_proc_t *procs,
                                                        size_t nprocs, pmix_cmd_t type);

PMIX_EXPORT pmix_server_trkr_t *pmix_server_new_tracker(char *id, pmix_proc_t *procs,
                                                        size_t nprocs, pmix_cmd_t type);

/* Record that every local participant of this tracker has had its
 * contribution delivered, so the next one can carry only what changes
 * from here. Call it only once the fence has completed successfully -
 * never from the collection, and never on the host merely accepting the
 * bucket, since the host can still fail the collective after that. See
 * the comment on the definition. */
PMIX_EXPORT void pmix_server_modex_contributed(pmix_server_trkr_t *trk);

/* Count a contributing peer's namespace into a tracker's expected local
 * participant count, if pmix_server_new_tracker could not. Call it for
 * every contribution, immediately before the caddy is appended to
 * local_cbs - it is a no-op for a namespace already counted. */
PMIX_EXPORT void pmix_server_trk_join(pmix_server_trkr_t *trk, pmix_peer_t *peer);

/* The same, keyed on the namespace rather than a contributing peer, for
 * the registration paths: a namespace registering is the other moment at
 * which a tracker built before we had heard of it can be repaired. */
PMIX_EXPORT bool pmix_server_trk_count_nspace(pmix_server_trkr_t *trk,
                                              pmix_namespace_t *nptr);

/* How many of nptr's local procs the participant list names, each
 * counted once however often it is named. Returns false when that
 * cannot be settled yet. Shared by the fence family and groups. */
PMIX_EXPORT bool pmix_server_count_local_participants(const pmix_proc_t *pcs, size_t npcs,
                                                      pmix_namespace_t *nptr,
                                                      size_t *count);
PMIX_EXPORT pmix_status_t pmix_server_build_proc_info(pmix_rank_info_t *info,
                                                      bool include_scope,
                                                      pmix_info_t *xfer,
                                                      bool *have_data);

/* Force this proc's next fence contribution to be cumulative. Call it
 * wherever remote-scope data reaches our datastore for a local proc by
 * some route other than pmix_server_commit. */

/* Tell our local clients that a key has been deleted so their cached
 * copies go too. "skip" is the peer that asked for the deletion and has
 * already applied it, or NULL. */
/** Tell our local clients in this nspace - or all of them, for NULL -
 *  that a datastore segment has been added to. Each peer's own gds
 *  module decides what, if anything, it needs to hear. */
PMIX_EXPORT void pmix_server_notify_gds_update(const char *nspace);

PMIX_EXPORT void pmix_server_notify_deleted(const pmix_proc_t *proc,
                                            pmix_scope_t scope,
                                            const char *key,
                                            pmix_peer_t *skip);

PMIX_EXPORT pmix_status_t pmix_server_collect_data(pmix_server_trkr_t *trk,
                                                   pmix_buffer_t *buf);

PMIX_EXPORT bool pmix_server_trk_complete(pmix_server_trkr_t *trk);

PMIX_EXPORT void pmix_server_set_collective_status(pmix_info_t *info, size_t ninfo,
                                                   pmix_status_t status);

PMIX_EXPORT pmix_status_t pmix_server_get_collective_status(pmix_info_t *info, size_t ninfo);

/* the two lost-connection entry points, one per tracker family - both are
 * called from lost_connection() in the PTL base, and both account only for
 * an abnormal termination (a peer that called PMIx_Finalize remains an
 * expected participant and is ignored) */
PMIX_EXPORT void pmix_server_trk_peer_lost(pmix_peer_t *peer);

PMIX_EXPORT void pmix_server_grp_peer_lost(pmix_peer_t *peer);

PMIX_EXPORT void pmix_server_grp_member_left(const char *grpid, const pmix_proc_t *proc);

/* Re-drive any in-flight group block whose definition was waiting on a
 * namespace that has just been registered - the group-family counterpart
 * of pmix_pending_nspace_requests, called from the same two sites. */
PMIX_EXPORT void pmix_server_grp_check_pending(void);

/* Drop any group invitation still in flight. The invitation list is
 * private to pmix_server_group.c and is not one of the pmix_server_globals
 * lists PMIx_server_finalize destructs, so it needs its own teardown or it
 * carries into the next PMIx_server_init in this process. */
PMIX_EXPORT void pmix_server_grp_finalize(void);

/* The membership of the groups this server's clients belong to - see
 * pmix_server_grpmbr.c. A synchronous construct is recorded in the host's
 * order; an invited group sorted (sort), as clients have always held it */
PMIX_EXPORT pmix_status_t pmix_server_grp_record(const char *grpid, const pmix_proc_t *members,
                                                 size_t nmembers, size_t ctxid, bool notterm,
                                                 bool sort);
PMIX_EXPORT pmix_group_t *pmix_server_grp_find(const char *grpid);
PMIX_EXPORT void pmix_server_grp_drop(const char *grpid);
/* a member left - and the group is dropped once no member is local */
PMIX_EXPORT void pmix_server_grp_remove_member(const char *grpid, const pmix_proc_t *proc);
/* drop the groups no local process is part of any more - once a job is
 * deregistered */
PMIX_EXPORT void pmix_server_grp_sweep(void);
/* Replace each proc naming a recorded group - its ID with
 * PMIX_RANK_WILDCARD for every member, or with a group rank for one - by
 * the members it names, dropping duplicates. The array returned is the
 * caller's, and is a copy even when nothing named a group. A group rank
 * past the membership is PMIX_ERR_NOT_FOUND */
PMIX_EXPORT pmix_status_t pmix_server_grp_expand(const pmix_proc_t *in, size_t nin,
                                                 pmix_proc_t **out, size_t *nout);
/* the same, replacing an array the caller owns (a PMIX_PROC_CREATE'd one) -
 * unless it names no group, when it is left as it came */
PMIX_EXPORT pmix_status_t pmix_server_grp_expand_procs(pmix_proc_t **procs, size_t *nprocs);
/* Is the peer among the (expanded) participants - by its rank, or by its
 * job with PMIX_RANK_WILDCARD, PMIX_RANK_LOCAL_NODE or
 * PMIX_RANK_LOCAL_PEERS? A fence, connect, disconnect, or group construct
 * or destruct is only for its participants, and a client that relies on
 * us for its groups no longer checks for itself. The server, making the
 * request for its host, always is */
PMIX_EXPORT bool pmix_server_grp_is_participant(const pmix_peer_t *peer,
                                                const pmix_proc_t *procs, size_t nprocs);
/* the members of a group other than self, for a leave's range */
PMIX_EXPORT pmix_status_t pmix_server_grp_others(const char *grpid, const pmix_proc_t *self,
                                                 pmix_proc_t **out, size_t *nout);
/* a group event delivered by the host */
PMIX_EXPORT void pmix_server_grp_host_event(pmix_status_t status, const pmix_proc_t *source,
                                            const pmix_info_t *info, size_t ninfo);
PMIX_EXPORT void pmix_server_grpmbr_finalize(void);

/* Does this entry belong somewhere other than a job's own job-level
 * table - a map, a realm array, a programming-model key, or a lone key
 * naming the session, node or app realm?
 *
 * Both paths by which a host revises a running job ask this, because
 * both hand what they collect to PMIX_GDS_ADD_JOB_DATA, which files
 * job-level VALUES. An entry that belongs to another realm cannot be
 * filed there: it would be stored under its own key where no reader of
 * its realm looks, and - since the "has this changed?" test also asks
 * the job-level table - would count as changed on every restatement.
 * See the note on the definition. */
PMIX_EXPORT bool pmix_server_job_update_is_elsewhere(const pmix_info_t *entry);

#endif // PMIX_SERVER_OPS_H
