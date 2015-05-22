/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015      Intel, Inc. All rights reserved
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 */

#ifndef PMIX_SERVER_OPS_H
#define PMIX_SERVER_OPS_H

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/usock/usock.h"

/* define an object for moving a send
 * request into the server's event base */
typedef struct {
    pmix_object_t super;
    int sd;
    pmix_send_message_cbfunc_t cbfunc;
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

typedef struct {
    pmix_list_item_t super;
    pmix_nspace_t *nptr;
    bool contribution_added;      // indicates the number of procs from this nspace have been counted
    pmix_proc_t *proc;            // point to the proc in tracker array
} pmix_range_trkr_t;
PMIX_CLASS_DECLARATION(pmix_range_trkr_t);

/* define a tracker for collective operations */
typedef struct {
    pmix_list_item_t super;
    pmix_cmd_t type;
    pmix_proc_t *pcs;       // retain a copy of the original procs
    volatile bool active;   // flag for waiting for completion
    bool def_complete;      // all procs have been recorded and the trk definition is complete
    pmix_list_t procs;      // list of pmix_range_trkr_t identifying the participating procs
    pmix_list_t locals;     // list of pmix_server_caddy_t identifying the local participants
    uint32_t local_cnt;     // number of local participants
    bool collect_data;
    pmix_modex_cbfunc_t modexcbfunc;
    pmix_op_cbfunc_t op_cbfunc;
} pmix_server_trkr_t;
PMIX_CLASS_DECLARATION(pmix_server_trkr_t);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_server_trkr_t *trk;
} pmix_trkr_caddy_t;
PMIX_CLASS_DECLARATION(pmix_trkr_caddy_t);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    volatile bool active;
    char nspace[PMIX_MAX_NSLEN+1];
    int rank;
    uid_t uid;
    gid_t gid;
    void *server_object;
    int nlocalprocs;
    pmix_info_t *info;
    size_t ninfo;
} pmix_setup_caddy_t;
PMIX_CLASS_DECLARATION(pmix_setup_caddy_t);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    volatile bool active;
    pmix_status_t status;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_proc_t *error_procs;
    size_t error_nprocs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_buffer_t *buf;
} pmix_notify_caddy_t;
PMIX_CLASS_DECLARATION(pmix_notify_caddy_t);

typedef struct {
    pmix_list_t nspaces;
    pmix_pointer_array_t clients;
    pmix_list_t collectives;
} pmix_server_globals_t;

#define PMIX_PEER_CADDY(c, p, t)                \
    do {                                        \
        (c) = PMIX_NEW(pmix_server_caddy_t);    \
        (c)->hdr.tag = (t);                     \
        PMIX_RETAIN((p));                       \
        (c)->peer = (p);                        \
    } while(0);

#define PMIX_SND_CADDY(c, h, s)                                         \
    do {                                                                \
        (c) = PMIX_NEW(pmix_server_caddy_t);                             \
        (void)memcpy(&(c)->hdr, &(h), sizeof(pmix_usock_hdr_t));        \
        PMIX_RETAIN((s));                                                \
        (c)->snd = (s);                                                 \
    } while(0);

#define PMIX_MARK_COLLECTIVE_COMPLETE(t, f)             \
    do {                                                \
        pmix_trkr_caddy_t *cd;                          \
        cd = PMIX_NEW(pmix_trkr_caddy_t);               \
        cd->trk = (t);                                  \
        event_assign(&cd->ev, pmix_globals.evbase, -1,  \
                     EV_WRITE, (f), cd);                \
        event_active(&cd->ev, EV_WRITE, 1);             \
    } while(0);

#define PMIX_SETUP_COLLECTIVE(c, t)             \
    do {                                        \
        (c) = PMIX_NEW(pmix_trkr_caddy_t);      \
        (c)->trk = (t);                         \
    } while(0);

#define PMIX_EXECUTE_COLLECTIVE(c, t, f)                        \
    do {                                                        \
        PMIX_SETUP_COLLECTIVE(c, t);                            \
        event_assign(&((c)->ev), pmix_globals.evbase, -1,       \
                     EV_WRITE, (f), (c));                       \
        event_active(&((c)->ev), EV_WRITE, 1);                  \
    } while(0);


bool pmix_server_trk_update(pmix_server_trkr_t *trk);

pmix_status_t pmix_server_authenticate(int sd, int *out_rank,
                                       pmix_peer_t **peer,
                                       pmix_buffer_t **reply);

pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_op_cbfunc_t cbfunc, void *cbdata);

pmix_status_t pmix_server_commit(pmix_peer_t *peer, pmix_buffer_t *buf);

pmix_status_t pmix_server_fence(pmix_server_caddy_t *cd,
                                pmix_buffer_t *buf,
                      pmix_modex_cbfunc_t modexcbfunc,
                      pmix_op_cbfunc_t opcbfunc);

pmix_status_t pmix_server_get(pmix_buffer_t *buf,
                              pmix_modex_cbfunc_t cbfunc,
                              void *cbdata);

pmix_status_t pmix_server_publish(pmix_peer_t *peer,
                                  pmix_buffer_t *buf,
                                  pmix_op_cbfunc_t cbfunc,
                                  void *cbdata);

pmix_status_t pmix_server_lookup(pmix_buffer_t *buf,
                                 pmix_lookup_cbfunc_t cbfunc,
                                 void *cbdata);

pmix_status_t pmix_server_unpublish(pmix_buffer_t *buf,
                                    pmix_op_cbfunc_t cbfunc,
                                    void *cbdata);

pmix_status_t pmix_server_spawn(pmix_buffer_t *buf,
                                pmix_spawn_cbfunc_t cbfunc,
                                void *cbdata);

pmix_status_t pmix_server_connect(pmix_server_caddy_t *cd,
                                  pmix_buffer_t *buf, bool disconnect,
                                  pmix_op_cbfunc_t cbfunc);

void pmix_pack_proc_map(pmix_buffer_t *buf,
                        char **nodes, char **procs);
pmix_status_t pmix_regex_parse_nodes(const char *regexp, char ***names);
pmix_status_t pmix_regex_parse_procs(const char *regexp, char ***procs);


extern pmix_server_module_t pmix_host_server;
extern pmix_server_globals_t pmix_server_globals;

#endif // PMIX_SERVER_OPS_H
