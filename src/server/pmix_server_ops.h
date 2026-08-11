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
    bool xoff;      // IOF flow control: suspend (true) or resume (false)
    pmix_byte_object_t *bo;
    size_t nbo;
    pmix_op_cbfunc_t opcbfunc;
    pmix_dmodex_response_fn_t cbfunc;
    pmix_setup_application_cbfunc_t setupcbfunc;
    pmix_lookup_cbfunc_t lkcbfunc;
    pmix_spawn_cbfunc_t spcbfunc;
    void *cbdata;
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
    pmix_list_t collectives;      // list of active pmix_server_trkr_t
    pmix_list_t remote_pnd; // list of pmix_dmdx_remote_t awaiting arrival of data fror servicing
                            // remote req's
    pmix_list_t local_reqs;     // list of pmix_dmdx_local_t awaiting arrival of data from local neighbours
    pmix_list_t gdata;  // cache of data given to me for passing to all clients
    char **genvars;     // argv array of envars given to me for passing to all clients
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

#define PMIX_GDS_CADDY(c, p, t)              \
    do {                                     \
        (c) = PMIX_NEW(pmix_server_caddy_t); \
        (c)->hdr.tag = (t);                  \
        PMIX_RETAIN((p));                    \
        (c)->peer = (p);                     \
    } while (0)

/* The caddy carries the tracker across an async hop onto the progress
 * thread, so it takes a reference for the duration - anything else that
 * runs in between (a departing participant completing the collective, for
 * one) would otherwise release the tracker out from under the handler.
 * The caddy's destructor gives the reference back. Note the reference
 * keeps the object alive; it does NOT keep it on the collectives list, so
 * a handler must still check that it is there before acting on it. */
#define PMIX_SETUP_COLLECTIVE(c, t)        \
    do {                                   \
        (c) = PMIX_NEW(pmix_trkr_caddy_t); \
        PMIX_RETAIN((t));                  \
        (c)->trk = (t);                    \
    } while (0)

#define PMIX_EXECUTE_COLLECTIVE(c, t, f)                                            \
    do {                                                                            \
        PMIX_SETUP_COLLECTIVE(c, t);                                                \
        pmix_event_assign(&((c)->ev), pmix_globals.evbase, -1, EV_WRITE, (f), (c)); \
        pmix_event_active(&((c)->ev), EV_WRITE, 1);                                 \
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
PMIX_EXPORT void pmix_server_spawn_parser(pmix_peer_t *peer,
                                          pmix_iof_channel_t *channels,
                                          pmix_iof_flags_t *flags,
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

PMIX_EXPORT pmix_status_t pmix_server_event_recvd_from_client(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                              pmix_op_cbfunc_t cbfunc,
                                                              void *cbdata);
PMIX_EXPORT void pmix_server_execute_collective(int sd, short args, void *cbdata);

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

PMIX_EXPORT void pmix_server_purge_events(pmix_peer_t *peer, pmix_proc_t *proc);

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

PMIX_EXPORT pmix_status_t pmix_server_collect_data(pmix_server_trkr_t *trk,
                                                   pmix_buffer_t *buf);

PMIX_EXPORT bool pmix_server_trk_complete(pmix_server_trkr_t *trk);

PMIX_EXPORT void pmix_server_set_collective_status(pmix_info_t *info, size_t ninfo,
                                                   pmix_status_t status);

PMIX_EXPORT pmix_status_t pmix_server_get_collective_status(pmix_info_t *info, size_t ninfo);

/* the two lost-connection entry points, one per tracker family - both are
 * called from lost_connection() in the PTL base */
PMIX_EXPORT void pmix_server_trk_peer_lost(pmix_peer_t *peer);

PMIX_EXPORT void pmix_server_grp_peer_lost(pmix_peer_t *peer);

PMIX_EXPORT void pmix_server_grp_member_left(const char *grpid, const pmix_proc_t *proc);

#endif // PMIX_SERVER_OPS_H
